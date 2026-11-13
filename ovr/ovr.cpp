// FrameSmooth OpenVR hook. Loaded (only on user opt-in) into a running OpenVR game by the dashboard.
// It patches the game's IVRCompositor interface tables in memory (nothing on disk is changed) and
// virtualizes the frame loop exactly like the OpenXR layer does:
//   * a private thread calls the real WaitGetPoses/Submit at the headset refresh rate,
//   * the game's WaitGetPoses is released every Nth display frame with poses predicted for its slot,
//   * the game's Submit is captured (GPU copy into shared textures, D3D11 or D3D12),
//   * missing frames are extrapolated with the shared motion engine and submitted with their pose.

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
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <openvr.h>
#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flow.h"
#include "shared.h"

using Microsoft::WRL::ComPtr;
using fsflow::NowSec;
using namespace vr;

// ---------------------------------------------------------------------------------------------
static std::mutex g_logMtx;
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMtx);
    static FILE* f = nullptr;
    if (!f) {
        std::wstring p = fs_shared::DataDir() + L"\\openvr_hook.log";
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

static std::string g_exeName;  // lower-case file name, used as the per-game opt-in key
static fs_shared::Shared* g_shared = nullptr;

static void PublishStatus(int active, int divisor, float hz, float fps, float synth, const char* reason) {
    if (!g_shared) return;
    auto& st = g_shared->status;
    st.active = active;
    st.divisor = divisor;
    st.displayHz = hz;
    st.appFps = fps;
    st.synthPerSec = synth;
    strncpy_s(st.app, g_exeName.c_str(), _TRUNCATE);
    strncpy_s(st.reason, reason ? reason : "", _TRUNCATE);
    InterlockedExchange64(&st.heartbeatMs, (LONG64)GetTickCount64());
}

// ---------------------------------------------------------------------------------------------
// Interface tables
// ---------------------------------------------------------------------------------------------
enum { kWait = 2, kGetLast = 3, kGetLastIdx = 4, kSubmit = 5 };
constexpr int kMaxTables = 24;

struct Table {
    void** entries = nullptr;
    bool fn = false;  // C function table (no 'this')
    int ver = 0;
    int idxSubmit = 5, idxSWAI = -1, idxPPH = 7, idxExplicit = -1;
    int idxPredIds = -1, idxPosesForFrame = -1;  // pose-prediction-ID API (028+)
    void* orig[64]{};
};
static Table g_tab[kMaxTables];
static int g_nTab = 0;
static int g_primary = -1;          // table our thread uses (prefer C++ IVRCompositor_028)
static void* g_primarySelf = nullptr;
static IVRSystem* g_sys = nullptr;

typedef EVRCompositorError (*PFN_Wait)(void*, TrackedDevicePose_t*, uint32_t, TrackedDevicePose_t*, uint32_t);
typedef EVRCompositorError (*PFN_FnWait)(TrackedDevicePose_t*, uint32_t, TrackedDevicePose_t*, uint32_t);
typedef EVRCompositorError (*PFN_LastIdx)(void*, TrackedDeviceIndex_t, TrackedDevicePose_t*, TrackedDevicePose_t*);
typedef EVRCompositorError (*PFN_FnLastIdx)(TrackedDeviceIndex_t, TrackedDevicePose_t*, TrackedDevicePose_t*);
typedef EVRCompositorError (*PFN_Submit)(void*, EVREye, const Texture_t*, const VRTextureBounds_t*, EVRSubmitFlags);
typedef EVRCompositorError (*PFN_FnSubmit)(EVREye, const Texture_t*, const VRTextureBounds_t*, EVRSubmitFlags);
typedef EVRCompositorError (*PFN_SubmitAI)(void*, EVREye, const Texture_t*, uint32_t, const VRTextureBounds_t*, EVRSubmitFlags);
typedef EVRCompositorError (*PFN_FnSubmitAI)(EVREye, const Texture_t*, uint32_t, const VRTextureBounds_t*, EVRSubmitFlags);
typedef void (*PFN_Void)(void*);
typedef void (*PFN_FnVoid)();
typedef EVRCompositorError (*PFN_Err)(void*);
typedef EVRCompositorError (*PFN_FnErr)();
typedef ETrackingUniverseOrigin (*PFN_Space)(void*);
typedef EVRCompositorError (*PFN_PredIds)(void*, uint32_t*, uint32_t*);
typedef EVRCompositorError (*PFN_FnPredIds)(uint32_t*, uint32_t*);
typedef EVRCompositorError (*PFN_PosesForFrame)(void*, uint32_t, TrackedDevicePose_t*, uint32_t);
typedef EVRCompositorError (*PFN_FnPosesForFrame)(uint32_t, TrackedDevicePose_t*, uint32_t);
typedef void (*PFN_SysPose)(void*, ETrackingUniverseOrigin, float, TrackedDevicePose_t*, uint32_t);
typedef void (*PFN_FnSysPose)(ETrackingUniverseOrigin, float, TrackedDevicePose_t*, uint32_t);

// Which pose sources the game uses (logged in the periodic diagnostics)
static struct PoseCounters {
    std::atomic<uint32_t> wait{0}, last{0}, lastIdx{0}, predIds{0}, posesForFrame{0}, sysPose{0}, sysPoseOverridden{0};
} g_cnt;

static std::atomic<int> g_relOrigin{-1};  // tracking origin of the poses we hand to the game
static thread_local bool t_bypassPose = false;  // set while FrameSmooth itself queries poses
static void RealGetPose(ETrackingUniverseOrigin o, float secs, TrackedDevicePose_t* p, uint32_t n);

static EVRCompositorError CallWait(int I, void* self, TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* g, uint32_t gc, int idx = kWait) {
    auto& t = g_tab[I];
    return t.fn ? ((PFN_FnWait)t.orig[idx])(r, rc, g, gc) : ((PFN_Wait)t.orig[idx])(self, r, rc, g, gc);
}
static EVRCompositorError CallLastIdx(int I, void* self, TrackedDeviceIndex_t i, TrackedDevicePose_t* o, TrackedDevicePose_t* og) {
    auto& t = g_tab[I];
    return t.fn ? ((PFN_FnLastIdx)t.orig[kGetLastIdx])(i, o, og) : ((PFN_LastIdx)t.orig[kGetLastIdx])(self, i, o, og);
}
static EVRCompositorError CallSubmit(int I, void* self, EVREye e, const Texture_t* tx, const VRTextureBounds_t* b, EVRSubmitFlags f) {
    auto& t = g_tab[I];
    return t.fn ? ((PFN_FnSubmit)t.orig[t.idxSubmit])(e, tx, b, f) : ((PFN_Submit)t.orig[t.idxSubmit])(self, e, tx, b, f);
}
static EVRCompositorError CallSubmitAI(int I, void* self, EVREye e, const Texture_t* tx, uint32_t a, const VRTextureBounds_t* b, EVRSubmitFlags f) {
    auto& t = g_tab[I];
    return t.fn ? ((PFN_FnSubmitAI)t.orig[t.idxSWAI])(e, tx, a, b, f) : ((PFN_SubmitAI)t.orig[t.idxSWAI])(self, e, tx, a, b, f);
}
static void CallPPH(int I, void* self) {
    auto& t = g_tab[I];
    if (t.fn) ((PFN_FnVoid)t.orig[t.idxPPH])(); else ((PFN_Void)t.orig[t.idxPPH])(self);
}
static EVRCompositorError CallExplicit(int I, void* self) {
    auto& t = g_tab[I];
    if (t.idxExplicit < 0) return VRCompositorError_None;
    return t.fn ? ((PFN_FnErr)t.orig[t.idxExplicit])() : ((PFN_Err)t.orig[t.idxExplicit])(self);
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
enum class Api { None, D3D11, D3D12, Vulkan };

struct VkFns {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Vulkan queue lock. Vulkan requires that a VkQueue is never used by two threads at once. The game,
// SteamVR's client library and our frame thread all submit to the game's queue, so every queue
// function is wrapped so that each individual call is exclusive. The lock is held only for the
// duration of one driver call, never across other calls, so it cannot deadlock with other locks.
// Two copies per function: [0] = device-level address, [1] = vulkan-1.dll export (loader trampoline).
// ---------------------------------------------------------------------------------------------
static std::recursive_mutex g_qMtx;

template <int I>
struct QHook {
    static inline PFN_vkQueueSubmit oSubmit = nullptr;
    static inline PFN_vkQueueSubmit2 oSubmit2 = nullptr;
    static inline PFN_vkQueueSubmit2 oSubmit2KHR = nullptr;
    static inline PFN_vkQueuePresentKHR oPresent = nullptr;
    static inline PFN_vkQueueWaitIdle oWaitIdle = nullptr;
    static inline PFN_vkQueueBindSparse oBindSparse = nullptr;
    static VkResult VKAPI_CALL Submit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oSubmit(q, n, s, f);
    }
    static VkResult VKAPI_CALL Submit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oSubmit2(q, n, s, f);
    }
    static VkResult VKAPI_CALL Submit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oSubmit2KHR(q, n, s, f);
    }
    static VkResult VKAPI_CALL Present(VkQueue q, const VkPresentInfoKHR* p) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oPresent(q, p);
    }
    static VkResult VKAPI_CALL WaitIdle(VkQueue q) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oWaitIdle(q);
    }
    static VkResult VKAPI_CALL BindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* b, VkFence f) {
        std::lock_guard<std::recursive_mutex> l(g_qMtx);
        return oBindSparse(q, n, b, f);
    }
};

static std::vector<void*> g_hookedTargets;

static void Log(const char* fmt, ...);

static bool HookTarget(void* target, void* detour, void** orig) {
    if (!target) return true;
    for (void* t : g_hookedTargets)
        if (t == target) return true;  // same function reachable through both paths
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) { Log("MinHook failed on %p: %s", target, MH_StatusToString(s)); return false; }
    g_hookedTargets.push_back(target);
    return true;
}

