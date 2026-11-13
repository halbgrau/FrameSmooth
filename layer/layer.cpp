// FrameSmooth - OpenXR API layer providing half-rate rendering + motion-extrapolated frames
// (similar in spirit to Oculus ASW 1.0 / SteamVR Motion Smoothing).
//
// Supports games using Direct3D 11 or Direct3D 12:
//  * The OpenXR session is created on a private D3D11 device owned by this layer.
//  * The game renders into "proxy" textures shared between our device and the game's device
//    (D3D11 or D3D12), synchronized with shared fences.
//  * A dedicated thread drives the real runtime frame loop at the headset refresh rate.
//    The game's xrWaitFrame is throttled to every Nth display frame (N = 1 or 2).
//  * Display frames without a fresh game frame are extrapolated from GPU motion estimation.
//
// Nothing is modified on disk except this layer's own log/settings in %LOCALAPPDATA%\FrameSmooth.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "flow.h"
#include "shared.h"

using Microsoft::WRL::ComPtr;
using fsflow::NowSec;

// ---------------------------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------------------------
static std::mutex g_logMtx;
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    static FILE* f = nullptr;
    if (!f) {
        std::wstring p = fs_shared::DataDir() + L"\\layer.log";
        f = _wfsopen(p.c_str(), L"w", _SH_DENYNO);
        if (!f) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

static bool IsDepthFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_D16_UNORM ||
           f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// ---------------------------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------------------------
struct NextFns {
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR GetD3D11GraphicsRequirementsKHR = nullptr;
};
static NextFns nx;
static XrInstance g_instance = XR_NULL_HANDLE;
static bool g_hookEnabled = false;
static bool g_d3d11Available = false;  // XR_KHR_D3D11_enable enabled on the instance (by the app or by us)
static std::string g_appName;
static fs_shared::Shared* g_shared = nullptr;

static bool AppRunning() {
    static std::atomic<double> last{-10.0};
    static std::atomic<bool> val{true};
    double now = NowSec();
    if (now - last.load() > 1.0) {
        last = now;
        val = fs_shared::DashboardRunning();
    }
    return val;
}

static fs_shared::Settings ReadSettings() {
    fs_shared::Settings st = g_shared ? g_shared->settings : fs_shared::LoadSettings();
    if (!AppRunning()) {  // FrameSmooth app was closed: behave like plain pass-through
        st.rateMode = fs_shared::Rate_Native;
        st.synthMode = fs_shared::Synth_Rotation;
        st.debugTint = 0;
    }
    return st;
}

static void PublishStatus(int active, int divisor, float hz, float fps, float synth, const char* reason) {
    if (!g_shared) return;
    auto& st = g_shared->status;
    st.active = active;
    st.divisor = divisor;
    st.displayHz = hz;
    st.appFps = fps;
    st.synthPerSec = synth;
    strncpy_s(st.app, g_appName.c_str(), _TRUNCATE);
    strncpy_s(st.reason, reason ? reason : "", _TRUNCATE);
    InterlockedExchange64(&st.heartbeatMs, (LONG64)GetTickCount64());
}

// ---------------------------------------------------------------------------------------------
// Session data
// ---------------------------------------------------------------------------------------------
enum class Api { D3D11, D3D12 };

struct ProxySwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;  // runtime swapchain; also used as the "mirror" for quad etc. layers
    XrSwapchainCreateInfo ci{};
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool isDepth = false, isStatic = false, noIngest = false;
    std::atomic<bool> dead{false};
    uint32_t count = 0;
    std::vector<ComPtr<ID3D11Texture2D>> app11;  // game side (D3D11)
    std::vector<ComPtr<ID3D12Resource>> app12;   // game side (D3D12)
    std::vector<ComPtr<ID3D11Texture2D>> ourTex; // our side
    std::unique_ptr<std::atomic<uint64_t>[]> readDone;
    std::vector<ID3D11Texture2D*> mirror;

    std::mutex m;
    uint32_t nextAcquire = 0;
    std::deque<uint32_t> acquired, waited;
    int32_t lastReleased = -1;
    uint64_t lastReleaseFence = 0;

    bool mirrorFilled = false;
    ComPtr<ID3D11Texture2D> cur;
    ComPtr<ID3D11ShaderResourceView> curSrv;
    uint32_t curW = 0, curH = 0;
};

struct ScUse {
    std::shared_ptr<ProxySwapchain> sc;
    int32_t idx = -1;
    uint64_t fence = 0;
    bool proj = false, mirror = false;
};

struct LayerRec {
    XrStructureType type{};
    std::vector<uint8_t> data;
    std::vector<XrCompositionLayerProjectionView> views;
};

struct Submission {
    uint64_t seq = 0;
    XrTime displayTime = 0;
    XrEnvironmentBlendMode blend{};
    std::vector<LayerRec> layers;
    std::vector<ScUse> uses;
    uint64_t maxFence = 0;
};

struct ViewState {
    XrSwapchain out = XR_NULL_HANDLE;
    uint32_t outW = 0, outH = 0;
    int64_t outFmt = 0;
    std::vector<ID3D11Texture2D*> outImgs;
    std::vector<ComPtr<ID3D11RenderTargetView>> outRtv;
    bool outValid = false;
    std::shared_ptr<ProxySwapchain> sc;
    XrRect2Di rect{};
    uint32_t slice = 0;
    fsflow::View fv;

    fsflow::Source Src() const {
        fsflow::Source s;
        s.srv = sc ? sc->curSrv.Get() : nullptr;
        s.texW = sc ? sc->curW : 0;
        s.texH = sc ? sc->curH : 0;
        s.x = rect.offset.x;
        s.y = rect.offset.y;
        s.w = uint32_t(rect.extent.width);
        s.h = uint32_t(rect.extent.height);
        s.slice = slice;
        return s;
    }
};

struct Session {
    XrSession handle = XR_NULL_HANDLE;
    Api api = Api::D3D11;

    // game side
    ComPtr<ID3D11Device1> app11;
    ComPtr<ID3D11Device5> app11_5;
    ComPtr<ID3D11DeviceContext> appCtx;
    ComPtr<ID3D11DeviceContext4> appCtx4;
    ComPtr<ID3D12Device> app12;
    ComPtr<ID3D12CommandQueue> appQueue;

    // our side
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    fsflow::Engine eng;

