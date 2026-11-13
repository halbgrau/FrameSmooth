// Experiment: does SteamVR accept D3D11 textures from a process that has already submitted Vulkan textures?
// Usage: fs_submit_test.exe d3d11     -> D3D11 only (control)
//        fs_submit_test.exe vkmix     -> Vulkan for ~2 s, then D3D11 for ~3 s
// Prints error counts per phase. Shows solid colours in the headset (green = D3D11, blue = Vulkan).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <openvr.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static const uint32_t TW = 1024, TH = 1024;

struct D3D {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* tex = nullptr;
};

static bool InitD3D(D3D& d) {
    int32_t idx = 0;
    vr::VRSystem()->GetDXGIOutputInfo(&idx);
    IDXGIFactory1* f = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f);
    IDXGIAdapter1* ad = nullptr;
    f->EnumAdapters1(idx, &ad);
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d.dev, nullptr, &d.ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08lx\n", hr); return false; }
    // same configuration as FrameSmooth's output textures
    D3D11_TEXTURE2D_DESC td{};
    td.Width = TW; td.Height = TH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = d.dev->CreateTexture2D(&td, nullptr, &d.tex);
    if (FAILED(hr)) { printf("CreateTexture2D failed 0x%08lx\n", hr); return false; }
    ID3D11RenderTargetView* rtv = nullptr;
    d.dev->CreateRenderTargetView(d.tex, nullptr, &rtv);
    float green[4] = {0.0f, 0.6f, 0.0f, 1.0f};
    d.ctx->ClearRenderTargetView(rtv, green);
    d.ctx->Flush();
    return true;
}

struct VK {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkImage img = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
};

// Fake Vulkan VR game: clears the eye image to a colour that changes every frame, submits like a real game.
static void VkGameLoop(VK& v, double seconds) {
#define GF(n) auto n = (PFN_vk##n)v.gdpa(v.dev, "vk" #n)
    GF(BeginCommandBuffer); GF(EndCommandBuffer); GF(ResetCommandBuffer); GF(CmdPipelineBarrier); GF(CmdClearColorImage);
    GF(QueueSubmit); GF(WaitForFences); GF(ResetFences);
    vr::VRVulkanTextureData_t vd{};
    vd.m_nImage = (uint64_t)v.img; vd.m_pDevice = v.dev; vd.m_pPhysicalDevice = v.phys; vd.m_pInstance = v.inst;
    vd.m_pQueue = v.queue; vd.m_nQueueFamilyIndex = v.family; vd.m_nWidth = TW; vd.m_nHeight = TH;
    vd.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM; vd.m_nSampleCount = 1;
    vr::Texture_t vt{&vd, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
    std::map<int, int> errs;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    int frames = 0;
    double maxGap = 0, last = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        double now = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (now > seconds) break;
        vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        // like engines that predict their own render pose
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::VRCompositor()->GetTrackingSpace(), 0.025f, poses,
                                                         vr::k_unMaxTrackedDeviceCount);
        QueryPerformanceCounter(&t);
        now = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (frames > 0 && now - last > maxGap) maxGap = now - last;
        last = now;
        WaitForFences(v.dev, 1, &v.fence, VK_TRUE, UINT64_MAX);
        ResetFences(v.dev, 1, &v.fence);
        ResetCommandBuffer(v.cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        BeginCommandBuffer(v.cmd, &bi);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED, v.img, r};
        CmdPipelineBarrier(v.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        float ph = float(now) * 2.0f;
        VkClearColorValue c{{0.5f + 0.5f * sinf(ph), 0.5f + 0.5f * sinf(ph + 2.1f), 0.5f + 0.5f * sinf(ph + 4.2f), 1.0f}};
        CmdClearColorImage(v.cmd, v.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        CmdPipelineBarrier(v.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        EndCommandBuffer(v.cmd);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &v.cmd;
        QueueSubmit(v.queue, 1, &si, v.fence);
        for (int e = 0; e < 2; e++) errs[int(vr::VRCompositor()->Submit(vr::EVREye(e), &vt, nullptr, vr::Submit_Default))]++;
        frames++;
        if (frames % 450 == 0) { printf("t=%.0fs frames=%d\n", now, frames); fflush(stdout); }
    }
    printf("RESULT vkgame: %d frames in %.0f s (%.1f fps), longest gap %.0f ms, submit results:", frames, seconds,
           frames / seconds, maxGap * 1000.0);
    for (auto& kv : errs) printf(" [%d]=%d", kv.first, kv.second);
    printf("\n");
}

static std::vector<std::string> Split(const char* s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    std::string w;
    while (is >> w) v.push_back(w);
    return v;
}

static bool InitVk(VK& v) {
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) { printf("vulkan-1.dll not found\n"); return false; }
    v.gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)v.gipa(nullptr, "vkCreateInstance");
    char buf[4096];
    vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof(buf));
    auto iext = Split(buf);
    std::vector<const char*> ip;
    for (auto& e : iext) ip.push_back(e.c_str());
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = uint32_t(ip.size());
    ici.ppEnabledExtensionNames = ip.data();
    if (createInstance(&ici, nullptr, &v.inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return false; }
#define IF(n) auto n = (PFN_vk##n)v.gipa(v.inst, "vk" #n)
    IF(EnumeratePhysicalDevices); IF(GetPhysicalDeviceQueueFamilyProperties); IF(CreateDevice); IF(GetDeviceProcAddr);
    IF(GetPhysicalDeviceMemoryProperties);
    uint64_t hmdPhys = 0;
    vr::VRSystem()->GetOutputDevice(&hmdPhys, vr::TextureType_Vulkan, v.inst);
    v.phys = (VkPhysicalDevice)hmdPhys;
    if (!v.phys) { uint32_t n = 1; EnumeratePhysicalDevices(v.inst, &n, &v.phys); }
    uint32_t qn = 0;
    GetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    GetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v.family = i; break; }
    vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(v.phys, buf, sizeof(buf));
    auto dext = Split(buf);
    std::vector<const char*> dp;
    for (auto& e : dext) dp.push_back(e.c_str());
    printf("device extensions required by SteamVR: %s\n", buf);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, v.family, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dp.size()); dci.ppEnabledExtensionNames = dp.data();
    if (CreateDevice(v.phys, &dci, nullptr, &v.dev) != VK_SUCCESS) { printf("vkCreateDevice failed\n"); return false; }