static bool InstallQueueLock(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev) {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { Log("MinHook init failed: %s", MH_StatusToString(s)); return false; }
    HMODULE lib = GetModuleHandleW(L"vulkan-1.dll");
    auto dproc = [&](const char* n) { return reinterpret_cast<void*>(gdpa(dev, n)); };
    auto lproc = [&](const char* n) { return lib ? reinterpret_cast<void*>(GetProcAddress(lib, n)) : nullptr; };
    bool ok = true;
#define QH(name, member)                                                                                  \
    ok = HookTarget(dproc(name), (void*)&QHook<0>::member, (void**)&QHook<0>::o##member) && ok;            \
    ok = HookTarget(lproc(name), (void*)&QHook<1>::member, (void**)&QHook<1>::o##member) && ok;
    QH("vkQueueSubmit", Submit)
    QH("vkQueueSubmit2", Submit2)
    QH("vkQueueSubmit2KHR", Submit2KHR)
    QH("vkQueuePresentKHR", Present)
    QH("vkQueueWaitIdle", WaitIdle)
    QH("vkQueueBindSparse", BindSparse)
#undef QH
    Log("Vulkan queue lock installed on %zu functions%s", g_hookedTargets.size(), ok ? "" : " (with errors)");
    return ok && !g_hookedTargets.empty();
}

// VkFormat -> matching DXGI format for the shared capture texture.
static DXGI_FORMAT DxgiFromVk(uint32_t f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

struct EyeRect {
    bool valid = false;
    int capEye = 0;  // which capture texture holds this eye (shared texture -> 0)
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0, slice = 0;
};

struct AppFrame {
    uint64_t seq = 0;
    uint64_t target = 0;
    int ring = 0;
    HmdMatrix34_t pose{};
    EyeRect eye[2];
    void* leftHandle = nullptr;
    uint64_t fence = 0;
    EColorSpace cs = ColorSpace_Auto;
};

struct Capture {
    ComPtr<ID3D11Texture2D> ours;
    ComPtr<ID3D11Texture2D> app11;
    ComPtr<ID3D12Resource> app12;
    VkImage vkImg = VK_NULL_HANDLE;      // Vulkan view of the shared texture
    VkDeviceMemory vkMem = VK_NULL_HANDLE;
    uint32_t vkFormat = 0;
    UINT w = 0, h = 0, arr = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

struct OutEye {
    ComPtr<ID3D11Texture2D> cur;
    ComPtr<ID3D11ShaderResourceView> curSrv;
    UINT curW = 0, curH = 0, curArr = 0;
    DXGI_FORMAT curFmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> out[3];
    ComPtr<ID3D11RenderTargetView> rtv[3];
    UINT outW = 0, outH = 0;
    int outIdx = 0;
    bool haveOut = false;
    fsflow::View fv;
    EyeRect rect;
    // Vulkan games: our output textures are also imported as Vulkan images and submitted as such
    VkImage vkImg[3]{};
    VkDeviceMemory vkMem[3]{};
    uint32_t vkFormat = 0;
    float fov[4]{};  // tanLeft, tanRight, tanUp, tanDown of this eye
};

struct State {
    std::atomic<bool> virt{false};
    std::atomic<bool> wantVirt{false};
    std::atomic<bool> failed{false};  // compositor rejected our frames: permanently fall back to pass-through
    std::atomic<bool> softStop{false};  // watchdog: hand rendering back to the game for now
    int watchdogTrips = 0;
    int submitErrors = 0;
    std::atomic<double> lastAppCall{0};
    std::thread thr;

    // game side
    Api api = Api::None;
    ComPtr<ID3D11Device1> app11;
    ComPtr<ID3D11Device5> app11_5;
    ComPtr<ID3D11DeviceContext> appCtx;
    ComPtr<ID3D11DeviceContext4> appCtx4;
    ComPtr<ID3D12Device> app12;
    ID3D12CommandQueue* appQueue = nullptr;
    ComPtr<ID3D12CommandAllocator> alloc[3];
    ComPtr<ID3D12GraphicsCommandList> list[3];
    uint64_t allocFence[3]{};
    // Vulkan game side
    VkFns vk;
    VkDevice vkDev = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    uint32_t vkFamily = 0;
    VkPhysicalDeviceMemoryProperties vkMemProps{};
    VkCommandPool vkPool = VK_NULL_HANDLE;
    VkCommandBuffer vkCmd[3][2]{};
    VkFence vkFence[3][2]{};
    bool vkUsed[3][2]{};
    VkInstance vkInst = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhys = VK_NULL_HANDLE;
    // frame-thread side (command pools are not thread-safe, so it has its own)
    VkCommandPool vkPoolFT = VK_NULL_HANDLE;
    VkCommandBuffer vkAcq[2][3]{}, vkRel[2][3]{};
    VkFence vkOutFence[2][3]{};
    ComPtr<ID3D11Query> doneQuery;

    // ours
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    fsflow::Engine eng;
    bool devOk = false, devFailed = false;

    ComPtr<ID3D11Fence> appFenceOurs, appFence11, backFenceOurs, backFence11;
    ComPtr<ID3D12Fence> appFence12, backFence12;
    uint64_t appFenceValue = 0, backValue = 0;
    uint64_t readDone[3]{};

    Capture cap[3][2];
    std::mutex appMtx;
    AppFrame building;
    bool buildingActive = false;
    int lastRing = 0;
    uint64_t frameSeq = 0;

    // releases
    std::mutex relMtx;
    std::condition_variable relCv;
    uint64_t relSeq = 0, takenSeq = 0;
    uint64_t relTarget = 0;
    double relAt = 0, relFresh = 0;
    TrackedDevicePose_t relPoses[k_unMaxTrackedDeviceCount]{};
    TrackedDevicePose_t takenPoses[k_unMaxTrackedDeviceCount]{};
    uint64_t takenTarget = 0;
    uint64_t taken[8]{};
    int takenPos = 0;

    // submissions
    std::mutex subMtx;
    std::condition_variable subCv;
    std::deque<std::shared_ptr<AppFrame>> pending;

    // frame thread
    HmdMatrix34_t submitPose{};  // pose our output for the current slot was rotated to
    std::shared_ptr<AppFrame> current;
    uint64_t prevTarget = 0;
    OutEye eyes[2];
    fsflow::Pacer pacer;
    uint64_t slot = 0, nextRelease = 0;

    // settings cache
    double lastOptCheck = 0;
    bool optIn = true;
};
static State g;

static bool ShouldRun() {
    if (g_shared && !g_shared->settings.enabled) return false;
    double now = NowSec();
    if (now - g.lastOptCheck > 2.0) {
        g.lastOptCheck = now;
        g.optIn = fs_shared::IsGameEnabled(g_exeName) && fs_shared::DashboardRunning();
    }
    return g.optIn;
}

// ---------------------------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------------------------
static bool InitOurDevice(IDXGIAdapter* adapter) {
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    std::string err;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2,
                                 D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)) ||
        FAILED(g.dev.As(&g.dev1)) || FAILED(g.dev.As(&g.dev5)) || FAILED(g.ctx.As(&g.ctx4))) {
        Log("our device creation failed");
        return false;
    }
    if (!g.eng.Init(g.dev.Get(), g.ctx.Get(), err)) { Log("engine init failed: %s", err.c_str()); return false; }
    return SUCCEEDED(g.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.backFenceOurs)));
}

static bool InitFrom11(ID3D11Texture2D* tex) {
    ComPtr<ID3D11Device> d;
    tex->GetDevice(&d);
    if (FAILED(d.As(&g.app11)) || FAILED(d.As(&g.app11_5))) return false;
    d->GetImmediateContext(&g.appCtx);
    if (FAILED(g.appCtx.As(&g.appCtx4))) return false;
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(d.As(&dx)) || FAILED(dx->GetAdapter(&ad)) || !InitOurDevice(ad.Get())) return false;
    HANDLE h = nullptr;
    bool ok = SUCCEEDED(g.app11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.appFence11))) &&
              SUCCEEDED(g.appFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
              SUCCEEDED(g.dev5->OpenSharedFence(h, IID_PPV_ARGS(&g.appFenceOurs)));
    if (h) CloseHandle(h);
    h = nullptr;
    ok = ok && SUCCEEDED(g.backFenceOurs->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
         SUCCEEDED(g.app11_5->OpenSharedFence(h, IID_PPV_ARGS(&g.backFence11)));
    if (h) CloseHandle(h);
    g.api = Api::D3D11;
    return ok;
}

static bool InitFrom12(ID3D12Resource* res, ID3D12CommandQueue* q) {
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&g.app12)))) return false;
    g.appQueue = q;
    LUID luid = g.app12->GetAdapterLuid();
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) ||
        !InitOurDevice(ad.Get()))
        return false;
    HANDLE h = nullptr;
    bool ok = SUCCEEDED(g.app12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.appFence12))) &&
              SUCCEEDED(g.app12->CreateSharedHandle(g.appFence12.Get(), nullptr, GENERIC_ALL, nullptr, &h)) &&
              SUCCEEDED(g.dev5->OpenSharedFence(h, IID_PPV_ARGS(&g.appFenceOurs)));
    if (h) CloseHandle(h);
    h = nullptr;
    ok = ok && SUCCEEDED(g.backFenceOurs->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
         SUCCEEDED(g.app12->OpenSharedHandle(h, IID_PPV_ARGS(&g.backFence12)));
    if (h) CloseHandle(h);
    for (int i = 0; ok && i < 3; i++) {
        ok = SUCCEEDED(g.app12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i]))) &&
             SUCCEEDED(g.app12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i].Get(), nullptr, IID_PPV_ARGS(&g.list[i])));
        if (ok) g.list[i]->Close();
    }
    g.api = Api::D3D12;
    return ok;
}

// Vulkan game: frames are copied (on the game's queue) into D3D11 textures imported as Vulkan images.
// Requires the game's device to have VK_KHR_external_memory_win32 enabled (SteamVR asks games for it).
static bool InitFromVk(const VRVulkanTextureData_t* d) {
    HMODULE vkLib = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = vkLib ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vkLib, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa || !d->m_pInstance || !d->m_pDevice || !d->m_pPhysicalDevice || !d->m_pQueue) { Log("Vulkan: missing handles"); return false; }
    VkInstance inst = d->m_pInstance;
    auto props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!props2) props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
    auto memProps = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    g.vk.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    if (!props2 || !memProps || !g.vk.GetDeviceProcAddr) { Log("Vulkan: instance functions unavailable"); return false; }
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    props2(d->m_pPhysicalDevice, &p2);
    if (!idp.deviceLUIDValid) { Log("Vulkan: no device LUID"); return false; }
    memProps(d->m_pPhysicalDevice, &g.vkMemProps);

    VkDevice dev = d->m_pDevice;
    auto& f = g.vk;