    // game -> us ("image released") and us -> game ("finished reading")
    ComPtr<ID3D11Fence> appFenceOurs;
    ComPtr<ID3D11Fence> appFence11;
    ComPtr<ID3D12Fence> appFence12;
    std::mutex appFenceMtx;
    uint64_t appFenceValue = 0;
    ComPtr<ID3D11Fence> backFenceOurs;
    ComPtr<ID3D11Fence> backFence11;
    ComPtr<ID3D12Fence> backFence12;
    uint64_t backValue = 0;

    std::mutex devMtx;
    std::mutex scMtx;
    std::unordered_map<XrSwapchain, std::shared_ptr<ProxySwapchain>> scs;

    std::thread thr;
    std::atomic<bool> running{false};

    std::mutex relMtx;
    std::condition_variable relCv;
    struct Rel {
        uint64_t seq = 0;
        XrTime target = 0;
        XrDuration period = 0;
        bool shouldRender = true;
        double at = 0, freshFor = 0;
    } rel;
    uint64_t takenSeq = 0;
    XrTime taken[8]{};
    int takenPos = 0;

    std::mutex subMtx;
    std::condition_variable subCv;
    std::deque<std::shared_ptr<Submission>> pending;
    uint64_t subSeq = 0;

    std::shared_ptr<Submission> current;
    XrTime prevTime = 0;
    std::vector<std::vector<ViewState>> views;
    fsflow::Pacer pacer;
    uint64_t iter = 0, nextRelease = 0;
    double statT0 = 0;
    int statReal = 0, statSynth = 0;
    float lastHz = 0;

    std::shared_ptr<ProxySwapchain> Find(XrSwapchain h) {
        std::lock_guard<std::mutex> lk(scMtx);
        auto it = scs.find(h);
        return it == scs.end() ? nullptr : it->second;
    }
};

static Session* g_sess = nullptr;  // intentionally leaked on process exit
static Session* Active(XrSession s) { return (g_sess && g_sess->handle == s) ? g_sess : nullptr; }

// ---------------------------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------------------------
static bool CreateOurDevice(Session& s, IDXGIAdapter* adapter, std::string& reason) {
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2,
                                 D3D11_SDK_VERSION, &s.dev, nullptr, &s.ctx))) {
        reason = "Could not create D3D11 device";
        return false;
    }
    if (FAILED(s.dev.As(&s.dev1)) || FAILED(s.dev.As(&s.dev5)) || FAILED(s.ctx.As(&s.ctx4))) {
        reason = "D3D11.4 not available";
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(s.ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    if (!s.eng.Init(s.dev.Get(), s.ctx.Get(), reason)) return false;
    // back fence (ours, shared to the game)
    if (FAILED(s.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.backFenceOurs)))) {
        reason = "Shared fences unsupported";
        return false;
    }
    return true;
}

static bool InitD3D11(Session& s, ID3D11Device* appDevice, std::string& reason) {
    s.api = Api::D3D11;
    if (FAILED(appDevice->QueryInterface(IID_PPV_ARGS(&s.app11))) || FAILED(appDevice->QueryInterface(IID_PPV_ARGS(&s.app11_5)))) {
        reason = "D3D11.4 not available";
        return false;
    }
    appDevice->GetImmediateContext(&s.appCtx);
    if (FAILED(s.appCtx.As(&s.appCtx4))) { reason = "D3D11.4 context not available"; return false; }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(appDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev))) || FAILED(dxgiDev->GetAdapter(&adapter))) {
        reason = "Could not get GPU adapter";
        return false;
    }
    if (!CreateOurDevice(s, adapter.Get(), reason)) return false;
    HANDLE h = nullptr;
    bool ok = SUCCEEDED(s.app11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.appFence11))) &&
              SUCCEEDED(s.appFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
              SUCCEEDED(s.dev5->OpenSharedFence(h, IID_PPV_ARGS(&s.appFenceOurs)));
    if (h) CloseHandle(h);
    h = nullptr;
    ok = ok && SUCCEEDED(s.backFenceOurs->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
         SUCCEEDED(s.app11_5->OpenSharedFence(h, IID_PPV_ARGS(&s.backFence11)));
    if (h) CloseHandle(h);
    if (!ok) reason = "Shared fences unsupported";
    return ok;
}

static bool InitD3D12(Session& s, ID3D12Device* dev12, ID3D12CommandQueue* queue, std::string& reason) {
    s.api = Api::D3D12;
    s.app12 = dev12;
    s.appQueue = queue;
    LUID luid = dev12->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        reason = "Could not find GPU adapter";
        return false;
    }
    if (!CreateOurDevice(s, adapter.Get(), reason)) return false;
    HANDLE h = nullptr;
    bool ok = SUCCEEDED(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.appFence12))) &&
              SUCCEEDED(dev12->CreateSharedHandle(s.appFence12.Get(), nullptr, GENERIC_ALL, nullptr, &h)) &&
              SUCCEEDED(s.dev5->OpenSharedFence(h, IID_PPV_ARGS(&s.appFenceOurs)));
    if (h) CloseHandle(h);
    h = nullptr;
    ok = ok && SUCCEEDED(s.backFenceOurs->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
         SUCCEEDED(dev12->OpenSharedHandle(h, IID_PPV_ARGS(&s.backFence12)));
    if (h) CloseHandle(h);
    if (!ok) reason = "Shared fences unsupported (D3D12)";
    return ok;
}