#define DF(n) auto n = (PFN_vk##n)GetDeviceProcAddr(v.dev, "vk" #n)
    DF(GetDeviceQueue); DF(CreateImage); DF(GetImageMemoryRequirements); DF(AllocateMemory); DF(BindImageMemory);
    DF(CreateCommandPool); DF(AllocateCommandBuffers); DF(BeginCommandBuffer); DF(EndCommandBuffer);
    DF(CmdPipelineBarrier); DF(CmdClearColorImage); DF(QueueSubmit); DF(QueueWaitIdle);
    GetDeviceQueue(v.dev, v.family, 0, &v.queue);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {TW, TH, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CreateImage(v.dev, &ii, nullptr, &v.img);
    VkMemoryRequirements req;
    GetImageMemoryRequirements(v.dev, v.img, &req);
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(v.phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    VkDeviceMemory mem;
    AllocateMemory(v.dev, &mai, nullptr, &mem);
    BindImageMemory(v.dev, v.img, mem, 0);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, v.family};
    VkCommandPool pool;
    CreateCommandPool(v.dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    AllocateCommandBuffers(v.dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    BeginCommandBuffer(cmd, &bi);
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                           VK_QUEUE_FAMILY_IGNORED, v.img, r};
    CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue blue{{0.0f, 0.0f, 0.6f, 1.0f}};
    CmdClearColorImage(cmd, v.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    EndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    QueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE);
    QueueWaitIdle(v.queue);
    DF(CreateFence);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    CreateFence(v.dev, &fci, nullptr, &v.fence);
    AllocateCommandBuffers(v.dev, &cai, &v.cmd);
    v.gdpa = GetDeviceProcAddr;
    return true;
}

static void Phase(const char* name, double seconds, const vr::Texture_t& tex) {
    std::map<int, int> errs;
    int frames = 0;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    for (;;) {
        QueryPerformanceCounter(&t);
        if (double(t.QuadPart - t0.QuadPart) / f.QuadPart > seconds) break;
        vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        vr::VRTextureWithPose_t tp{};
        static_cast<vr::Texture_t&>(tp) = tex;
        tp.mDeviceToAbsoluteTracking = poses[0].mDeviceToAbsoluteTracking;
        for (int e = 0; e < 2; e++) {
            vr::EVRCompositorError err = vr::VRCompositor()->Submit(vr::EVREye(e), &tp, nullptr, vr::Submit_TextureWithPose);
            errs[int(err)]++;
        }
        vr::VRCompositor()->PostPresentHandoff();
        frames++;
    }
    printf("RESULT %s: %d frames, submit results:", name, frames);
    for (auto& kv : errs) printf(" [%d]=%d", kv.first, kv.second);
    printf("\n");
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "d3d11";
    if (mode == "overlay") {  // background client: prints the state of the FrameSmooth dashboard overlay
        vr::EVRInitError be = vr::VRInitError_None;
        vr::VR_Init(&be, vr::VRApplication_Background);
        if (be != vr::VRInitError_None) { printf("VR_Init failed: %d\n", int(be)); return 1; }
        auto* ov = vr::VROverlay();
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid, th = vr::k_ulOverlayHandleInvalid;
        printf("find main: err=%d handle=%llx\n", int(ov->FindOverlay("framesmooth.dashboard", &h)), (unsigned long long)h);
        printf("find thumb: err=%d handle=%llx\n", int(ov->FindOverlay("framesmooth.dashboard.thumb", &th)), (unsigned long long)th);
        if (h != vr::k_ulOverlayHandleInvalid) {
            float wm = 0, a = 0;
            uint32_t tw = 0, tht = 0;
            ov->GetOverlayWidthInMeters(h, &wm);
            ov->GetOverlayAlpha(h, &a);
            printf("visible=%d activeDashboard=%d width=%.2fm alpha=%.2f flags=0x%x\n", int(ov->IsOverlayVisible(h)),
                   int(ov->IsActiveDashboardOverlay(h)), wm, a, 0);
            vr::EVROverlayError te = ov->GetOverlayTextureSize(h, &tw, &tht);
            printf("texture size err=%d %ux%u\n", int(te), tw, tht);
            uint32_t iw = 0, ih = 0;
            std::vector<uint8_t> img(1024 * 1024 * 4);
            vr::EVROverlayError ie = ov->GetOverlayImageData(h, img.data(), uint32_t(img.size()), &iw, &ih);
            printf("image data err=%d %ux%u\n", int(ie), iw, ih);
            if (ie == vr::VROverlayError_None && iw && ih) {
                uint64_t sum = 0, alphaSum = 0;
                for (uint32_t i = 0; i < iw * ih; i++) { sum += img[i * 4] + img[i * 4 + 1] + img[i * 4 + 2]; alphaSum += img[i * 4 + 3]; }
                printf("avg rgb=%.1f avg alpha=%.1f\n", double(sum) / (iw * ih * 3), double(alphaSum) / (iw * ih));
                if (argc > 2) {  // write raw RGBA for inspection
                    FILE* f = fopen(argv[2], "wb");
                    if (f) { fwrite(&iw, 4, 1, f); fwrite(&ih, 4, 1, f); fwrite(img.data(), 1, size_t(iw) * ih * 4, f); fclose(f); }
                }
            }
        }
        vr::VR_Shutdown();
        return 0;
    }
    if (mode == "proj") {  // background client: prints the projection conventions of the connected headset
        vr::EVRInitError be = vr::VRInitError_None;
        vr::VR_Init(&be, vr::VRApplication_Background);
        if (be != vr::VRInitError_None) { printf("VR_Init failed: %d\n", int(be)); return 1; }
        for (int e = 0; e < 2; e++) {
            float l, r, t, b;
            vr::VRSystem()->GetProjectionRaw(vr::EVREye(e), &l, &r, &t, &b);
            vr::HmdMatrix44_t m = vr::VRSystem()->GetProjectionMatrix(vr::EVREye(e), 0.1f, 100.0f);
            vr::HmdMatrix34_t e2h = vr::VRSystem()->GetEyeToHeadTransform(vr::EVREye(e));
            printf("eye %d raw: left=%.4f right=%.4f top=%.4f bottom=%.4f\n", e, l, r, t, b);
            printf("eye %d projection: m00=%.4f m02=%.4f m11=%.4f m12=%.4f\n", e, m.m[0][0], m.m[0][2], m.m[1][1], m.m[1][2]);
            printf("eye %d eyeToHead rot: [%.4f %.4f %.4f] [%.4f %.4f %.4f] [%.4f %.4f %.4f] pos=(%.4f %.4f %.4f)\n", e,
                   e2h.m[0][0], e2h.m[0][1], e2h.m[0][2], e2h.m[1][0], e2h.m[1][1], e2h.m[1][2], e2h.m[2][0], e2h.m[2][1],
                   e2h.m[2][2], e2h.m[0][3], e2h.m[1][3], e2h.m[2][3]);
        }
        vr::VR_Shutdown();
        return 0;
    }
    vr::EVRInitError e = vr::VRInitError_None;
    vr::VR_Init(&e, vr::VRApplication_Scene);
    if (e != vr::VRInitError_None) { printf("VR_Init failed: %d\n", int(e)); return 1; }
    D3D d;
    if (mode == "vkgame") {
        VK v;
        if (!InitVk(v)) return 1;
        double secs = argc > 2 ? atof(argv[2]) : 40.0;
        VkGameLoop(v, secs);
    } else if (mode == "vkmix") {
        VK v;
        if (!InitVk(v)) return 1;
        vr::VRVulkanTextureData_t vd{};
        vd.m_nImage = (uint64_t)v.img; vd.m_pDevice = v.dev; vd.m_pPhysicalDevice = v.phys; vd.m_pInstance = v.inst;
        vd.m_pQueue = v.queue; vd.m_nQueueFamilyIndex = v.family; vd.m_nWidth = TW; vd.m_nHeight = TH;
        vd.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM; vd.m_nSampleCount = 1;
        vr::Texture_t vt{&vd, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        Phase("vulkan", 2.0, vt);
        if (!InitD3D(d)) return 1;
        vr::Texture_t dt{d.tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
        Phase("d3d11-after-vulkan", 3.0, dt);
    } else {
        if (!InitD3D(d)) return 1;
        vr::Texture_t dt{d.tex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
        Phase("d3d11-only", 3.0, dt);
    }
    vr::VR_Shutdown();
    return 0;
}