#define VKD(name) f.name = (PFN_vk##name)f.GetDeviceProcAddr(dev, "vk" #name); if (!f.name) { Log("Vulkan: missing vk" #name); return false; }
    VKD(CreateImage) VKD(DestroyImage) VKD(GetImageMemoryRequirements) VKD(AllocateMemory) VKD(FreeMemory)
    VKD(BindImageMemory) VKD(CreateCommandPool) VKD(AllocateCommandBuffers) VKD(BeginCommandBuffer)
    VKD(EndCommandBuffer) VKD(ResetCommandBuffer) VKD(CmdPipelineBarrier) VKD(CmdCopyImage) VKD(QueueSubmit)
    VKD(CreateFence) VKD(GetFenceStatus) VKD(WaitForFences) VKD(ResetFences)
#undef VKD
    if (!f.GetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR")) {
        Log("Vulkan: game device has no VK_KHR_external_memory_win32; cannot share frames");
        return false;
    }

    LUID luid;
    memcpy(&luid, idp.deviceLUID, sizeof(luid));
    ComPtr<IDXGIFactory4> fac;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || FAILED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) ||
        !InitOurDevice(ad.Get()))
        return false;

    g.vkDev = dev;
    g.vkQueue = d->m_pQueue;
    g.vkFamily = d->m_nQueueFamilyIndex;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g.vkFamily};
    if (f.CreateCommandPool(dev, &pci, nullptr, &g.vkPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g.vkPool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 6};
    if (f.AllocateCommandBuffers(dev, &cai, &g.vkCmd[0][0]) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int r = 0; r < 3; r++)
        for (int e = 0; e < 2; e++)
            if (f.CreateFence(dev, &fci, nullptr, &g.vkFence[r][e]) != VK_SUCCESS) return false;
    // frame thread: its own pool, acquire/release command buffers and fences per eye and output slot
    if (f.CreateCommandPool(dev, &pci, nullptr, &g.vkPoolFT) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g.vkPoolFT,
                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 6};
    if (f.AllocateCommandBuffers(dev, &cai2, &g.vkAcq[0][0]) != VK_SUCCESS ||
        f.AllocateCommandBuffers(dev, &cai2, &g.vkRel[0][0]) != VK_SUCCESS)
        return false;
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < 3; i++)
            if (f.CreateFence(dev, &fci, nullptr, &g.vkOutFence[e][i]) != VK_SUCCESS) return false;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    if (FAILED(g.dev->CreateQuery(&qd, &g.doneQuery))) return false;
    g.vkInst = d->m_pInstance;
    g.vkPhys = d->m_pPhysicalDevice;
    // Serialize all use of Vulkan queues (game, SteamVR client and us)
    if (!InstallQueueLock(f.GetDeviceProcAddr, dev)) return false;
    g.api = Api::Vulkan;
    return true;
}

constexpr bool kVulkanSubmitSupported = true;  // frames are handed to SteamVR as Vulkan images (queue-locked)

// Capture setup (shader compilation, device and Vulkan sharing setup) takes a few seconds, so it runs
// on a background thread; the game keeps running untouched until it is ready.
static std::atomic<int> g_initState{0};  // 0 = not started, 1 = running, 2 = ready, 3 = failed

template <typename Fn>
static void StartInit(const char* what, Fn fn) {
    int expected = 0;
    if (!g_initState.compare_exchange_strong(expected, 1)) return;
    std::thread([what, fn]() {
        double t0 = NowSec();
        bool ok = fn();
        g.devOk = ok;
        g.devFailed = !ok;
        Log("%s capture init: %s (%.1f s, in background)", what, ok ? "ok" : "FAILED", NowSec() - t0);
        g_initState = ok ? 2 : 3;
    }).detach();
}

static bool CaptureInitPending() { return g_initState.load() == 1; }

// Is this Submit something we can capture? Starts the (background) device setup on first use.
static bool Capturable(const Texture_t* t, EVRSubmitFlags flags) {
    if (!t || !t->handle) return false;
    if (flags & (Submit_LensDistortionAlreadyApplied | Submit_GlRenderBuffer | Submit_GlArrayTexture | Submit_VulkanTextureWithArrayData))
        return false;
    if (t->eType == TextureType_DirectX) {
        ComPtr<ID3D11Texture2D> tex = static_cast<ID3D11Texture2D*>(t->handle);  // keeps it alive for the init thread
        StartInit("D3D11", [tex]() { return InitFrom11(tex.Get()); });
        return g_initState == 2 && g.api == Api::D3D11;
    }
    if (t->eType == TextureType_DirectX12) {
        auto* d = static_cast<const D3D12TextureData_t*>(t->handle);
        if (!d->m_pResource || !d->m_pCommandQueue) return false;
        if (d->m_pResource->GetDesc().SampleDesc.Count > 1) return false;
        ComPtr<ID3D12Resource> res = d->m_pResource;
        ComPtr<ID3D12CommandQueue> q = d->m_pCommandQueue;
        StartInit("D3D12", [res, q]() { return InitFrom12(res.Get(), q.Get()); });
        return g_initState == 2 && g.api == Api::D3D12;
    }
    if (t->eType == TextureType_Vulkan) {
        if (!kVulkanSubmitSupported) return false;
        auto* d = static_cast<const VRVulkanTextureData_t*>(t->handle);
        if (d->m_nSampleCount > 1 || DxgiFromVk(d->m_nFormat) == DXGI_FORMAT_UNKNOWN) {
            static bool logged = false;
            if (!logged) { Log("Vulkan: unsupported image (format %u, samples %u)", d->m_nFormat, d->m_nSampleCount); logged = true; }
            return false;
        }
        VRVulkanTextureData_t copy = *d;
        StartInit("Vulkan", [copy]() { return InitFromVk(&copy); });
        return g_initState == 2 && g.api == Api::Vulkan;
    }
    return false;
}

static bool ImportD3D11ToVk(HANDLE nt, uint32_t w, uint32_t h, uint32_t vkFormat, VkImage* outImg, VkDeviceMemory* outMem) {
    auto& f = g.vk;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VkFormat(vkFormat);
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (f.CreateImage(g.vkDev, &ici, nullptr, &img) != VK_SUCCESS) { Log("Vulkan: CreateImage (output) failed"); return false; }
    VkMemoryRequirements req;
    f.GetImageMemoryRequirements(g.vkDev, img, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.vkMemProps.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (g.vkMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, img, VK_NULL_HANDLE};
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded,
                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, nt, nullptr};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, req.size, type};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (type == UINT32_MAX || f.AllocateMemory(g.vkDev, &mai, nullptr, &mem) != VK_SUCCESS ||
        f.BindImageMemory(g.vkDev, img, mem, 0) != VK_SUCCESS) {
        Log("Vulkan: output memory import failed");
        if (mem) f.FreeMemory(g.vkDev, mem, nullptr);
        f.DestroyImage(g.vkDev, img, nullptr);
        return false;
    }
    *outImg = img;
    *outMem = mem;
    return true;
}

static bool ImportToVulkan(Capture& c, HANDLE nt, uint32_t vkFormat) {
    auto& f = g.vk;
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VkFormat(vkFormat);
    ici.extent = {c.w, c.h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.CreateImage(g.vkDev, &ici, nullptr, &c.vkImg) != VK_SUCCESS) { Log("Vulkan: CreateImage failed"); return false; }
    VkMemoryRequirements req;
    f.GetImageMemoryRequirements(g.vkDev, c.vkImg, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.vkMemProps.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (g.vkMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == UINT32_MAX) { Log("Vulkan: no memory type"); return false; }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, c.vkImg, VK_NULL_HANDLE};
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &ded,
                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, nt, nullptr};
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, req.size, type};
    if (f.AllocateMemory(g.vkDev, &mai, nullptr, &c.vkMem) != VK_SUCCESS) { Log("Vulkan: memory import failed"); return false; }
    if (f.BindImageMemory(g.vkDev, c.vkImg, c.vkMem, 0) != VK_SUCCESS) { Log("Vulkan: bind failed"); return false; }
    c.vkFormat = vkFormat;
    return true;
}

static void ReleaseVulkanCapture(Capture& c) {
    if (g.api != Api::Vulkan) return;
    if (c.vkImg) g.vk.DestroyImage(g.vkDev, c.vkImg, nullptr);
    if (c.vkMem) g.vk.FreeMemory(g.vkDev, c.vkMem, nullptr);
    c.vkImg = VK_NULL_HANDLE;
    c.vkMem = VK_NULL_HANDLE;
}

static bool EnsureCapture(Capture& c, UINT w, UINT h, UINT arr, DXGI_FORMAT fmt, uint32_t vkFormat = 0) {
    if (c.ours && c.w == w && c.h == h && c.arr == arr && c.fmt == fmt) return true;
    ReleaseVulkanCapture(c);
    c = Capture{};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = arr;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;  // shared textures must be render targets
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<IDXGIResource1> res;
    HANDLE hnd = nullptr;
    HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &c.ours);
    if (SUCCEEDED(hr)) hr = c.ours.As(&res);
    if (SUCCEEDED(hr)) hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hnd);
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 3) Log("capture texture create failed (fmt %d, %ux%u x%u): hr=0x%08lx", fmt, w, h, arr, (unsigned long)hr);
        c = Capture{};
        return false;
    }
    c.w = w; c.h = h; c.arr = arr; c.fmt = fmt;
    bool ok;
    if (g.api == Api::D3D11) ok = SUCCEEDED(g.app11->OpenSharedResource1(hnd, IID_PPV_ARGS(&c.app11)));
    else if (g.api == Api::D3D12) ok = SUCCEEDED(g.app12->OpenSharedHandle(hnd, IID_PPV_ARGS(&c.app12)));
    else ok = ImportToVulkan(c, hnd, vkFormat);
    CloseHandle(hnd);
    if (!ok) { Log("capture texture open failed"); ReleaseVulkanCapture(c); c = Capture{}; return false; }
    return true;
}