// Transition freshly opened D3D12 resources into the state OpenXR promises (RENDER_TARGET / DEPTH_WRITE).
static void D3D12InitialTransition(Session& s, const std::vector<ComPtr<ID3D12Resource>>& res, D3D12_RESOURCE_STATES to) {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(s.app12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(s.app12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(s.app12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return;
    std::vector<D3D12_RESOURCE_BARRIER> b;
    for (auto& r : res) {
        D3D12_RESOURCE_BARRIER x{};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r.Get();
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        x.Transition.StateAfter = to;
        b.push_back(x);
    }
    list->ResourceBarrier(UINT(b.size()), b.data());
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    s.appQueue->ExecuteCommandLists(1, lists);
    s.appQueue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 2000);
    CloseHandle(ev);
}

// ---------------------------------------------------------------------------------------------
// Frame thread
// ---------------------------------------------------------------------------------------------
static void EnsureCur(Session& s, ProxySwapchain& sc) {
    if (sc.cur) return;
    D3D11_TEXTURE2D_DESC d;
    sc.ourTex[0]->GetDesc(&d);
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(s.dev->CreateTexture2D(&d, nullptr, &sc.cur))) { Log("cur texture failed"); return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = sc.fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = d.ArraySize;
    if (FAILED(s.dev->CreateShaderResourceView(sc.cur.Get(), &sd, &sc.curSrv))) Log("cur SRV failed (fmt %d)", sc.fmt);
    sc.curW = d.Width;
    sc.curH = d.Height;
}

static void CopyToCur(Session& s, ProxySwapchain& sc, ID3D11Texture2D* src) {
    EnsureCur(s, sc);
    if (!sc.cur) return;
    uint32_t slices = sc.ci.arraySize * sc.ci.faceCount;
    for (uint32_t sl = 0; sl < slices; sl++) {
        UINT dst = D3D11CalcSubresource(0, sl, 1);
        UINT srcSub = D3D11CalcSubresource(0, sl, sc.ci.mipCount);
        if (sc.ci.sampleCount > 1) s.ctx->ResolveSubresource(sc.cur.Get(), dst, src, srcSub, sc.fmt);
        else s.ctx->CopySubresourceRegion(sc.cur.Get(), dst, 0, 0, 0, src, srcSub, nullptr);
    }
}

static void CopyMirror(Session& s, ProxySwapchain& sc, ID3D11Texture2D* src) {
    if (sc.isStatic && sc.mirrorFilled) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(nx.AcquireSwapchainImage(sc.handle, &ai, &idx)) || idx >= sc.mirror.size()) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    nx.WaitSwapchainImage(sc.handle, &wi);
    s.ctx->CopyResource(sc.mirror[idx], src);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    nx.ReleaseSwapchainImage(sc.handle, &ri);
    sc.mirrorFilled = true;
}

static bool EnsureOut(Session& s, ViewState& v, uint32_t w, uint32_t h, int64_t fmt) {
    if (v.out != XR_NULL_HANDLE && v.outW == w && v.outH == h && v.outFmt == fmt) return true;
    if (v.out != XR_NULL_HANDLE) nx.DestroySwapchain(v.out);
    v.out = XR_NULL_HANDLE;
    v.outImgs.clear();
    v.outRtv.clear();
    v.outValid = false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = nx.CreateSwapchain(s.handle, &ci, &v.out);
    if (XR_FAILED(r)) { Log("output swapchain failed %d", r); v.out = XR_NULL_HANDLE; return false; }
    uint32_t n = 0;
    nx.EnumerateSwapchainImages(v.out, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    nx.EnumerateSwapchainImages(v.out, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& i : imgs) {
        v.outImgs.push_back(i.texture);
        D3D11_TEXTURE2D_DESC td;
        i.texture->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = (DXGI_FORMAT)fmt;
        if (td.ArraySize > 1) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.ArraySize = 1;
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(s.dev->CreateRenderTargetView(i.texture, &rd, &rtv))) Log("RTV failed");
        v.outRtv.push_back(rtv);
    }
    v.outW = w;
    v.outH = h;
    v.outFmt = fmt;
    return true;
}

static void ProduceOut(Session& s, ViewState& v, bool warp, float sf, bool tint) {
    ProxySwapchain& sc = *v.sc;
    if (!sc.cur || !sc.curSrv) return;
    uint32_t w = v.rect.extent.width, h = v.rect.extent.height;
    if (!EnsureOut(s, v, w, h, sc.ci.format)) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(nx.AcquireSwapchainImage(v.out, &ai, &idx)) || idx >= v.outImgs.size()) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    nx.WaitSwapchainImage(v.out, &wi);
    if (!warp) {
        D3D11_BOX box{UINT(v.rect.offset.x), UINT(v.rect.offset.y), 0, UINT(v.rect.offset.x) + w, UINT(v.rect.offset.y) + h, 1};
        s.ctx->CopySubresourceRegion(v.outImgs[idx], 0, 0, 0, 0, sc.cur.Get(), D3D11CalcSubresource(0, v.slice, 1), &box);
    } else {
        s.eng.Warp(v.fv, v.Src(), v.outRtv[idx].Get(), sf, tint);
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    nx.ReleaseSwapchainImage(v.out, &ri);
    v.outValid = true;
}

static void Ingest(Session& s, const std::shared_ptr<Submission>& sub, const fs_shared::Settings& st) {
    s.ctx4->Wait(s.appFenceOurs.Get(), sub->maxFence);
    for (auto& u : sub->uses) {
        if (u.sc->dead || u.sc->noIngest || u.idx < 0) continue;
        ID3D11Texture2D* src = u.sc->ourTex[u.idx].Get();
        if (u.proj) CopyToCur(s, *u.sc, src);
        if (u.mirror) CopyMirror(s, *u.sc, src);
    }
    s.backValue++;
    s.ctx4->Signal(s.backFenceOurs.Get(), s.backValue);
    for (auto& u : sub->uses)
        if (u.idx >= 0 && !u.sc->noIngest) u.sc->readDone[u.idx] = s.backValue;

    bool motion = st.synthMode == fs_shared::Synth_Motion;
    size_t pi = 0;
    for (auto& L : sub->layers) {
        if (L.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(L.data.data());
        if (s.views.size() <= pi) s.views.resize(pi + 1);
        auto& vv = s.views[pi];
        if (vv.size() != L.views.size()) vv.resize(L.views.size());
        for (size_t vi = 0; vi < L.views.size(); vi++) {
            ViewState& v = vv[vi];
            const auto& pv = L.views[vi];
            std::shared_ptr<ProxySwapchain> sc;
            for (auto& u : sub->uses)
                if (u.sc->handle == pv.subImage.swapchain) sc = u.sc;
            if (!sc) continue;
            v.sc = sc;
            v.rect = pv.subImage.imageRect;
            v.slice = pv.subImage.imageArrayIndex;
            float R[3][3];
            fsflow::QuatToMat(pv.pose.orientation.x, pv.pose.orientation.y, pv.pose.orientation.z, pv.pose.orientation.w, R);
            float fov[4] = {tanf(pv.fov.angleLeft), tanf(pv.fov.angleRight), tanf(pv.fov.angleUp), tanf(pv.fov.angleDown)};
            s.eng.Analyze(v.fv, v.Src(), R, fov, (uint64_t)proj->space, motion);
        }
        pi++;
    }
    s.prevTime = s.current ? s.current->displayTime : 0;
    s.current = sub;
    s.ctx->Flush();
}

struct FrameStorage {
    std::vector<XrCompositionLayerProjection> projs;
    std::vector<std::vector<XrCompositionLayerProjectionView>> views;
    std::vector<const XrCompositionLayerBaseHeader*> hdrs;
};

static XrSwapchain LayerSwapchain(XrStructureType t, const void* L) {
    switch (t) {
        case XR_TYPE_COMPOSITION_LAYER_QUAD: return static_cast<const XrCompositionLayerQuad*>(L)->subImage.swapchain;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return static_cast<const XrCompositionLayerCylinderKHR*>(L)->subImage.swapchain;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return static_cast<const XrCompositionLayerEquirectKHR*>(L)->subImage.swapchain;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return static_cast<const XrCompositionLayerEquirect2KHR*>(L)->subImage.swapchain;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return static_cast<const XrCompositionLayerCubeKHR*>(L)->swapchain;
        default: return XR_NULL_HANDLE;
    }
}

static int ProduceAndBuild(Session& s, XrTime T, XrDuration Pn, bool newFrame, const fs_shared::Settings& st, FrameStorage& fsx) {
    int synthesized = 0;
    auto& cur = s.current;
    if (!cur) return 0;
    bool motion = st.synthMode == fs_shared::Synth_Motion;
    bool onTime = newFrame && std::llabs(cur->displayTime - T) < Pn / 2;
    float sf = 0;
    if (s.prevTime > 0 && cur->displayTime > s.prevTime)
        sf = float(double(T - cur->displayTime) / double(cur->displayTime - s.prevTime));
    sf = std::clamp(sf, 0.0f, 3.0f);

    fsx.projs.reserve(cur->layers.size());
    fsx.views.reserve(cur->layers.size());
    size_t pi = 0;
    for (auto& L : cur->layers) {
        if (L.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            if (pi >= s.views.size()) break;
            auto& vv = s.views[pi++];
            bool ok = vv.size() == L.views.size();
            bool warped = false;
            for (size_t vi = 0; ok && vi < vv.size(); vi++) {
                ViewState& v = vv[vi];
                if (!v.sc || v.sc->dead) { ok = false; break; }
                if (onTime) ProduceOut(s, v, false, 0, false);
                else if (motion && v.fv.flowValid && sf > 0.01f) { ProduceOut(s, v, true, sf, st.debugTint != 0); warped = true; }
                else if (newFrame) ProduceOut(s, v, false, 0, false);
                ok = v.outValid;
            }
            if (!ok) continue;
            if (warped) synthesized = 1;
            XrCompositionLayerProjection p = *reinterpret_cast<const XrCompositionLayerProjection*>(L.data.data());
            fsx.views.emplace_back(L.views);
            auto& pvs = fsx.views.back();
            for (size_t vi = 0; vi < pvs.size(); vi++) {
                pvs[vi].next = nullptr;
                pvs[vi].subImage.swapchain = vv[vi].out;
                pvs[vi].subImage.imageRect = {{0, 0}, {int32_t(vv[vi].outW), int32_t(vv[vi].outH)}};
                pvs[vi].subImage.imageArrayIndex = 0;
            }
            p.next = nullptr;
            p.views = pvs.data();
            p.viewCount = uint32_t(pvs.size());
            fsx.projs.push_back(p);
            fsx.hdrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fsx.projs.back()));
        } else {
            XrSwapchain h = LayerSwapchain(L.type, L.data.data());
            bool alive = h != XR_NULL_HANDLE;
            for (auto& u : cur->uses)
                if (u.sc->handle == h && (u.sc->dead || !u.sc->mirrorFilled)) alive = false;
            if (alive) fsx.hdrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(L.data.data()));
        }
    }
    return synthesized;
}