static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    l->ResourceBarrier(1, &x);
}

// GPU copy of the game's texture into capture slot [ring][eye] (runs on the game's render thread).
static bool CaptureEye(const Texture_t* t, int ring, int eye, UINT& texW, UINT& texH) {
    Capture& c = g.cap[ring][eye];
    if (g.api == Api::D3D11) {
        auto* src = static_cast<ID3D11Texture2D*>(t->handle);
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (!EnsureCapture(c, sd.Width, sd.Height, sd.ArraySize, sd.Format)) return false;
        for (UINT sl = 0; sl < sd.ArraySize; sl++) {
            UINT dst = D3D11CalcSubresource(0, sl, 1), srcSub = D3D11CalcSubresource(0, sl, sd.MipLevels);
            if (sd.SampleDesc.Count > 1)
                g.appCtx->ResolveSubresource(c.app11.Get(), dst, src, srcSub, fsflow::TypedFormat(sd.Format));
            else
                g.appCtx->CopySubresourceRegion(c.app11.Get(), dst, 0, 0, 0, src, srcSub, nullptr);
        }
        texW = sd.Width;
        texH = sd.Height;
        return true;
    }
    if (g.api == Api::Vulkan) {
        auto* d = static_cast<const VRVulkanTextureData_t*>(t->handle);
        if (!EnsureCapture(c, d->m_nWidth, d->m_nHeight, 1, DxgiFromVk(d->m_nFormat), d->m_nFormat)) return false;
        auto& f = g.vk;
        VkCommandBuffer cmd = g.vkCmd[ring][eye];
        VkFence fence = g.vkFence[ring][eye];
        f.ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        f.BeginCommandBuffer(cmd, &bi);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b[2]{};
        // game image: OpenVR requires TRANSFER_SRC_OPTIMAL at Submit; make prior writes visible to our copy
        b[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, (VkImage)d->m_nImage, range};
        b[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c.vkImg, range};
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {d->m_nWidth, d->m_nHeight, 1};
        f.CmdCopyImage(cmd, (VkImage)d->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.vkImg,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        // hand the shared image back to the "external" (D3D11) side
        VkImageMemoryBarrier rel{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, g.vkFamily,
                                 VK_QUEUE_FAMILY_EXTERNAL, c.vkImg, range};
        f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &rel);
        f.EndCommandBuffer(cmd);
        f.ResetFences(g.vkDev, 1, &fence);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (f.QueueSubmit(g.vkQueue, 1, &si, fence) != VK_SUCCESS) { Log("Vulkan: QueueSubmit failed"); return false; }
        g.vkUsed[ring][eye] = true;
        texW = d->m_nWidth;
        texH = d->m_nHeight;
        return true;
    }
    auto* d = static_cast<const D3D12TextureData_t*>(t->handle);
    D3D12_RESOURCE_DESC rd = d->m_pResource->GetDesc();
    if (!EnsureCapture(c, UINT(rd.Width), rd.Height, rd.DepthOrArraySize, rd.Format)) return false;
    g.appQueue = d->m_pCommandQueue;
    ID3D12GraphicsCommandList* l = g.list[ring].Get();
    l->Reset(g.alloc[ring].Get(), nullptr);
    // OpenVR requires D3D12 textures to be in PIXEL_SHADER_RESOURCE state at Submit.
    Barrier(l, d->m_pResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(l, c.app12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT sl = 0; sl < rd.DepthOrArraySize; sl++) {
        D3D12_TEXTURE_COPY_LOCATION dst{c.app12.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = sl;
        D3D12_TEXTURE_COPY_LOCATION src{d->m_pResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = sl * rd.MipLevels;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Barrier(l, d->m_pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(l, c.app12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    l->Close();
    ID3D12CommandList* lists[] = {l};
    g.appQueue->ExecuteCommandLists(1, lists);
    texW = UINT(rd.Width);
    texH = rd.Height;
    return true;
}

// Start capturing a new game frame into ring slot r; makes sure the slot is no longer being read.
static void BeginFrameCapture() {
    int r = (g.lastRing + 1) % 3;
    g.lastRing = r;
    if (g.api == Api::D3D11) {
        if (g.readDone[r]) g.appCtx4->Wait(g.backFence11.Get(), g.readDone[r]);
    } else if (g.api == Api::Vulkan) {
        // previous copies into this slot must be finished, and we must have finished reading it
        VkFence fs[2];
        uint32_t n = 0;
        for (int e = 0; e < 2; e++)
            if (g.vkUsed[r][e]) fs[n++] = g.vkFence[r][e];
        if (n) g.vk.WaitForFences(g.vkDev, n, fs, VK_TRUE, 100000000ull);
        if (g.readDone[r] && g.backFenceOurs->GetCompletedValue() < g.readDone[r]) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g.backFenceOurs->SetEventOnCompletion(g.readDone[r], ev);
            WaitForSingleObject(ev, 100);
            CloseHandle(ev);
        }
    } else {
        if (g.allocFence[r] && g.appFence12->GetCompletedValue() < g.allocFence[r]) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g.appFence12->SetEventOnCompletion(g.allocFence[r], ev);
            WaitForSingleObject(ev, 100);
            CloseHandle(ev);
        }
        g.alloc[r]->Reset();
        if (g.readDone[r] && g.appQueue) g.appQueue->Wait(g.backFence12.Get(), g.readDone[r]);
    }
    g.building = AppFrame{};
    g.building.ring = r;
    {
        std::lock_guard<std::mutex> lk(g.relMtx);
        g.building.target = g.takenTarget;
        g.building.pose = g.takenPoses[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    }
    g.buildingActive = true;
}

static void FinishFrameCapture() {
    uint64_t v = ++g.appFenceValue;
    if (g.api == Api::D3D11) {
        g.appCtx4->Signal(g.appFence11.Get(), v);
        g.appCtx->Flush();
    } else if (g.api == Api::D3D12) {
        g.appQueue->Signal(g.appFence12.Get(), v);
        g.allocFence[g.building.ring] = v;
    }  // Vulkan: completion is tracked with the per-eye VkFences
    auto f = std::make_shared<AppFrame>(g.building);
    f->fence = v;
    f->seq = ++g.frameSeq;
    g.buildingActive = false;
    {
        std::lock_guard<std::mutex> lk(g.subMtx);
        g.pending.push_back(f);
        while (g.pending.size() > 3) g.pending.pop_front();
    }
    g.subCv.notify_all();
}

static void CaptureSubmit(EVREye eye, const Texture_t* t, uint32_t arrayIndex, const VRTextureBounds_t* b, EVRSubmitFlags flags) {
    std::lock_guard<std::mutex> lk(g.appMtx);
    int e = eye == Eye_Right ? 1 : 0;
    if (!g.buildingActive || g.building.eye[e].valid) BeginFrameCapture();
    AppFrame& f = g.building;
    if (flags & Submit_TextureWithPose) f.pose = static_cast<const VRTextureWithPose_t*>(t)->mDeviceToAbsoluteTracking;
    f.cs = t->eColorSpace;
    UINT texW = 0, texH = 0;
    EyeRect r;
    if (e == 1 && f.eye[0].valid && f.leftHandle == t->handle) {
        r.capEye = 0;  // same texture as the left eye (side-by-side)
        texW = g.cap[f.ring][0].w;
        texH = g.cap[f.ring][0].h;
    } else {
        if (!CaptureEye(t, f.ring, e, texW, texH)) return;
        r.capEye = e;
        if (e == 0) f.leftHandle = t->handle;
    }
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (b) {
        u0 = std::min(b->uMin, b->uMax); u1 = std::max(b->uMin, b->uMax);
        v0 = std::min(b->vMin, b->vMax); v1 = std::max(b->vMin, b->vMax);
    }
    r.x = int32_t(u0 * texW + 0.5f);
    r.y = int32_t(v0 * texH + 0.5f);
    r.w = std::max(1u, uint32_t((u1 - u0) * texW + 0.5f));
    r.h = std::max(1u, uint32_t((v1 - v0) * texH + 0.5f));
    r.slice = arrayIndex;
    r.valid = true;
    f.eye[e] = r;
    if (f.eye[0].valid && f.eye[1].valid) FinishFrameCapture();
}

// ---------------------------------------------------------------------------------------------
// Frame thread
// ---------------------------------------------------------------------------------------------
static void EnsureCur(OutEye& o, const Capture& c) {
    if (o.cur && o.curW == c.w && o.curH == c.h && o.curArr == c.arr && o.curFmt == c.fmt) return;
    o.cur.Reset();
    o.curSrv.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = c.w; d.Height = c.h; d.MipLevels = 1; d.ArraySize = c.arr; d.Format = c.fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &o.cur))) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fsflow::TypedFormat(c.fmt);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = c.arr;
    g.dev->CreateShaderResourceView(o.cur.Get(), &sd, &o.curSrv);
    o.curW = c.w; o.curH = c.h; o.curArr = c.arr; o.curFmt = c.fmt;
}

static int EyeIndex(const OutEye& o) { return &o == &g.eyes[1] ? 1 : 0; }

static void WaitOutSlot(const OutEye& o, int idx) {
    if (g.api != Api::Vulkan) return;
    g.vk.WaitForFences(g.vkDev, 1, &g.vkOutFence[EyeIndex(o)][idx], VK_TRUE, 100000000ull);
}

static bool EnsureOutTex(OutEye& o, UINT w, UINT h) {
    if (o.out[0] && o.outW == w && o.outH == h) return true;
    const bool vk = g.api == Api::Vulkan;
    if (vk) {  // release previous Vulkan views once the GPU is done with them
        for (int i = 0; i < 3; i++) {
            WaitOutSlot(o, i);
            if (o.vkImg[i]) g.vk.DestroyImage(g.vkDev, o.vkImg[i], nullptr);
            if (o.vkMem[i]) g.vk.FreeMemory(g.vkDev, o.vkMem[i], nullptr);
            o.vkImg[i] = VK_NULL_HANDLE;
            o.vkMem[i] = VK_NULL_HANDLE;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = o.curFmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = vk ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE) : D3D11_RESOURCE_MISC_SHARED;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = fsflow::TypedFormat(o.curFmt);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < 3; i++) {
        o.out[i].Reset();
        o.rtv[i].Reset();
        HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &o.out[i]);
        if (SUCCEEDED(hr)) hr = g.dev->CreateRenderTargetView(o.out[i].Get(), &rd, &o.rtv[i]);
        if (SUCCEEDED(hr) && vk) {
            ComPtr<IDXGIResource1> res;
            HANDLE nt = nullptr;
            hr = o.out[i].As(&res);
            if (SUCCEEDED(hr)) hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt);
            if (SUCCEEDED(hr) && !ImportD3D11ToVk(nt, w, h, o.vkFormat, &o.vkImg[i], &o.vkMem[i])) hr = E_FAIL;
            if (nt) CloseHandle(nt);
        }
        if (FAILED(hr)) {
            Log("output texture failed (fmt %d): hr=0x%08lx", o.curFmt, (unsigned long)hr);
            o.out[0].Reset();
            return false;
        }
    }
    o.outW = w;
    o.outH = h;
    o.haveOut = false;
    return true;
}

static fsflow::Source SrcOf(const OutEye& o) {
    fsflow::Source s;
    s.srv = o.curSrv.Get();
    s.texW = o.curW;
    s.texH = o.curH;
    s.x = o.rect.x;
    s.y = o.rect.y;
    s.w = o.rect.w;
    s.h = o.rect.h;
    s.slice = o.rect.slice;
    return s;
}

static void EyeRotation(const HmdMatrix34_t& hmd, EVREye eye, float R[3][3]) {
    HmdMatrix34_t e2h = g_sys->GetEyeToHeadTransform(eye);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            R[i][j] = hmd.m[i][0] * e2h.m[0][j] + hmd.m[i][1] * e2h.m[1][j] + hmd.m[i][2] * e2h.m[2][j];
}

static void Ingest(const std::shared_ptr<AppFrame>& f, bool motion) {
    if (g.api != Api::Vulkan) g.ctx4->Wait(g.appFenceOurs.Get(), f->fence);  // Vulkan: already complete (FrameReady)
    for (int e = 0; e < 2; e++) {
        OutEye& o = g.eyes[e];
        const EyeRect& r = f->eye[e];
        const Capture& c = g.cap[f->ring][r.capEye];
        if (!c.ours) continue;
        EnsureCur(o, c);
        if (!o.cur) continue;
        o.vkFormat = c.vkFormat;
        if (r.w != o.rect.w || r.h != o.rect.h) o.fv.havePrev = false;
        o.rect = r;
        g.ctx->CopyResource(o.cur.Get(), c.ours.Get());
    }
    g.backValue++;
    g.ctx4->Signal(g.backFenceOurs.Get(), g.backValue);
    g.readDone[f->ring] = g.backValue;
    for (int e = 0; e < 2; e++) {
        OutEye& o = g.eyes[e];
        if (!o.curSrv) continue;
        float R[3][3];
        EyeRotation(f->pose, e ? Eye_Right : Eye_Left, R);
        float l, rr, t, b;
        g_sys->GetProjectionRaw(e ? Eye_Right : Eye_Left, &l, &rr, &t, &b);
        // OpenVR quirk (verified against GetProjectionMatrix on the Steam Frame): the raw "bottom" value is
        // the tangent of the UPPER view edge and "top" the (negative) tangent of the LOWER edge.
        o.fov[0] = l; o.fov[1] = rr; o.fov[2] = b; o.fov[3] = t;
        g.eng.Analyze(o.fv, SrcOf(o), R, o.fov, 1, motion);
    }
    g.prevTarget = g.current ? g.current->target : 0;
    g.current = f;
    g.ctx->Flush();
}

// Produce one eye's output for this display slot: the newest game frame, extrapolated by s frames of scene
// motion (s = 0: none) and rotated from the pose it was rendered with to the slot's own pose. The result is
// submitted with the slot pose, so it is correct whether or not SteamVR applies per-frame poses itself.
static void ProduceEye(OutEye& o, int eye, float s, bool tint, const HmdMatrix34_t& framePose, const HmdMatrix34_t& slotPose) {
    if (!o.curSrv || !EnsureOutTex(o, o.rect.w, o.rect.h)) return;
    o.outIdx = (o.outIdx + 1) % 3;
    WaitOutSlot(o, o.outIdx);  // Vulkan: SteamVR must be done with this slot before we overwrite it
    float Rsrc[3][3], Rout[3][3];
    EyeRotation(framePose, eye ? Eye_Right : Eye_Left, Rsrc);
    EyeRotation(slotPose, eye ? Eye_Right : Eye_Left, Rout);
    g.eng.Warp(o.fv, SrcOf(o), o.rtv[o.outIdx].Get(), s, tint, Rsrc, Rout, o.fov, o.fov);
    o.haveOut = true;
}

// Record + submit one image barrier on the game's queue (the queue functions are wrapped by the queue lock).
static void VkBarrierSubmit(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t srcQ,
                            uint32_t dstQ, VkAccessFlags srcA, VkAccessFlags dstA, VkFence fence) {
    auto& f = g.vk;
    f.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    f.BeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, srcA, dstA, from, to, srcQ, dstQ, img,
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    f.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    f.EndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    f.QueueSubmit(g.vkQueue, 1, &si, fence);
}

// Wait (briefly) until our D3D11 work for this frame has finished on the GPU.
static void WaitD3D11Idle() {
    g.ctx->End(g.doneQuery.Get());
    g.ctx->Flush();
    double t0 = NowSec();
    while (g.ctx->GetData(g.doneQuery.Get(), nullptr, 0, 0) == S_FALSE && NowSec() - t0 < 0.03) YieldProcessor();
}

// Hand one eye's output to SteamVR as a Vulkan image: acquire from D3D11 -> Submit -> release back to D3D11.
static EVRCompositorError SubmitEyeVulkan(int P0, void* self, int e, OutEye& o, const AppFrame& frame) {
    int i = o.outIdx;
    VkImage img = o.vkImg[i];
    if (!img) return VRCompositorError_InvalidTexture;
    WaitOutSlot(o, i);
    g.vk.ResetFences(g.vkDev, 1, &g.vkOutFence[e][i]);
    VkBarrierSubmit(g.vkAcq[e][i], img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_EXTERNAL, g.vkFamily, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_NULL_HANDLE);
    VRVulkanTextureData_t vd{};
    vd.m_nImage = (uint64_t)img;
    vd.m_pDevice = g.vkDev;
    vd.m_pPhysicalDevice = g.vkPhys;
    vd.m_pInstance = g.vkInst;
    vd.m_pQueue = g.vkQueue;
    vd.m_nQueueFamilyIndex = g.vkFamily;
    vd.m_nWidth = o.outW;
    vd.m_nHeight = o.outH;
    vd.m_nFormat = o.vkFormat;
    vd.m_nSampleCount = 1;
    VRTextureWithPose_t t{};
    t.handle = &vd;
    t.eType = TextureType_Vulkan;
    t.eColorSpace = frame.cs;
    t.mDeviceToAbsoluteTracking = g.submitPose;
    EVRCompositorError se = CallSubmit(P0, self, e ? Eye_Right : Eye_Left, &t, nullptr, Submit_TextureWithPose);
    VkBarrierSubmit(g.vkRel[e][i], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, g.vkFamily,
                    VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_READ_BIT, 0, g.vkOutFence[e][i]);
    return se;
}

// Has the GPU finished copying this game frame into our capture textures?
static bool FrameReady(const AppFrame& f) {
    if (g.api == Api::Vulkan) {
        for (int e = 0; e < 2; e++)
            if (f.eye[e].valid && f.eye[e].capEye == e && g.vk.GetFenceStatus(g.vkDev, g.vkFence[f.ring][e]) != VK_SUCCESS)
                return false;
        return true;
    }
    return g.appFenceOurs->GetCompletedValue() >= f.fence;
}

static std::shared_ptr<AppFrame> PickReady(uint64_t slot) {
    int best = -1;
    for (int i = 0; i < int(g.pending.size()); i++)
        if (g.pending[i]->target <= slot && FrameReady(*g.pending[i])) best = i;
    if (best < 0) return nullptr;
    auto r = g.pending[best];
    g.pending.erase(g.pending.begin(), g.pending.begin() + best + 1);
    return r;
}

static bool ExpectedFor(uint64_t slot) {
    std::lock_guard<std::mutex> lk(g.relMtx);
    for (uint64_t t : g.taken)
        if (t == slot) return !g.current || g.current->target < t;
    return false;
}