static std::shared_ptr<Submission> PickReady(Session& s, XrTime T, XrDuration Pn) {
    uint64_t done = s.appFenceOurs->GetCompletedValue();
    int bestI = -1;
    for (int i = 0; i < int(s.pending.size()); i++) {
        auto& p = s.pending[i];
        if (p->displayTime <= T + Pn / 2 && p->maxFence <= done) bestI = i;
    }
    if (bestI < 0) return nullptr;
    auto r = s.pending[bestI];
    s.pending.erase(s.pending.begin(), s.pending.begin() + bestI + 1);
    return r;
}

static bool Expected(Session& s, XrTime T, XrDuration Pn) {
    std::lock_guard<std::mutex> lk(s.relMtx);
    for (XrTime t : s.taken)
        if (t != 0 && std::llabs(t - T) < Pn / 2) return !s.current || s.current->displayTime < t;
    return false;
}

static void PostRelease(Session& s, XrTime target, XrDuration period, bool shouldRender, double now, double freshFor) {
    {
        std::lock_guard<std::mutex> lk(s.relMtx);
        s.rel.seq++;
        s.rel.target = target;
        s.rel.period = period;
        s.rel.shouldRender = shouldRender;
        s.rel.at = now;
        s.rel.freshFor = freshFor;
    }
    s.relCv.notify_all();
}