static void FrameThread() {
    Log("frame thread started");
    TrackedDevicePose_t rp[k_unMaxTrackedDeviceCount], gp[k_unMaxTrackedDeviceCount];
    double statT0 = NowSec(), lastProp = 0;
    int statReal = 0, statSynth = 0;
    float hz = 90.0f, vsyncToPhotons = 0.0f;
    const char* apiName = g.api == Api::D3D12    ? "Active (OpenVR, D3D12)"
                          : g.api == Api::Vulkan ? "Active (OpenVR, Vulkan)"
                                                 : "Active (OpenVR, D3D11)";
    void* self = g_primarySelf;
    const int P0 = g_primary;
    double lastFrame = NowSec();
    while (g.virt && !g.failed && !g.softStop) {
        if (NowSec() - g.lastAppCall.load() > 0.5) { Sleep(2); lastFrame = NowSec(); continue; }  // game not rendering: pause
        // Watchdog: never keep showing a stale image. No complete game frame for 2 s -> give rendering back.
        if (NowSec() - lastFrame > 2.0) {
            if (++g.watchdogTrips >= 3) {
                Log("watchdog: no game frames for 2 s (3rd time); falling back to pass-through for this session");
                g.failed = true;
            } else {
                Log("watchdog: no game frames for 2 s; handing rendering back to the game");
                g.softStop = true;
            }
            g.relCv.notify_all();
            break;
        }
        if (CallWait(P0, self, rp, k_unMaxTrackedDeviceCount, gp, k_unMaxTrackedDeviceCount) != VRCompositorError_None) {
            Sleep(2);
        }
        double wake = NowSec();
        CallExplicit(P0, self);  // no-op unless the game enabled explicit timing
        g.slot++;
        if (wake - lastProp > 2.0) {
            lastProp = wake;
            float f = g_sys->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_DisplayFrequency_Float);
            if (f > 20.0f) hz = f;
            vsyncToPhotons = g_sys->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_SecondsFromVsyncToPhotons_Float);
        }
        const double P = 1.0 / hz;
        fs_shared::Settings st = g_shared ? g_shared->settings : fs_shared::LoadSettings();
        if (g.pacer.Update(st.rateMode, wake)) {
            Log("divisor -> %d", g.pacer.divisor);
            g.nextRelease = g.slot;
        }
        const int D = g.pacer.divisor;
        if (g.slot >= g.nextRelease) {
            float since = 0;
            uint64_t fc = 0;
            g_sys->GetTimeSinceLastVsync(&since, &fc);
            float pred = float(P - since + P + vsyncToPhotons + D * P);
            pred = std::clamp(pred, 0.0f, 0.2f);
            ETrackingUniverseOrigin origin = ((PFN_Space)g_tab[P0].orig[1] && !g_tab[P0].fn)
                                                 ? ((PFN_Space)g_tab[P0].orig[1])(self)
                                                 : TrackingUniverseStanding;
            g_relOrigin = int(origin);
            TrackedDevicePose_t predicted[k_unMaxTrackedDeviceCount];
            RealGetPose(origin, pred, predicted, k_unMaxTrackedDeviceCount);  // never while holding relMtx
            {
                std::lock_guard<std::mutex> lk(g.relMtx);
                memcpy(g.relPoses, predicted, sizeof(predicted));
                g.relSeq++;
                g.relTarget = g.slot + D;
                g.relAt = wake;
                g.relFresh = (D - 0.5) * P;
            }
            g.relCv.notify_all();
            g.nextRelease = g.slot + D;
        }

        bool expected = ExpectedFor(g.slot);
        const double deadline = wake + 0.5 * P;
        std::shared_ptr<AppFrame> pick;
        {
            std::unique_lock<std::mutex> lk(g.subMtx);
            for (;;) {
                auto p = PickReady(g.slot);
                if (p) pick = p;
                if ((pick && pick->target == g.slot) || !expected || !g.virt || NowSec() >= deadline) break;
                g.subCv.wait_for(lk, std::chrono::microseconds(500));
            }
            if (D == 2 && g.nextRelease == g.slot + 1) {
                bool ready = false;
                for (auto& p : g.pending)
                    if (p->target == g.slot + 1 && FrameReady(*p)) ready = true;
                g.pacer.Early(ready);
            }
        }
        if (expected) g.pacer.Expected(pick && pick->target == g.slot);

        bool motion = st.synthMode == fs_shared::Synth_Motion;
        bool newFrame = false;
        if (pick && (!g.current || pick->seq > g.current->seq)) {
            Ingest(pick, motion);
            newFrame = true;
            statReal++;
            lastFrame = NowSec();
        }
        if (g.current) {
            bool onTime = newFrame && g.current->target == g.slot;
            float s = 0;
            if (g.prevTarget && g.current->target > g.prevTarget)
                s = float(double(g.slot - g.current->target) / double(g.current->target - g.prevTarget));
            s = std::clamp(s, 0.0f, 3.0f);
            // The pose SteamVR predicted for this display slot (from our own WaitGetPoses call)
            const TrackedDevicePose_t& hp = rp[k_unTrackedDeviceIndex_Hmd];
            const HmdMatrix34_t slotPose = (hp.bPoseIsValid ? hp.mDeviceToAbsoluteTracking : g.current->pose);
            bool warped = false;
            for (int e = 0; e < 2; e++) {
                OutEye& o = g.eyes[e];
                bool extrapolate = !onTime && motion && o.fv.flowValid && s > 0.01f;
                ProduceEye(o, e, extrapolate ? s : 0.0f, extrapolate && st.debugTint != 0, g.current->pose, slotPose);
                warped = warped || extrapolate;
            }
            if (warped) statSynth++;
            g.submitPose = slotPose;
            if (g.api == Api::Vulkan) WaitD3D11Idle();  // Vulkan side reads our output right away
            else g.ctx->Flush();
            for (int e = 0; e < 2; e++) {
                OutEye& o = g.eyes[e];
                if (!o.haveOut) continue;
                EVRCompositorError se;
                if (g.api == Api::Vulkan) {
                    se = SubmitEyeVulkan(P0, self, e, o, *g.current);
                } else {
                    VRTextureWithPose_t t{};
                    t.handle = o.out[o.outIdx].Get();
                    t.eType = TextureType_DirectX;
                    t.eColorSpace = g.current->cs;
                    t.mDeviceToAbsoluteTracking = g.submitPose;
                    se = CallSubmit(P0, self, e ? Eye_Right : Eye_Left, &t, nullptr, Submit_TextureWithPose);
                }
                if (se != VRCompositorError_None) {
                    if (g.submitErrors < 5) Log("Submit error %d", int(se));
                    if (++g.submitErrors > 30 && !g.failed) {
                        Log("compositor keeps rejecting frames; falling back to pass-through");
                        g.failed = true;
                        g.relCv.notify_all();
                    }
                } else {
                    g.submitErrors = 0;
                }
            }
        }
        CallPPH(P0, self);

        double now = NowSec();
        if (now - statT0 >= 1.0) {
            double dt = now - statT0;
            PublishStatus(1, D, hz, float(statReal / dt), float(statSynth / dt), apiName);
            static double lastDiag = 0;
            if (now - lastDiag >= 5.0) {
                lastDiag = now;
                size_t pend;
                {
                    std::lock_guard<std::mutex> lk(g.subMtx);
                    pend = g.pending.size();
                }
                Log("diag: slot=%llu div=%d rateMode=%d game fps=%.1f synth/s=%.1f lastAppCall=%.0f ms ago pending=%zu | "
                    "pose calls: Wait=%u GetLast=%u LastIdx=%u PredIds=%u PosesForFrame=%u SysPose=%u (overridden %u)",
                    (unsigned long long)g.slot, D, st.rateMode, statReal / dt, statSynth / dt,
                    (now - g.lastAppCall.load()) * 1000.0, pend, g_cnt.wait.exchange(0), g_cnt.last.exchange(0),
                    g_cnt.lastIdx.exchange(0), g_cnt.predIds.exchange(0), g_cnt.posesForFrame.exchange(0),
                    g_cnt.sysPose.exchange(0), g_cnt.sysPoseOverridden.exchange(0));
            }
            statReal = statSynth = 0;
            statT0 = now;
        }
    }
    Log("frame thread stopped (virt=%d failed=%d softStop=%d)", int(g.virt.load()), int(g.failed.load()), int(g.softStop.load()));
}

static void StartVirt() {
    {
        std::lock_guard<std::mutex> lk(g.relMtx);
        g.takenSeq = g.relSeq;
        memset(g.taken, 0, sizeof(g.taken));
    }
    g.nextRelease = g.slot;
    g.virt = true;
    g.thr = std::thread(FrameThread);
    Log("virtualization started");
}

static void StopVirt() {
    g.virt = false;
    g.relCv.notify_all();
    g.subCv.notify_all();
    if (g.thr.joinable()) g.thr.join();
    {
        std::lock_guard<std::mutex> lk(g.subMtx);
        g.pending.clear();
    }
    g.current.reset();
    std::lock_guard<std::mutex> lk(g.appMtx);
    g.buildingActive = false;
    Log("virtualization stopped");
    PublishStatus(0, 1, 0, 0, 0, "Disabled for this game");
}

// ---------------------------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------------------------
static EVRCompositorError Impl_Wait(int I, void* self, TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* gp, uint32_t gc) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) Log("first WaitGetPoses from the game (table %d, IVRCompositor_%03d)", I, g_tab[I].ver);
    g_cnt.wait++;
    g.lastAppCall = NowSec();
    if (g.virt && g.softStop) {  // watchdog: back to pass-through until the game submits frames again
        StopVirt();
        g.softStop = false;
        g.wantVirt = false;
    }
    bool run = ShouldRun() && !g.failed;
    if (g.virt && !run) {
        StopVirt();
        if (g.failed) PublishStatus(0, 1, 0, 0, 0, "Compositor rejected frames - fell back (see openvr_hook.log)");
    }
    if (!g.virt) {
        if (run && g.wantVirt && g_primary >= 0) StartVirt();
        else return CallWait(I, self, r, rc, gp, gc);
    }
    std::unique_lock<std::mutex> lk(g.relMtx);
    const double waitStart = NowSec();
    for (;;) {
        // Game-side safety net: no frame slot for 1 s means our frame thread is stuck. Take rendering back
        // immediately without waiting for that thread (it exits on its own if it ever continues).
        if (g.virt && NowSec() - waitStart > 1.0) {
            lk.unlock();
            Log("game waited >1 s for a frame slot; FrameSmooth's frame thread seems stuck - returning to normal rendering");
            g.failed = true;
            g.virt = false;
            if (g.thr.joinable()) g.thr.detach();
            PublishStatus(0, 1, 0, 0, 0, "Fell back to normal rendering (see openvr_hook.log)");
            return CallWait(I, self, r, rc, gp, gc);
        }
        if (!g.virt || g.failed || g.softStop) {
            lk.unlock();
            if (g.virt) {
                StopVirt();
                if (g.failed) PublishStatus(0, 1, 0, 0, 0, "Fell back to normal rendering (see openvr_hook.log)");
                g.softStop = false;
                g.wantVirt = false;
            }
            return CallWait(I, self, r, rc, gp, gc);
        }
        if (g.relSeq > g.takenSeq) {
            if (NowSec() - g.relAt <= g.relFresh) break;
            g.takenSeq = g.relSeq;
        }
        g.lastAppCall = NowSec();
        g.relCv.wait_for(lk, std::chrono::milliseconds(20));
    }
    g.takenSeq = g.relSeq;
    g.takenTarget = g.relTarget;
    memcpy(g.takenPoses, g.relPoses, sizeof(g.relPoses));
    g.taken[g.takenPos++ % 8] = g.relTarget;
    if (r) memcpy(r, g.relPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(rc, k_unMaxTrackedDeviceCount));
    if (gp) memcpy(gp, g.relPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(gc, k_unMaxTrackedDeviceCount));
    return VRCompositorError_None;
}

static EVRCompositorError Impl_GetLast(int I, void* self, TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* gp, uint32_t gc) {
    g_cnt.last++;
    if (!g.virt) return CallWait(I, self, r, rc, gp, gc, kGetLast);
    std::lock_guard<std::mutex> lk(g.relMtx);
    if (r) memcpy(r, g.takenPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(rc, k_unMaxTrackedDeviceCount));
    if (gp) memcpy(gp, g.takenPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(gc, k_unMaxTrackedDeviceCount));
    return VRCompositorError_None;
}

// Pose-prediction-ID API: while active, the "current frame" is the one we released to the game.
static EVRCompositorError Impl_PredIds(int I, void* self, uint32_t* render, uint32_t* game) {
    g_cnt.predIds++;
    auto& t = g_tab[I];
    if (!g.virt) return t.fn ? ((PFN_FnPredIds)t.orig[t.idxPredIds])(render, game) : ((PFN_PredIds)t.orig[t.idxPredIds])(self, render, game);
    std::lock_guard<std::mutex> lk(g.relMtx);
    uint32_t id = uint32_t(g.takenTarget) | 0x80000000u;  // our own ID space
    if (render) *render = id;
    if (game) *game = id;
    return VRCompositorError_None;
}

static EVRCompositorError Impl_PosesForFrame(int I, void* self, uint32_t id, TrackedDevicePose_t* p, uint32_t n) {
    g_cnt.posesForFrame++;
    auto& t = g_tab[I];
    if (!g.virt || !(id & 0x80000000u))
        return t.fn ? ((PFN_FnPosesForFrame)t.orig[t.idxPosesForFrame])(id, p, n)
                    : ((PFN_PosesForFrame)t.orig[t.idxPosesForFrame])(self, id, p, n);
    std::lock_guard<std::mutex> lk(g.relMtx);
    if (p) memcpy(p, g.takenPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(n, k_unMaxTrackedDeviceCount));
    return VRCompositorError_None;
}

static EVRCompositorError Impl_GetLastIdx(int I, void* self, TrackedDeviceIndex_t idx, TrackedDevicePose_t* o, TrackedDevicePose_t* og) {
    g_cnt.lastIdx++;
    if (!g.virt || idx >= k_unMaxTrackedDeviceCount) return CallLastIdx(I, self, idx, o, og);
    std::lock_guard<std::mutex> lk(g.relMtx);
    if (o) *o = g.takenPoses[idx];
    if (og) *og = g.takenPoses[idx];
    return VRCompositorError_None;
}

static EVRCompositorError Impl_Submit(int I, void* self, EVREye eye, const Texture_t* t, uint32_t arr, bool withArr,
                                      const VRTextureBounds_t* b, EVRSubmitFlags f) {
    static std::atomic<bool> first{true};
    if (first.exchange(false))
        Log("first Submit from the game (table %d, texture type %d, flags 0x%x, enabled=%d)", I, int(t ? t->eType : -1),
            unsigned(f), int(ShouldRun()));
    if (!g.virt) {
        if (!g.wantVirt && g_primary >= 0 && ShouldRun()) {
            if (Capturable(t, f)) {
                g.wantVirt = true;
                Log("game frames are capturable (%s); enabling",
                    t->eType == TextureType_DirectX12 ? "D3D12" : t->eType == TextureType_Vulkan ? "Vulkan" : "D3D11");
            } else if (!CaptureInitPending()) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    Log("game frames not capturable (texture type %d, flags 0x%x); staying pass-through", int(t ? t->eType : -1), unsigned(f));
                    int type = t ? int(t->eType) : -1;
                    PublishStatus(0, 1, 0, 0, 0,
                                  type == TextureType_Vulkan ? "Not supported: Vulkan OpenVR game (game runs normally)"
                                  : type == TextureType_OpenGL ? "Not supported: OpenGL game (game runs normally)"
                                                               : "Not supported for this game (game runs normally)");
                }
            }
        }
        return withArr ? CallSubmitAI(I, self, eye, t, arr, b, f) : CallSubmit(I, self, eye, t, b, f);
    }
    if (!Capturable(t, f)) return VRCompositorError_None;
    CaptureSubmit(eye, t, withArr ? arr : 0, b, f);
    return VRCompositorError_None;
}

static void Impl_PPH(int I, void* self) {
    if (!g.virt) CallPPH(I, self);
}

static EVRCompositorError Impl_Explicit(int I, void* self) {
    return g.virt ? VRCompositorError_None : CallExplicit(I, self);
}

template <int I>
struct Hooks {
    static EVRCompositorError Wait(void* s, TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* g2, uint32_t gc) { return Impl_Wait(I, s, r, rc, g2, gc); }
    static EVRCompositorError FnWait(TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* g2, uint32_t gc) { return Impl_Wait(I, nullptr, r, rc, g2, gc); }
    static EVRCompositorError Last(void* s, TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* g2, uint32_t gc) { return Impl_GetLast(I, s, r, rc, g2, gc); }
    static EVRCompositorError FnLast(TrackedDevicePose_t* r, uint32_t rc, TrackedDevicePose_t* g2, uint32_t gc) { return Impl_GetLast(I, nullptr, r, rc, g2, gc); }
    static EVRCompositorError LastIdx(void* s, TrackedDeviceIndex_t i, TrackedDevicePose_t* o, TrackedDevicePose_t* og) { return Impl_GetLastIdx(I, s, i, o, og); }
    static EVRCompositorError FnLastIdx(TrackedDeviceIndex_t i, TrackedDevicePose_t* o, TrackedDevicePose_t* og) { return Impl_GetLastIdx(I, nullptr, i, o, og); }
    static EVRCompositorError Submit(void* s, EVREye e, const Texture_t* t, const VRTextureBounds_t* b, EVRSubmitFlags f) { return Impl_Submit(I, s, e, t, 0, false, b, f); }
    static EVRCompositorError FnSubmit(EVREye e, const Texture_t* t, const VRTextureBounds_t* b, EVRSubmitFlags f) { return Impl_Submit(I, nullptr, e, t, 0, false, b, f); }
    static EVRCompositorError SubmitAI(void* s, EVREye e, const Texture_t* t, uint32_t a, const VRTextureBounds_t* b, EVRSubmitFlags f) { return Impl_Submit(I, s, e, t, a, true, b, f); }
    static EVRCompositorError FnSubmitAI(EVREye e, const Texture_t* t, uint32_t a, const VRTextureBounds_t* b, EVRSubmitFlags f) { return Impl_Submit(I, nullptr, e, t, a, true, b, f); }
    static void PPH(void* s) { Impl_PPH(I, s); }
    static void FnPPH() { Impl_PPH(I, nullptr); }
    static EVRCompositorError Explicit(void* s) { return Impl_Explicit(I, s); }
    static EVRCompositorError FnExplicit() { return Impl_Explicit(I, nullptr); }
    static EVRCompositorError PredIds(void* s, uint32_t* r, uint32_t* gm) { return Impl_PredIds(I, s, r, gm); }
    static EVRCompositorError FnPredIds(uint32_t* r, uint32_t* gm) { return Impl_PredIds(I, nullptr, r, gm); }
    static EVRCompositorError PosesForFrame(void* s, uint32_t id, TrackedDevicePose_t* p, uint32_t n) { return Impl_PosesForFrame(I, s, id, p, n); }
    static EVRCompositorError FnPosesForFrame(uint32_t id, TrackedDevicePose_t* p, uint32_t n) { return Impl_PosesForFrame(I, nullptr, id, p, n); }
};

struct HookSet {
    void *wait, *last, *lastIdx, *submit, *submitAI, *pph, *expl, *predIds, *posesForFrame;
};
static HookSet g_cpp[kMaxTables], g_fn[kMaxTables];

template <size_t... Is>
static void FillHookSets(std::index_sequence<Is...>) {
    ((g_cpp[Is] = HookSet{(void*)&Hooks<Is>::Wait, (void*)&Hooks<Is>::Last, (void*)&Hooks<Is>::LastIdx, (void*)&Hooks<Is>::Submit,
                          (void*)&Hooks<Is>::SubmitAI, (void*)&Hooks<Is>::PPH, (void*)&Hooks<Is>::Explicit,
                          (void*)&Hooks<Is>::PredIds, (void*)&Hooks<Is>::PosesForFrame},
      g_fn[Is] = HookSet{(void*)&Hooks<Is>::FnWait, (void*)&Hooks<Is>::FnLast, (void*)&Hooks<Is>::FnLastIdx, (void*)&Hooks<Is>::FnSubmit,
                         (void*)&Hooks<Is>::FnSubmitAI, (void*)&Hooks<Is>::FnPPH, (void*)&Hooks<Is>::FnExplicit,
                         (void*)&Hooks<Is>::FnPredIds, (void*)&Hooks<Is>::FnPosesForFrame}),
     ...);
}

// ---------------------------------------------------------------------------------------------
// IVRSystem::GetDeviceToAbsoluteTrackingPose: games that predict their own render pose get the same
// pose FrameSmooth reports to SteamVR for that frame (only while active and only for predicted poses;
// requests for the current pose, e.g. for physics, are passed through unchanged).
// ---------------------------------------------------------------------------------------------
struct SysTable {
    void** entries = nullptr;
    bool fn = false;
    int idx = 0;
    void* orig = nullptr;
};
static SysTable g_sysTab[8];
static int g_nSysTab = 0;