static void FrameThread(Session* sp) {
    Session& s = *sp;
    Log("frame thread started");
    s.statT0 = NowSec();
    const char* apiName = s.api == Api::D3D12 ? "Active (OpenXR, D3D12)" : "Active (OpenXR, D3D11)";
    while (s.running) {
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrResult r = nx.WaitFrame(s.handle, &wi, &fs);
        if (XR_FAILED(r)) { Log("xrWaitFrame %d", r); Sleep(5); continue; }
        double wake = NowSec();
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        r = nx.BeginFrame(s.handle, &bi);
        if (XR_FAILED(r)) { Log("xrBeginFrame %d", r); continue; }

        const XrTime T = fs.predictedDisplayTime;
        const XrDuration Pn = std::max<XrDuration>(fs.predictedDisplayPeriod, 1000000);
        const double P = double(Pn) * 1e-9;
        s.lastHz = float(1.0 / P);
        fs_shared::Settings st = ReadSettings();
        if (s.pacer.Update(st.rateMode, wake)) {
            Log("divisor -> %d", s.pacer.divisor);
            s.nextRelease = s.iter;
        }
        const int D = s.pacer.divisor;
        if (s.iter >= s.nextRelease) {
            PostRelease(s, T + D * Pn, D * Pn, fs.shouldRender == XR_TRUE, wake, (D - 0.5) * P);
            s.nextRelease = s.iter + D;
        }

        bool expected = Expected(s, T, Pn);
        const double deadline = wake + 0.5 * P;
        std::shared_ptr<Submission> pick;
        {
            std::unique_lock<std::mutex> lk(s.subMtx);
            for (;;) {
                auto p = PickReady(s, T, Pn);
                if (p) pick = p;
                bool onTime = pick && std::llabs(pick->displayTime - T) < Pn / 2;
                if (onTime || !expected || !s.running || NowSec() >= deadline) break;
                s.subCv.wait_for(lk, std::chrono::microseconds(500));
            }
            if (D == 2 && s.nextRelease == s.iter + 1) {
                bool ready = false;
                uint64_t done = s.appFenceOurs->GetCompletedValue();
                for (auto& p : s.pending)
                    if (std::llabs(p->displayTime - (T + Pn)) < Pn / 2 && p->maxFence <= done) ready = true;
                s.pacer.Early(ready);
            }
        }
        if (expected) s.pacer.Expected(pick && std::llabs(pick->displayTime - T) < Pn / 2);

        FrameStorage fsx;
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = T;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        {
            std::lock_guard<std::mutex> dl(s.devMtx);
            bool newFrame = false;
            if (pick && (!s.current || pick->seq > s.current->seq)) {
                Ingest(s, pick, st);
                newFrame = true;
                s.statReal++;
            }
            if (s.current) {
                ei.environmentBlendMode = s.current->blend;
                if (fs.shouldRender) s.statSynth += ProduceAndBuild(s, T, Pn, newFrame, st, fsx);
            }
            ei.layerCount = uint32_t(fsx.hdrs.size());
            ei.layers = fsx.hdrs.empty() ? nullptr : fsx.hdrs.data();
            r = nx.EndFrame(s.handle, &ei);
        }
        if (XR_FAILED(r)) Log("xrEndFrame %d (layers %u)", r, ei.layerCount);

        double now = NowSec();
        if (now - s.statT0 >= 1.0) {
            double dt = now - s.statT0;
            PublishStatus(1, D, s.lastHz, float(s.statReal / dt), float(s.statSynth / dt), apiName);
            s.statReal = s.statSynth = 0;
            s.statT0 = now;
        }
        s.iter++;
    }
    Log("frame thread stopped");
}

static void StopThread(Session& s) {
    s.running = false;
    s.relCv.notify_all();
    s.subCv.notify_all();
    if (s.thr.joinable()) s.thr.join();
}