static void Impl_SysPose(int I, void* self, ETrackingUniverseOrigin o, float secs, TrackedDevicePose_t* p, uint32_t n) {
    if (!t_bypassPose) g_cnt.sysPose++;
    if (!t_bypassPose && g.virt && p && secs > 0.0005f && secs < 0.25f && int(o) == g_relOrigin.load()) {
        g_cnt.sysPoseOverridden++;
        std::lock_guard<std::mutex> lk(g.relMtx);
        memcpy(p, g.takenPoses, sizeof(TrackedDevicePose_t) * std::min<uint32_t>(n, k_unMaxTrackedDeviceCount));
        return;
    }
    auto& t = g_sysTab[I];
    if (t.fn) ((PFN_FnSysPose)t.orig)(o, secs, p, n);
    else ((PFN_SysPose)t.orig)(self, o, secs, p, n);
}

template <int I>
struct SysHooks {
    static void Pose(void* s, ETrackingUniverseOrigin o, float secs, TrackedDevicePose_t* p, uint32_t n) { Impl_SysPose(I, s, o, secs, p, n); }
    static void FnPose(ETrackingUniverseOrigin o, float secs, TrackedDevicePose_t* p, uint32_t n) { Impl_SysPose(I, nullptr, o, secs, p, n); }
};
static void* g_sysCpp[8] = {(void*)&SysHooks<0>::Pose, (void*)&SysHooks<1>::Pose, (void*)&SysHooks<2>::Pose, (void*)&SysHooks<3>::Pose,
                            (void*)&SysHooks<4>::Pose, (void*)&SysHooks<5>::Pose, (void*)&SysHooks<6>::Pose, (void*)&SysHooks<7>::Pose};
static void* g_sysFn[8] = {(void*)&SysHooks<0>::FnPose, (void*)&SysHooks<1>::FnPose, (void*)&SysHooks<2>::FnPose, (void*)&SysHooks<3>::FnPose,
                           (void*)&SysHooks<4>::FnPose, (void*)&SysHooks<5>::FnPose, (void*)&SysHooks<6>::FnPose, (void*)&SysHooks<7>::FnPose};

static void PatchSysTable(void** entries, bool fn, int idx, const char* name) {
    for (int i = 0; i < g_nSysTab; i++)
        if (g_sysTab[i].entries == entries) return;
    if (g_nSysTab >= 8) return;
    for (int i = 0; i < 8; i++)
        if (entries[idx] == g_sysCpp[i] || entries[idx] == g_sysFn[i]) return;
    int I = g_nSysTab++;
    SysTable& t = g_sysTab[I];
    t.entries = entries;
    t.fn = fn;
    t.idx = idx;
    t.orig = entries[idx];
    DWORD old;
    if (!VirtualProtect(&entries[idx], sizeof(void*), PAGE_READWRITE, &old)) { g_nSysTab--; return; }
    entries[idx] = fn ? g_sysFn[I] : g_sysCpp[I];
    VirtualProtect(&entries[idx], sizeof(void*), old, &old);
    Log("hooked %s %s::GetDeviceToAbsoluteTrackingPose (slot %d)", fn ? "FnTable" : "C++", name, idx);
}

// Our own pose queries must bypass the override. SteamVR's client library forwards older interface
// versions to the newest one internally, so a per-thread flag is used (works however the call is routed).
static void RealGetPose(ETrackingUniverseOrigin o, float secs, TrackedDevicePose_t* p, uint32_t n) {
    t_bypassPose = true;
    g_sys->GetDeviceToAbsoluteTrackingPose(o, secs, p, n);
    t_bypassPose = false;
}

static bool IsOurHook(void* p) {
    for (int i = 0; i < kMaxTables; i++)
        for (void* h : {g_cpp[i].wait, g_cpp[i].submit, g_fn[i].wait, g_fn[i].submit})
            if (h == p) return true;
    return false;
}

static int PatchTable(void** entries, bool fn, int ver) {
    for (int i = 0; i < g_nTab; i++)
        if (g_tab[i].entries == entries) return i;
    if (g_nTab >= kMaxTables || IsOurHook(entries[kWait])) return -1;
    int I = g_nTab++;
    Table& t = g_tab[I];
    t.entries = entries;
    t.fn = fn;
    t.ver = ver;
    // Method positions per interface version (from the official OpenVR headers):
    //   <=027: Submit 5, PostPresentHandoff 7, SubmitExplicitTimingData 42 (>=022)
    //    028: + SubmitWithArrayIndex at 6           -> Submit 5, SWAI 6, PPH 8, Explicit 43
    //    029: + GetSubmitTexture at 5               -> Submit 6, SWAI 7, PPH 9, Explicit 44
    //   028: GetLastPosePredictionIDs 50, GetPosesForFrame 51;  029: 51, 52
    if (ver >= 29) { t.idxSubmit = 6; t.idxSWAI = 7; t.idxPPH = 9; t.idxExplicit = 44; t.idxPredIds = 51; t.idxPosesForFrame = 52; }
    else if (ver == 28) { t.idxSubmit = 5; t.idxSWAI = 6; t.idxPPH = 8; t.idxExplicit = 43; t.idxPredIds = 50; t.idxPosesForFrame = 51; }
    else { t.idxSubmit = 5; t.idxSWAI = -1; t.idxPPH = 7; t.idxExplicit = ver >= 22 ? 42 : -1; }
    int n = std::max({t.idxExplicit, t.idxPPH, t.idxPosesForFrame}) + 1;
    memcpy(t.orig, entries, sizeof(void*) * n);
    const HookSet& h = fn ? g_fn[I] : g_cpp[I];
    DWORD old;
    if (!VirtualProtect(entries, sizeof(void*) * n, PAGE_READWRITE, &old)) { g_nTab--; return -1; }
    entries[kWait] = h.wait;
    entries[kGetLast] = h.last;
    entries[kGetLastIdx] = h.lastIdx;
    entries[t.idxSubmit] = h.submit;
    if (t.idxSWAI >= 0) entries[t.idxSWAI] = h.submitAI;
    entries[t.idxPPH] = h.pph;
    if (t.idxExplicit >= 0) entries[t.idxExplicit] = h.expl;
    if (t.idxPredIds >= 0) entries[t.idxPredIds] = h.predIds;
    if (t.idxPosesForFrame >= 0) entries[t.idxPosesForFrame] = h.posesForFrame;
    VirtualProtect(entries, sizeof(void*) * n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("hooked %s IVRCompositor_%03d (table %d)", fn ? "FnTable" : "C++", ver, I);
    return I;
}

typedef void* (VR_CALLTYPE* PFN_GetGeneric)(const char*, EVRInitError*);

static DWORD WINAPI InitThread(void*) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring wp(path);
    std::wstring wn = wp.substr(wp.find_last_of(L"\\/") + 1);
    g_exeName.assign(wn.begin(), wn.end());
    for (auto& c : g_exeName) c = char(tolower((unsigned char)c));
    g_shared = fs_shared::Open();
    FillHookSets(std::make_index_sequence<kMaxTables>{});
    Log("FrameSmooth OpenVR hook loaded into %s", g_exeName.c_str());

    for (int tries = 0; tries < 600; tries++) {  // up to ~5 minutes for the game to initialize OpenVR
        HMODULE m = GetModuleHandleW(L"openvr_api.dll");
        auto getGeneric = m ? (PFN_GetGeneric)GetProcAddress(m, "VR_GetGenericInterface") : nullptr;
        bool logThis = tries < 5 || tries % 20 == 0;
        if (!getGeneric && logThis) Log("attempt %d: openvr_api.dll %s, VR_GetGenericInterface not found", tries, m ? "loaded" : "not loaded");
        if (getGeneric) {
            EVRInitError errSys = VRInitError_None, errComp = VRInitError_None, err = VRInitError_None;
            g_sys = static_cast<IVRSystem*>(getGeneric(IVRSystem_Version, &errSys));
            void* c28 = getGeneric("IVRCompositor_028", &errComp);
            if (logThis || (g_sys && c28))
                Log("attempt %d: %s=%p (err %d), IVRCompositor_028=%p (err %d)", tries, IVRSystem_Version, (void*)g_sys,
                    int(errSys), c28, int(errComp));
            if (g_sys && c28) {
                constexpr int kMaxKnownVersion = 29;  // newest IVRCompositor layout we know
                for (int ver = 19; ver <= kMaxKnownVersion; ver++) {
                    char name[64];
                    sprintf_s(name, "IVRCompositor_%03d", ver);
                    if (void* p = getGeneric(name, &err)) {
                        int I = PatchTable(*static_cast<void***>(p), false, ver);
                        if (I >= 0 && ver >= 28) { g_primary = I; g_primarySelf = p; }  // our thread uses the newest
                    }
                    sprintf_s(name, "FnTable:IVRCompositor_%03d", ver);
                    if (void* p = getGeneric(name, &err)) PatchTable(static_cast<void**>(p), true, ver);
                }
                // Newer, unknown layouts are never patched (wrong method positions could break the game).
                for (int ver = kMaxKnownVersion + 1; ver <= kMaxKnownVersion + 6; ver++) {
                    char name[64];
                    sprintf_s(name, "IVRCompositor_%03d", ver);
                    if (getGeneric(name, &err)) Log("note: SteamVR offers IVRCompositor_%03d (unknown layout, not hooked)", ver);
                }
                // IVRSystem::GetDeviceToAbsoluteTrackingPose (slot 11 in _022, 12 in _026)
                const struct { int ver, idx; } sysVers[] = {{22, 11}, {26, 12}};
                for (auto& sv : sysVers) {
                    char name[64];
                    sprintf_s(name, "IVRSystem_%03d", sv.ver);
                    if (void* p = getGeneric(name, &err)) PatchSysTable(*static_cast<void***>(p), false, sv.idx, name);
                    char fname[80];
                    sprintf_s(fname, "FnTable:IVRSystem_%03d", sv.ver);
                    if (void* p = getGeneric(fname, &err)) PatchSysTable(static_cast<void**>(p), true, sv.idx, name);
                }
                if (g_primary < 0) {
                    Log("could not hook IVRCompositor_028/029; staying inactive");
                    PublishStatus(0, 1, 0, 0, 0, "OpenVR hook failed (see openvr_hook.log)");
                } else {
                    Log("frame thread will use IVRCompositor_%03d", g_tab[g_primary].ver);
                    PublishStatus(0, 1, 0, 0, 0, "Hooked, waiting for frames");
                }
                return 0;
            }
        }
        Sleep(500);
    }
    Log("OpenVR never initialized in this process");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