static void DestroyOutputs(Session& s) {
    std::lock_guard<std::mutex> dl(s.devMtx);
    for (auto& vv : s.views)
        for (auto& v : vv)
            if (v.out != XR_NULL_HANDLE) nx.DestroySwapchain(v.out);
    s.views.clear();
    s.current.reset();
    s.pending.clear();
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------
static XrResult XRAPI_CALL Hook_CreateSession(XrInstance instance, const XrSessionCreateInfo* ci, XrSession* out) {
    if (!g_shared) g_shared = fs_shared::Open();
    fs_shared::Settings st = ReadSettings();
    XrBaseInStructure* gb = nullptr;
    XrBaseInStructure* gbPrev = reinterpret_cast<XrBaseInStructure*>(const_cast<XrSessionCreateInfo*>(ci));
    for (auto* p = gbPrev; p->next; p = const_cast<XrBaseInStructure*>(p->next)) {
        auto* n = const_cast<XrBaseInStructure*>(p->next);
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || n->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            gb = n;
            gbPrev = p;
            break;
        }
    }

    std::string reason;
    if (!fs_shared::DashboardRunning()) reason = "FrameSmooth app is not running";
    else if (!st.enabled) reason = "Disabled in FrameSmooth settings";
    else if (!gb) reason = "Game uses an unsupported graphics API (Vulkan/OpenGL)";
    else if (!g_d3d11Available || !nx.GetD3D11GraphicsRequirementsKHR) reason = "Runtime has no D3D11 support";

    std::unique_ptr<Session> s;
    if (reason.empty()) {
        s = std::make_unique<Session>();
        bool ok;
        if (gb->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
            ok = InitD3D11(*s, reinterpret_cast<XrGraphicsBindingD3D11KHR*>(gb)->device, reason);
        else
            ok = InitD3D12(*s, reinterpret_cast<XrGraphicsBindingD3D12KHR*>(gb)->device,
                           reinterpret_cast<XrGraphicsBindingD3D12KHR*>(gb)->queue, reason);
        if (!ok) s.reset();
    }
    if (s) {
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        nx.GetD3D11GraphicsRequirementsKHR(instance, ci->systemId, &req);
        XrGraphicsBindingD3D11KHR ours{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        ours.next = gb->next;
        ours.device = s->dev.Get();
        // temporarily splice our binding into the app's chain
        const XrBaseInStructure* saved = gbPrev->next;
        gbPrev->next = reinterpret_cast<const XrBaseInStructure*>(&ours);
        XrResult r = nx.CreateSession(instance, ci, out);
        gbPrev->next = saved;
        if (XR_SUCCEEDED(r)) {
            s->handle = *out;
            g_sess = s.release();
            Log("session created, layer ACTIVE (%s)", g_sess->api == Api::D3D12 ? "D3D12" : "D3D11");
            PublishStatus(1, 1, 0, 0, 0, "Waiting for game to start rendering");
            return r;
        }
        Log("runtime rejected layer device (%d), falling back", r);
        reason = "Runtime rejected layer device";
    }
    Log("layer inactive: %s", reason.c_str());
    PublishStatus(0, 1, 0, 0, 0, reason.c_str());
    return nx.CreateSession(instance, ci, out);
}

static XrResult XRAPI_CALL Hook_DestroySession(XrSession session) {
    if (Session* s = Active(session)) {
        StopThread(*s);
        DestroyOutputs(*s);
        {
            std::lock_guard<std::mutex> lk(s->scMtx);
            s->scs.clear();
        }
        g_sess = nullptr;
        XrResult r;
        {
            std::lock_guard<std::mutex> dl(s->devMtx);
            r = nx.DestroySession(session);
        }
        delete s;
        PublishStatus(0, 1, 0, 0, 0, "Game session ended");
        return r;
    }
    return nx.DestroySession(session);
}

static XrResult XRAPI_CALL Hook_BeginSession(XrSession session, const XrSessionBeginInfo* bi) {
    XrResult r = nx.BeginSession(session, bi);
    Session* s = Active(session);
    if (s && XR_SUCCEEDED(r) && !s->running) {
        s->iter = s->nextRelease = 0;
        s->takenSeq = s->rel.seq;
        memset(s->taken, 0, sizeof(s->taken));
        s->running = true;
        s->thr = std::thread(FrameThread, s);
    }
    return r;
}

static XrResult XRAPI_CALL Hook_EndSession(XrSession session) {
    if (Session* s = Active(session)) {
        StopThread(*s);
        std::lock_guard<std::mutex> dl(s->devMtx);
        return nx.EndSession(session);
    }
    return nx.EndSession(session);
}

static bool CreateProxies(Session& s, ProxySwapchain& sc) {
    const XrSwapchainCreateInfo& ci = sc.ci;
    sc.count = sc.isStatic ? 1 : 3;
    sc.readDone = std::make_unique<std::atomic<uint64_t>[]>(sc.count);
    DXGI_FORMAT tl = fsflow::TypelessFormat(sc.fmt);

    if (sc.isDepth) {
        // Depth is never forwarded (composition depth is stripped): plain textures on the game device.
        sc.noIngest = true;
        for (uint32_t i = 0; i < sc.count; i++) {
            sc.readDone[i] = 0;
            if (s.api == Api::D3D11) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = ci.width; d.Height = ci.height; d.MipLevels = ci.mipCount;
                d.ArraySize = ci.arraySize * ci.faceCount; d.Format = tl; d.SampleDesc.Count = ci.sampleCount;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_DEPTH_STENCIL | (tl != sc.fmt ? D3D11_BIND_SHADER_RESOURCE : 0);
                ComPtr<ID3D11Texture2D> t;
                if (FAILED(s.app11->CreateTexture2D(&d, nullptr, &t))) return false;
                sc.app11.push_back(t);
            } else {
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                D3D12_RESOURCE_DESC d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = ci.width; d.Height = ci.height; d.DepthOrArraySize = UINT16(ci.arraySize * ci.faceCount);
                d.MipLevels = UINT16(ci.mipCount); d.Format = tl; d.SampleDesc.Count = ci.sampleCount;
                d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                ComPtr<ID3D12Resource> r;
                if (FAILED(s.app12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                            nullptr, IID_PPV_ARGS(&r))))
                    return false;
                sc.app12.push_back(r);
            }
        }
        return true;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = ci.width;
    d.Height = ci.height;
    d.MipLevels = ci.mipCount;
    d.ArraySize = ci.arraySize * ci.faceCount;
    d.Format = tl;
    d.SampleDesc.Count = ci.sampleCount;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (ci.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                  ((ci.faceCount == 6) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
    for (uint32_t i = 0; i < sc.count; i++) {
        sc.readDone[i] = 0;
        ComPtr<ID3D11Texture2D> ours;
        ComPtr<IDXGIResource1> res;
        HANDLE h = nullptr;
        if (FAILED(s.dev->CreateTexture2D(&d, nullptr, &ours)) || FAILED(ours.As(&res)) ||
            FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h))) {
            Log("proxy create failed fmt %d", sc.fmt);
            return false;
        }
        bool ok;
        if (s.api == Api::D3D11) {
            ComPtr<ID3D11Texture2D> at;
            ok = SUCCEEDED(s.app11->OpenSharedResource1(h, IID_PPV_ARGS(&at)));
            if (ok) sc.app11.push_back(at);
        } else {
            ComPtr<ID3D12Resource> ar;
            ok = SUCCEEDED(s.app12->OpenSharedHandle(h, IID_PPV_ARGS(&ar)));
            if (ok) sc.app12.push_back(ar);
        }
        CloseHandle(h);
        if (!ok) { Log("proxy open failed fmt %d", sc.fmt); return false; }
        sc.ourTex.push_back(ours);
    }
    if (s.api == Api::D3D12) D3D12InitialTransition(s, sc.app12, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

static XrResult XRAPI_CALL Hook_CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    Session* s = Active(session);
    if (!s) return nx.CreateSwapchain(session, ci, out);
    auto sc = std::make_shared<ProxySwapchain>();
    sc->ci = *ci;
    sc->ci.next = nullptr;
    sc->fmt = DXGI_FORMAT(ci->format);
    sc->isDepth = IsDepthFormat(sc->fmt) || (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    sc->isStatic = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;

    XrSwapchainCreateInfo rci = *ci;
    if (!sc->isDepth) rci.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    XrResult r;
    {
        std::lock_guard<std::mutex> dl(s->devMtx);
        r = nx.CreateSwapchain(session, &rci, out);
        if (XR_FAILED(r)) return r;
        uint32_t n = 0;
        nx.EnumerateSwapchainImages(*out, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        nx.EnumerateSwapchainImages(*out, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (auto& i : imgs) sc->mirror.push_back(i.texture);
    }
    sc->handle = *out;
    if (!CreateProxies(*s, *sc)) {
        std::lock_guard<std::mutex> dl(s->devMtx);
        nx.DestroySwapchain(*out);
        *out = XR_NULL_HANDLE;
        return XR_ERROR_RUNTIME_FAILURE;
    }
    Log("swapchain %p: %ux%u fmt %lld arr %u samples %u%s", (void*)*out, ci->width, ci->height, (long long)ci->format,
        ci->arraySize, ci->sampleCount, sc->isDepth ? " (depth)" : "");
    std::lock_guard<std::mutex> lk(s->scMtx);
    s->scs[*out] = sc;
    return r;
}

static XrResult XRAPI_CALL Hook_DestroySwapchain(XrSwapchain h) {
    if (g_sess) {
        std::shared_ptr<ProxySwapchain> sc;
        {
            std::lock_guard<std::mutex> lk(g_sess->scMtx);
            auto it = g_sess->scs.find(h);
            if (it != g_sess->scs.end()) { sc = it->second; g_sess->scs.erase(it); }
        }
        if (sc) {
            sc->dead = true;
            std::lock_guard<std::mutex> dl(g_sess->devMtx);
            return nx.DestroySwapchain(h);
        }
    }
    return nx.DestroySwapchain(h);
}

static XrResult XRAPI_CALL Hook_EnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t* count,
                                                         XrSwapchainImageBaseHeader* images) {
    auto sc = g_sess ? g_sess->Find(h) : nullptr;
    if (!sc) return nx.EnumerateSwapchainImages(h, cap, count, images);
    *count = sc->count;
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->count) return XR_ERROR_SIZE_INSUFFICIENT;
    if (g_sess->api == Api::D3D11) {
        auto* d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < sc->count; i++) d[i].texture = sc->app11[i].Get();
    } else {
        auto* d = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (uint32_t i = 0; i < sc->count; i++) d[i].texture = sc->app12[i].Get();
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo* ai, uint32_t* index) {
    auto sc = g_sess ? g_sess->Find(h) : nullptr;
    if (!sc) return nx.AcquireSwapchainImage(h, ai, index);
    std::lock_guard<std::mutex> lk(sc->m);
    if (sc->acquired.size() + sc->waited.size() >= sc->count) return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t i = sc->nextAcquire;
    sc->nextAcquire = (i + 1) % sc->count;
    sc->acquired.push_back(i);
    *index = i;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_WaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo* wi) {
    auto sc = g_sess ? g_sess->Find(h) : nullptr;
    if (!sc) return nx.WaitSwapchainImage(h, wi);
    Session& s = *g_sess;
    std::lock_guard<std::mutex> lk(sc->m);
    if (sc->acquired.empty()) return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t i = sc->acquired.front();
    sc->acquired.pop_front();
    sc->waited.push_back(i);
    if (!sc->noIngest) {
        uint64_t v = sc->readDone[i];
        if (v) {  // GPU-side wait until we finished reading this image
            if (s.api == Api::D3D11) s.appCtx4->Wait(s.backFence11.Get(), v);
            else s.appQueue->Wait(s.backFence12.Get(), v);
        }
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo* ri) {
    auto sc = g_sess ? g_sess->Find(h) : nullptr;
    if (!sc) return nx.ReleaseSwapchainImage(h, ri);
    Session& s = *g_sess;
    std::lock_guard<std::mutex> lk(sc->m);
    if (sc->waited.empty()) return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t i = sc->waited.front();
    sc->waited.pop_front();
    uint64_t v;
    {
        std::lock_guard<std::mutex> fl(s.appFenceMtx);
        v = ++s.appFenceValue;
        if (s.api == Api::D3D11) {
            s.appCtx4->Signal(s.appFence11.Get(), v);
            s.appCtx->Flush();
        } else {
            s.appQueue->Signal(s.appFence12.Get(), v);
        }
    }
    sc->lastReleased = int32_t(i);
    sc->lastReleaseFence = v;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_WaitFrame(XrSession session, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    Session* s = Active(session);
    if (!s) return nx.WaitFrame(session, wi, fs);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    std::unique_lock<std::mutex> lk(s->relMtx);
    for (;;) {
        if (!s->running) {
            fs->predictedDisplayTime = s->rel.target;
            fs->predictedDisplayPeriod = s->rel.period;
            fs->shouldRender = XR_FALSE;
            return XR_SUCCESS;
        }
        if (s->rel.seq > s->takenSeq) {
            if (NowSec() - s->rel.at <= s->rel.freshFor) break;
            s->takenSeq = s->rel.seq;
        }
        s->relCv.wait_for(lk, std::chrono::milliseconds(50));
    }
    s->takenSeq = s->rel.seq;
    fs->predictedDisplayTime = s->rel.target;
    fs->predictedDisplayPeriod = s->rel.period;
    fs->shouldRender = s->rel.shouldRender ? XR_TRUE : XR_FALSE;
    s->taken[s->takenPos++ % 8] = s->rel.target;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_BeginFrame(XrSession session, const XrFrameBeginInfo* bi) {
    Session* s = Active(session);
    if (!s) return nx.BeginFrame(session, bi);
    return s->running ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING;
}

static size_t LayerSize(XrStructureType t) {
    switch (t) {
        case XR_TYPE_COMPOSITION_LAYER_QUAD: return sizeof(XrCompositionLayerQuad);
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return sizeof(XrCompositionLayerCylinderKHR);
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return sizeof(XrCompositionLayerEquirectKHR);
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return sizeof(XrCompositionLayerEquirect2KHR);
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return sizeof(XrCompositionLayerCubeKHR);
        default: return 0;
    }
}

static XrResult XRAPI_CALL Hook_EndFrame(XrSession session, const XrFrameEndInfo* info) {
    Session* s = Active(session);
    if (!s) return nx.EndFrame(session, info);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;

    auto sub = std::make_shared<Submission>();
    sub->displayTime = info->displayTime;
    sub->blend = info->environmentBlendMode;

    auto addUse = [&](XrSwapchain h, bool proj) -> bool {
        auto sc = s->Find(h);
        if (!sc) return false;
        int32_t idx;
        uint64_t fence;
        {
            std::lock_guard<std::mutex> lk(sc->m);
            idx = sc->lastReleased;
            fence = sc->lastReleaseFence;
        }
        if (idx < 0) return false;
        ScUse* u = nullptr;
        for (auto& e : sub->uses)
            if (e.sc == sc) u = &e;
        if (!u) {
            sub->uses.push_back(ScUse{sc, idx, fence});
            u = &sub->uses.back();
        }
        (proj ? u->proj : u->mirror) = true;
        sub->maxFence = std::max(sub->maxFence, fence);
        return true;
    };

    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrCompositionLayerBaseHeader* L = info->layers[i];
        if (!L) continue;
        LayerRec rec;
        rec.type = L->type;
        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(L);
            rec.data.resize(sizeof(XrCompositionLayerProjection));
            memcpy(rec.data.data(), p, sizeof(*p));
            reinterpret_cast<XrCompositionLayerProjection*>(rec.data.data())->next = nullptr;
            rec.views.assign(p->views, p->views + p->viewCount);
            bool ok = true;
            for (auto& v : rec.views) {
                v.next = nullptr;  // composition-layer depth is not forwarded
                ok = addUse(v.subImage.swapchain, true) && ok;
            }
            if (ok) sub->layers.push_back(std::move(rec));
        } else if (size_t sz = LayerSize(L->type)) {
            rec.data.resize(sz);
            memcpy(rec.data.data(), L, sz);
            reinterpret_cast<XrCompositionLayerBaseHeader*>(rec.data.data())->next = nullptr;
            if (addUse(LayerSwapchain(L->type, L), false)) sub->layers.push_back(std::move(rec));
        }
    }
    {
        std::lock_guard<std::mutex> lk(s->subMtx);
        sub->seq = ++s->subSeq;
        s->pending.push_back(sub);
        while (s->pending.size() > 4) s->pending.pop_front();
    }
    s->subCv.notify_all();
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL Hook_DestroyInstance(XrInstance instance) {
    XrResult r = nx.DestroyInstance(instance);
    g_instance = XR_NULL_HANDLE;
    return r;
}

static XrResult XRAPI_CALL Hook_GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    if (!nx.GetInstanceProcAddr) return XR_ERROR_HANDLE_INVALID;
    XrResult r = nx.GetInstanceProcAddr(instance, name, fn);
    if (XR_FAILED(r)) return r;
    const std::string n(name);
    if (n == "xrGetInstanceProcAddr") { *fn = (PFN_xrVoidFunction)Hook_GetInstanceProcAddr; return r; }
    if (n == "xrDestroyInstance") { *fn = (PFN_xrVoidFunction)Hook_DestroyInstance; return r; }
    if (!g_hookEnabled) return r;
#define FS_HOOK(x) if (n == "xr" #x) { *fn = (PFN_xrVoidFunction)Hook_##x; return r; }
    FS_HOOK(CreateSession) FS_HOOK(DestroySession) FS_HOOK(BeginSession) FS_HOOK(EndSession)
    FS_HOOK(CreateSwapchain) FS_HOOK(DestroySwapchain) FS_HOOK(EnumerateSwapchainImages)
    FS_HOOK(AcquireSwapchainImage) FS_HOOK(WaitSwapchainImage) FS_HOOK(ReleaseSwapchainImage)
    FS_HOOK(WaitFrame) FS_HOOK(BeginFrame) FS_HOOK(EndFrame)
#undef FS_HOOK
    return r;
}

template <typename T>
static void Resolve(const char* name, T& out) {
    out = nullptr;
    nx.GetInstanceProcAddr(g_instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out));
}

static XrResult XRAPI_CALL Hook_CreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layerInfo,
                                                       XrInstance* instance) {
    if (!layerInfo || !layerInfo->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerCreateInfo chain = *layerInfo;
    chain.nextInfo = layerInfo->nextInfo->next;

    bool d3d11 = false, d3d12 = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        if (strcmp(info->enabledExtensionNames[i], XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) d3d11 = true;
        if (strcmp(info->enabledExtensionNames[i], XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0) d3d12 = true;
    }
    XrResult r;
    g_d3d11Available = d3d11;
    if (d3d12 && !d3d11) {
        // We render through D3D11 internally: also enable the D3D11 extension on the runtime.
        std::vector<const char*> exts(info->enabledExtensionNames, info->enabledExtensionNames + info->enabledExtensionCount);
        exts.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        XrInstanceCreateInfo ci2 = *info;
        ci2.enabledExtensionCount = uint32_t(exts.size());
        ci2.enabledExtensionNames = exts.data();
        r = layerInfo->nextInfo->nextCreateApiLayerInstance(&ci2, &chain, instance);
        if (XR_SUCCEEDED(r)) g_d3d11Available = true;
        else r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chain, instance);
    } else {
        r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chain, instance);
    }
    if (XR_FAILED(r)) return r;

    g_instance = *instance;
    nx = NextFns{};
    nx.GetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;
    Resolve("xrDestroyInstance", nx.DestroyInstance);
    Resolve("xrCreateSession", nx.CreateSession);
    Resolve("xrDestroySession", nx.DestroySession);
    Resolve("xrBeginSession", nx.BeginSession);
    Resolve("xrEndSession", nx.EndSession);
    Resolve("xrCreateSwapchain", nx.CreateSwapchain);
    Resolve("xrDestroySwapchain", nx.DestroySwapchain);
    Resolve("xrEnumerateSwapchainImages", nx.EnumerateSwapchainImages);
    Resolve("xrAcquireSwapchainImage", nx.AcquireSwapchainImage);
    Resolve("xrWaitSwapchainImage", nx.WaitSwapchainImage);
    Resolve("xrReleaseSwapchainImage", nx.ReleaseSwapchainImage);
    Resolve("xrWaitFrame", nx.WaitFrame);
    Resolve("xrBeginFrame", nx.BeginFrame);
    Resolve("xrEndFrame", nx.EndFrame);
    if (g_d3d11Available) Resolve("xrGetD3D11GraphicsRequirementsKHR", nx.GetD3D11GraphicsRequirementsKHR);

    g_appName = info->applicationInfo.applicationName;
    g_hookEnabled = d3d11 || d3d12;
    if (!g_shared) g_shared = fs_shared::Open();
    Log("FrameSmooth layer loaded into '%s' (engine '%s'), D3D11=%d D3D12=%d", info->applicationInfo.applicationName,
        info->applicationInfo.engineName, int(d3d11), int(d3d12));
    if (!g_hookEnabled) PublishStatus(0, 1, 0, 0, 0, "Game uses Vulkan/OpenGL (unsupported)");
    return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* /*layerName*/, XrNegotiateApiLayerRequest* req) {
    if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo) ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        req->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || req->structSize != sizeof(XrNegotiateApiLayerRequest) ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    // Only take part while the FrameSmooth app runs. Otherwise decline (the loader then continues without
    // this layer) and switch our registration back off in case the app was terminated without cleaning up.
    if (!fs_shared::DashboardRunning()) {
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&xrNegotiateLoaderApiLayerInterface), &self) &&
            GetModuleFileNameW(self, path, MAX_PATH)) {
            std::wstring p(path);
            fs_shared::SetLayerRegistered(p.substr(0, p.find_last_of(L"\\/")) + L"\\FrameSmoothLayer.json", false);
        }
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = std::min<XrVersion>(XR_CURRENT_API_VERSION, loaderInfo->maxApiVersion);
    req->getInstanceProcAddr = Hook_GetInstanceProcAddr;
    req->createApiLayerInstance = Hook_CreateApiLayerInstance;
    return XR_SUCCESS;
}
