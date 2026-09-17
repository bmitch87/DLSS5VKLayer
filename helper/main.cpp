// dlssnr_helper.exe — Windows-side DLSSNR (Feature 18) service.
// Owns its own Vulkan device + the nvngx_dlssnr.dll snippet (verified
// standalone_runner sequence), waits on the shared-memory frame queue written
// by VK_LAYER_NV_dlssnr, runs the neural pass, and returns processed frames.
#include "ngx_snippet.h"
#include "guard.h"
#include "logging.h"
#include "../common/breadcrumbs.h"
#include "../common/frame_trace.h"
#include "../common/shm_protocol.h"
#include "mvec_deadzone_spv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <vector>

using namespace dlssnr;

static bool TimeEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_TIME");
        return p && p[0] == '1';
    }();
    return v;
}

static int TimeInterval() {
    static const int v = [] {
        const char* p = getenv("DLSSNR_TIME_EVERY");
        return p && *p ? atoi(p) : 30;
    }();
    return v > 0 ? v : 30;
}

// The helper's half of the kill switches (the layer holds DLSSNR_DRY_RUN and
// DLSSNR_NO_ROUNDTRIP; see layer_linux/src/layer.cpp).
//
// This one cuts between the transport and the model, which is the last division the other two
// cannot make: NO_ROUNDTRIP removes this process entirely, so a symptom that survives it and
// disappears here belongs to the DLL, and one that survives both belongs to our Vulkan work in
// here -- the import, the upload, the format conversion, the readback.
//
// The output is left equal to the input, which is the whole chain skipped rather than one pass of
// it: the passes ping-pong, so an evaluate skipped in the middle would leave a later pass reading a
// surface nothing wrote. Equal output is also what makes the switch checkable from outside -- the
// composed frame is the game's own frame, pixel for pixel.
static bool SkipEvaluate() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_SKIP_EVALUATE");
        return p && p[0] == '1';
    }();
    return v;
}

// The scene-cut detector's threshold now lives in the header so it can be moved while a game is
// running. The two environment variables it used to be are kept as a STARTUP SEED only -- they set
// the header's value once and then stop mattering.
//
// That ordering is deliberate. A variable that kept winning would make the GUI's control silently
// dead for anyone who had the variable exported, which is the failure this whole item exists to
// remove; and dropping the variables outright would break every recipe already written down. So:
// the environment chooses where the session starts, the header decides from then on, and the log
// says when the environment did anything at all.
// 0 off, 1 a 64px checkerboard, 2 all zero, 3 all 255.
//
// The checkerboard answers "does this key do anything"; the two solid fills answer "which way
// round is it", which is the half DXL-05 asks for and no document states.
static int ProbeControlMask() {
    static const int v = [] {
        const char* p = getenv("DLSSNR_PROBE_CONTROLMASK");
        return p && *p ? atoi(p) : 0;
    }();
    return v;
}

static void SeedSceneCutFromEnv(ShmHeader* hdr) {
    if (!hdr) return;
    const char* off = getenv("DLSSNR_SCENE_CUT");
    const char* thr = getenv("DLSSNR_SCENE_CUT_THRESHOLD");
    if (off && off[0] == '0') {
        hdr->sceneCutThreshold.store(0);
        Log("[mvec] DLSSNR_SCENE_CUT=0: scene-cut detection off for this session (threshold 0). "
            "Set it above zero to turn it back on without restarting.");
        return;
    }
    if (thr && *thr) {
        const long v = strtol(thr, nullptr, 10);
        const uint32_t t = v > 0 && v < 256 ? uint32_t(v) : kSceneCutThresholdDefault;
        hdr->sceneCutThreshold.store(t);
        Log("[mvec] DLSSNR_SCENE_CUT_THRESHOLD=%s: seeding the scene-cut threshold at %u", thr, t);
    }
}

// Breadcrumbs, this process's own. See common/breadcrumbs.h.
//
// From the shared-memory side a hung helper and a hung layer look identical -- seq_req ahead of
// seq_resp and nothing else to read -- so each side records its own path and each says which step
// it stopped on. The stages here are the helper's frame, not the layer's.
static dlssnr::Breadcrumbs g_crumbs;

// The frame trace. See common/frame_trace.h. One ring per process, so the helper's lives here and
// the layer's lives in the game -- which is the arrangement that makes a per-process trace easy
// rather than a thing to coordinate.
static dlssnr::FrameTrace g_trace;

// GPU-side checkpoints, off unless asked for. Same switch, same reasoning and the same name as the
// layer's: the CPU ring says which step this process was standing on, checkpoints say where the GPU
// was when a device was lost. Each marker is a real command in the buffer, so it is not free the way
// the ring is.
static bool CheckpointsEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_CHECKPOINTS");
        return p && p[0] == '1';
    }();
    return v;
}

static void DumpCrumbs(const char* why) {
    Log("[crumbs] %s -- last steps in the helper's frame, newest first:", why);
    bool any = false;
    g_crumbs.Dump([&](const char* stage, uint32_t detail, double ageMs) {
        any = true;
        Log("[crumbs]   %-15s %8u  %9.2f ms ago", stage, detail, ageMs);
    });
    if (!any) Log("[crumbs]   (nothing recorded)");
}

static double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

static inline void CpuYield() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    Sleep(0);
#endif
}

// ---------------------------------------------------------------------------
// Shared memory transport (must match layer_linux/src/layer.cpp)
// ---------------------------------------------------------------------------
struct ShmMap {
    HANDLE mapping = nullptr;
    HANDLE file = nullptr;
    void* base = nullptr;
    ShmHeader* hdr = nullptr;
    uint8_t* inPixels = nullptr;
    uint8_t* outPixels = nullptr;
};

static bool ShmOpen(ShmMap& s) {
    // DLSSNR_SHM holds the POSIX path (used by the Linux layer); translate to
    // the Wine-visible drive path (Z:\...) for CreateFileW.
    const char* posix = getenv("DLSSNR_SHM");
    std::string p = (posix && *posix) ? posix : ShmDefaultPath();
    std::wstring winPath = L"Z:";
    for (char c : p) winPath += (c == '/') ? L'\\' : (wchar_t)c;
    std::wstring dir = winPath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir.resize(slash);
        if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    }
    s.file = CreateFileW(winPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) { Log("[helper] open %ls failed (%lu)", winPath.c_str(), GetLastError()); return false; }
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)ShmTotalBytes();
    SetFilePointerEx(s.file, size, nullptr, FILE_BEGIN);
    SetEndOfFile(s.file);
    s.mapping = CreateFileMappingW(s.file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!s.mapping) { Log("[helper] CreateFileMapping failed"); return false; }
    // Windows hands out views at the 64 KiB allocation granularity, but the API does not promise it
    // and Wine does not guarantee it. The transport import (VK_EXT_external_memory_host) demands a
    // 64 KiB-aligned host pointer, so ask for aligned addresses first and keep only views that
    // actually land aligned; a base that does not simply means the helper keeps its staging copies.
    s.base = nullptr;
    for (uintptr_t hint = 0x200000000000ull; hint < 0x200000000000ull + (8ull << 20); hint += 64ull << 10) {
        void* b = MapViewOfFileEx(s.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0, (void*)hint);
        if (!b) continue;
        if ((reinterpret_cast<uintptr_t>(b) & ((64ull << 10) - 1)) == 0) { s.base = b; break; }
        UnmapViewOfFile(b);
    }
    if (!s.base) s.base = MapViewOfFile(s.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (!s.base) { Log("[helper] MapViewOfFile failed"); return false; }
    s.hdr = (ShmHeader*)s.base;
    s.inPixels = (uint8_t*)s.base + kHeaderBytes;
    s.outPixels = s.inPixels + kMaxFrame;
    if (s.hdr->magic.load() != kShmMagic || s.hdr->version.load() != kShmVersion ||
        s.hdr->passes.load() == 0) {
        // Loud, for the same reason the layer says it: a live process on the other side of a version
        // mismatch silently resets this one's header back, and every setting looks dead.
        if (s.hdr->magic.load() == kShmMagic && s.hdr->version.load() != kShmVersion)
            Log("[helper] header is version %u but this helper is v%u -- another process is out of "
                "date, re-initialising it; update the layer, the helper and the GUI together",
                s.hdr->version.load(), kShmVersion);
        ShmInitDefaults(s.hdr);
    }
    if (s.hdr->quit.load()) Log("[helper] clearing stale quit flag");
    s.hdr->quit.store(0);
    // Pick up where the layer is, without claiming a frame this helper never answered. A response
    // that runs ahead of the request is a stale mapping; a response that matches a request the old
    // helper never marked good would tell the layer to compose a frame that was never produced.
    {
        const uint32_t req = s.hdr->seq_req.load();
        const uint32_t resp = s.hdr->seq_resp.load();
        const uint32_t ok = s.hdr->seq_ok.load();
        if (resp > req) s.hdr->seq_resp.store(req);
        else if (req > 0 && resp == req && ok < req) s.hdr->seq_resp.store(req - 1);
        else s.hdr->seq_resp.store(req);
    }
    // Announced before anything slow happens. The first frame at a new size makes the model load a
    // 165 MB library and build a feature -- hundreds of milliseconds at least -- during which this
    // process ticks no heartbeat because it is busy. A layer inferring liveness from heartbeats alone
    // concludes nobody is there at exactly the moment the helper is working hardest, which is how a
    // running helper came to be ignored.
    s.hdr->helperState.store(kHelperStarting);
    s.hdr->controlSeq.fetch_add(1);
    s.hdr->heartbeat.fetch_add(1);
    Log("[helper] shm attached: %ls", winPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan context (from standalone_runner/main.cpp, verified)
// ---------------------------------------------------------------------------
static HMODULE g_vkModule = nullptr;
static PFN_vkGetInstanceProcAddr g_gipa = nullptr;

#define VK_FN(name) static PFN_##name name = nullptr;
VK_FN(vkCreateInstance) VK_FN(vkDestroyInstance) VK_FN(vkEnumeratePhysicalDevices)
VK_FN(vkGetPhysicalDeviceProperties) VK_FN(vkGetPhysicalDeviceFormatProperties) VK_FN(vkEnumerateDeviceExtensionProperties)
VK_FN(vkGetPhysicalDeviceQueueFamilyProperties) VK_FN(vkCreateDevice) VK_FN(vkDestroyDevice)
VK_FN(vkGetDeviceQueue) VK_FN(vkCreateCommandPool) VK_FN(vkDestroyCommandPool)
VK_FN(vkAllocateCommandBuffers) VK_FN(vkBeginCommandBuffer) VK_FN(vkEndCommandBuffer) VK_FN(vkResetCommandBuffer)
VK_FN(vkQueueSubmit) VK_FN(vkCreateFence) VK_FN(vkDestroyFence) VK_FN(vkWaitForFences)
VK_FN(vkResetFences) VK_FN(vkCreateImage) VK_FN(vkDestroyImage) VK_FN(vkCreateImageView) VK_FN(vkDestroyImageView)
VK_FN(vkGetImageMemoryRequirements) VK_FN(vkAllocateMemory) VK_FN(vkFreeMemory)
VK_FN(vkMapMemory) VK_FN(vkUnmapMemory) VK_FN(vkBindImageMemory)
VK_FN(vkGetImageSubresourceLayout)
VK_FN(vkCreateBuffer) VK_FN(vkDestroyBuffer) VK_FN(vkGetBufferMemoryRequirements)
VK_FN(vkBindBufferMemory) VK_FN(vkCmdCopyBufferToImage) VK_FN(vkCmdCopyImageToBuffer)
VK_FN(vkGetMemoryHostPointerPropertiesEXT)
VK_FN(vkGetMemoryFdKHR) VK_FN(vkGetMemoryFdPropertiesKHR)
VK_FN(vkCmdCopyImage) VK_FN(vkCmdPipelineBarrier) VK_FN(vkDeviceWaitIdle)
VK_FN(vkGetPhysicalDeviceProperties2) VK_FN(vkGetPhysicalDeviceMemoryProperties2)
VK_FN(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
VK_FN(vkCreateOpticalFlowSessionNV) VK_FN(vkDestroyOpticalFlowSessionNV)
VK_FN(vkBindOpticalFlowSessionImageNV) VK_FN(vkCmdOpticalFlowExecuteNV) VK_FN(vkCmdBlitImage)
VK_FN(vkCmdPipelineBarrier2) VK_FN(vkQueueSubmit2) VK_FN(vkCmdWriteTimestamp2)
VK_FN(vkCreateQueryPool) VK_FN(vkDestroyQueryPool) VK_FN(vkCmdResetQueryPool)
VK_FN(vkCmdWriteTimestamp) VK_FN(vkCmdCopyQueryPoolResults)
VK_FN(vkCreateSemaphore) VK_FN(vkDestroySemaphore) VK_FN(vkCmdClearColorImage)
VK_FN(vkCreateShaderModule) VK_FN(vkDestroyShaderModule)
VK_FN(vkCreatePipelineLayout) VK_FN(vkDestroyPipelineLayout)
VK_FN(vkCreateComputePipelines) VK_FN(vkDestroyPipeline)
VK_FN(vkCmdBindPipeline) VK_FN(vkCmdDispatch) VK_FN(vkCmdBindDescriptorSets)
VK_FN(vkCmdPushDescriptorSetKHR)
// VK_NV_device_diagnostic_checkpoints: null unless the device enabled it, which
// happens only under DLSSNR_CHECKPOINTS=1.
VK_FN(vkCmdSetCheckpointNV) VK_FN(vkGetQueueCheckpointDataNV)
VK_FN(vkCreateDescriptorSetLayout) VK_FN(vkDestroyDescriptorSetLayout)
VK_FN(vkCreateDescriptorPool) VK_FN(vkDestroyDescriptorPool)
VK_FN(vkAllocateDescriptorSets) VK_FN(vkUpdateDescriptorSets)
#undef VK_FN



struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect() const {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
};

struct VkCtx {
    VkInstance instance = nullptr;
    VkPhysicalDevice physical = nullptr;
    VkDevice device = nullptr;
    VkQueue queue = nullptr;
    uint32_t queueFamily = 0;
    VkQueue opticalQueue = VK_NULL_HANDLE;
    uint32_t opticalQueueFamily = UINT32_MAX;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPool cmdPoolFlow = VK_NULL_HANDLE;
    VkCommandBuffer cmdScratch = VK_NULL_HANDLE;
    VkCommandBuffer cmdCreate = VK_NULL_HANDLE;
    VkCommandBuffer cmdEval = VK_NULL_HANDLE;
    VkCommandBuffer cmdFlow = VK_NULL_HANDLE;
    VkCommandBuffer cmdFlowPost = VK_NULL_HANDLE;
    // Fence ring: every submit takes the next fence; the CPU only blocks on a
    // fence when its result is genuinely needed (never per-submit).
    static constexpr uint32_t kFenceRing = 8;
    VkFence fences[kFenceRing] = {};
    uint32_t fenceCursor = 0;
    // Graphics -> optical-flow -> graphics handoff for the async NVOF stages.
    VkSemaphore semPrep = VK_NULL_HANDLE;
    VkSemaphore semFlow = VK_NULL_HANDLE;
    VkBuffer uploadStaging = VK_NULL_HANDLE;
    VkBuffer readStaging = VK_NULL_HANDLE;
    VkDeviceMemory uploadMem = VK_NULL_HANDLE;
    VkDeviceMemory readMem = VK_NULL_HANDLE;
    void* uploadMap = nullptr;
    void* readMap = nullptr;
    size_t stagingSize = 0;
    bool opticalFlow = false;
    bool sync2 = false;
    // Phase 5: the dma-buf exchange. This process owns both images that cross the boundary -- the
    // proxy it reads and the answer it writes -- and exports each as a dma-buf whose fd number it
    // names in the shared header. The layer takes its own reference through pidfd_getfd.
    // The export fds stay open for as long as the images do; the layer's open is a fresh one.
    bool dmaBuf = false;
    uint32_t linuxPid = 0;
    GpuImage proxyIn{};
    int proxyExportFd = -1;
    uint32_t proxyGen = 0, proxyW = 0, proxyH = 0, proxySeq = 0;
    GpuImage answerOut{};
    int answerExportFd = -1;
    uint32_t answerGen = 0, answerSeq = 0;
    VkQueryPool flowQuery = nullptr;
    VkBuffer queryStaging = nullptr;
    VkDeviceMemory queryMem = nullptr;
    void* queryMap = nullptr;
    bool flowQueryAvailable = false;
    bool fenceTimedOut = false;  // the GPU stopped retiring our work; stand down
    bool memoryBudget = false;   // VK_EXT_memory_budget is available
    // VK_KHR_push_descriptor: lets the MVec pass hand its images to the command buffer at
    // record time instead of owning a pool, a set and the question of whether that set is
    // still in flight. It stops being a nicety the moment the binding COUNT varies with what
    // the optical-flow session granted, because a pool sized for one rung has to be torn
    // down and rebuilt for another.
    bool pushDescriptors = false;
    uint32_t driverMajor = 0, driverMinor = 0;
    // 1x1 stand-ins for the two optional bindings, so every declared binding always has a
    // valid descriptor whatever rung the session took.
    GpuImage dummyCost{};
    GpuImage dummyBack{};
    VkResult lastQueueResult = VK_SUCCESS;  // why the last submit or fence wait failed
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    bool pinMissed = false;      // DLSSNR_GPU_UUID/INDEX named a device that is not here
    // The model's own cost, on the graphics queue. Deliberately a separate pool from the
    // flow one: they are written from different queue families, and the whole GPU-timing
    // apparatus used to live inside the optical-flow capability check -- so a device
    // without VK_NV_optical_flow, or a user who turned estimated vectors off, lost the
    // ability to time the model, which is the one thing here worth timing.
    VkQueryPool evalQuery = nullptr;
    VkBuffer evalStaging = nullptr;
    VkDeviceMemory evalMem = nullptr;
    void* evalMap = nullptr;
    bool evalQueryAvailable = false;
    float timestampPeriod = 1.0f;
    uint32_t flowTimestampBits = 0;
    uint32_t gfxTimestampBits = 0;
    // Persistent MVec post pass (pipeline itself is per-size). The deadzone shader comes in two
    // compile-time variants -- one per flow format -- because a spec-constant branch on this driver
    // mispredicted and wrote NaN into MVec; the pipeline picks the module that matches the session.
    bool mvComputeSupported = false;
    VkShaderModule mvShaderFixed5 = nullptr;
    VkShaderModule mvShaderFloat = nullptr;
    VkDescriptorSetLayout mvDescLayout = nullptr;
    VkPipelineLayout mvPipeLayout = nullptr;
    // Transport: the shared-memory pixel regions imported as buffers via
    // VK_EXT_external_memory_host. When the driver accepts the import, the proxy upload and the
    // answer readback are GPU copies into and out of the mapping itself -- no staging, no memcpy.
    VkBuffer transportIn = VK_NULL_HANDLE;
    VkBuffer transportOut = VK_NULL_HANDLE;
    VkDeviceMemory transportInMem = VK_NULL_HANDLE;
    VkDeviceMemory transportOutMem = VK_NULL_HANDLE;
    void* transportInPtr = nullptr;
    void* transportOutPtr = nullptr;
    size_t transportBytes = 0;
};

static uint32_t FindMemoryType(VkCtx& c, uint32_t bits, VkMemoryPropertyFlags want);
static void DestroyImage2D(VkCtx& c, GpuImage& img);
static uint32_t FindHostMemoryType(VkCtx& c, uint32_t bits, bool preferCached);
static bool CreateStaging(VkCtx& c, size_t bytes);
static float HalfToFloat(uint16_t h);

static bool HasDeviceExt(VkPhysicalDevice phys, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, props.data());
    for (auto& p : props) if (!std::strcmp(p.extensionName, name)) return true;
    return false;
}

static bool CreateContext(VkCtx& c) {
    g_vkModule = LoadLibraryA("vulkan-1.dll");
    if (!g_vkModule) { Log("[helper] no vulkan-1.dll"); return false; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_vkModule, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(nullptr, "vkCreateInstance");

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlssnr_helper";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExts[] = { "VK_KHR_get_physical_device_properties2", "VK_EXT_debug_utils" };
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExts;
    if (vkCreateInstance(&ici, nullptr, &c.instance) != VK_SUCCESS) {
        // debug_utils unavailable: retry without it
        ici.enabledExtensionCount = 1;
        if (vkCreateInstance(&ici, nullptr, &c.instance) != VK_SUCCESS) { Log("[helper] vkCreateInstance failed"); return false; }
    }

    // The instance has asked for VK_EXT_debug_utils since it was written and never created a
    // messenger, so anything the validation layers had to say went nowhere. That made a
    // validation run unable to answer the question it is for -- whether the layouts we
    // record still match what the images are in after the NGX snippet has recorded its own
    // work into our command buffer. Zero messages is only evidence if messages had a path.
    //
    // Off unless asked for: the callback itself is free, but running with validation enabled
    // is not, and this is a diagnostic rather than a mode.
    if (getenv("DLSSNR_VK_DEBUG")) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)g_gipa(c.instance,
                                                                 "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                     void*) -> VkBool32 {
                Log("[vk-%s] %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error"
                                                                                      : "warn",
                    data && data->pMessage ? data->pMessage : "(no message)");
                return VK_FALSE;
            };
            if (create(c.instance, &dci, nullptr, &c.debugMessenger) == VK_SUCCESS)
                Log("[helper] vulkan debug messenger installed");
            else
                Log("[helper] vulkan debug messenger could not be created");
        } else {
            Log("[helper] DLSSNR_VK_DEBUG set but VK_EXT_debug_utils is unavailable");
        }
    }

#define LOAD(name) name = (PFN_##name)g_gipa(c.instance, #name);
    LOAD(vkDestroyInstance) LOAD(vkEnumeratePhysicalDevices) LOAD(vkGetPhysicalDeviceProperties)
    LOAD(vkGetPhysicalDeviceFormatProperties)
    LOAD(vkEnumerateDeviceExtensionProperties) LOAD(vkGetPhysicalDeviceQueueFamilyProperties)
    LOAD(vkCreateDevice) LOAD(vkDestroyDevice) LOAD(vkGetDeviceQueue) LOAD(vkCreateCommandPool)
    LOAD(vkDestroyCommandPool) LOAD(vkAllocateCommandBuffers) LOAD(vkBeginCommandBuffer)
    LOAD(vkEndCommandBuffer) LOAD(vkResetCommandBuffer) LOAD(vkQueueSubmit) LOAD(vkCreateFence) LOAD(vkDestroyFence)
    LOAD(vkWaitForFences) LOAD(vkResetFences) LOAD(vkCreateImage) LOAD(vkDestroyImage)
    LOAD(vkCreateImageView) LOAD(vkDestroyImageView) LOAD(vkGetImageMemoryRequirements)
    LOAD(vkAllocateMemory) LOAD(vkFreeMemory) LOAD(vkMapMemory) LOAD(vkUnmapMemory)
    LOAD(vkBindImageMemory) LOAD(vkGetImageSubresourceLayout) LOAD(vkCreateBuffer) LOAD(vkDestroyBuffer)
    LOAD(vkGetBufferMemoryRequirements) LOAD(vkBindBufferMemory) LOAD(vkCmdCopyBufferToImage)
    LOAD(vkCmdCopyImageToBuffer) LOAD(vkCmdCopyImage) LOAD(vkCmdPipelineBarrier) LOAD(vkDeviceWaitIdle)
    LOAD(vkGetMemoryHostPointerPropertiesEXT) LOAD(vkGetMemoryFdKHR) LOAD(vkGetMemoryFdPropertiesKHR)
    LOAD(vkGetPhysicalDeviceProperties2) LOAD(vkGetPhysicalDeviceMemoryProperties2)
    LOAD(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
    LOAD(vkCreateOpticalFlowSessionNV) LOAD(vkDestroyOpticalFlowSessionNV)
    LOAD(vkBindOpticalFlowSessionImageNV) LOAD(vkCmdOpticalFlowExecuteNV) LOAD(vkCmdBlitImage)
    LOAD(vkCmdPipelineBarrier2) LOAD(vkQueueSubmit2) LOAD(vkCmdWriteTimestamp2)
    LOAD(vkCreateQueryPool) LOAD(vkDestroyQueryPool) LOAD(vkCmdResetQueryPool)
    LOAD(vkCmdWriteTimestamp) LOAD(vkCmdCopyQueryPoolResults)
    LOAD(vkCreateSemaphore) LOAD(vkDestroySemaphore) LOAD(vkCmdClearColorImage)
    LOAD(vkCreateShaderModule) LOAD(vkDestroyShaderModule)
    LOAD(vkCreatePipelineLayout) LOAD(vkDestroyPipelineLayout)
    LOAD(vkCreateComputePipelines) LOAD(vkDestroyPipeline)
    LOAD(vkCmdBindPipeline) LOAD(vkCmdDispatch) LOAD(vkCmdBindDescriptorSets)
    LOAD(vkCreateDescriptorSetLayout) LOAD(vkDestroyDescriptorSetLayout)
    LOAD(vkCreateDescriptorPool) LOAD(vkDestroyDescriptorPool)
    LOAD(vkAllocateDescriptorSets) LOAD(vkUpdateDescriptorSets)
    LOAD(vkCmdPushDescriptorSetKHR)
    LOAD(vkCmdSetCheckpointNV) LOAD(vkGetQueueCheckpointDataNV)
#undef LOAD

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(c.instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> phys(devCount);
    vkEnumeratePhysicalDevices(c.instance, &devCount, phys.data());

    // Picking the FIRST qualifying device is right on the machine this was written for and
    // is not a choice. On a host with two NVIDIA cards we take card 0 with no way to say
    // otherwise, and CUDA_VISIBLE_DEVICES does not reach Vulkan any more than it reaches
    // DXGI. So: an explicit UUID wins, then an ordinal among the qualifying devices, then
    // the first as before.
    //
    // UUID rather than name, because two identical cards share a name and cannot be told
    // apart by one. Vulkan hands it to us directly through VkPhysicalDeviceIDProperties,
    // so there is no NVML or CUDA dependency to acquire for it.
    const char* wantUuid = getenv("DLSSNR_GPU_UUID");
    const char* wantIndexEnv = getenv("DLSSNR_GPU_INDEX");
    const long wantIndex = wantIndexEnv && *wantIndexEnv ? strtol(wantIndexEnv, nullptr, 10) : -1;

    const auto deviceUuid = [](VkPhysicalDevice p, char out[33]) {
        out[0] = '\0';
        if (!vkGetPhysicalDeviceProperties2) return false;
        VkPhysicalDeviceIDProperties idp{};
        idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(p, &p2);
        for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
            snprintf(out + i * 2, 3, "%02x", idp.deviceUUID[i]);
        return true;
    };

    long qualifying = 0;
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID != 0x10DE) continue;
        if (!HasDeviceExt(p, "VK_NVX_binary_import") || !HasDeviceExt(p, "VK_NVX_image_view_handle")) continue;

        char uuid[33];
        const bool haveUuid = deviceUuid(p, uuid);
        // Logged for every candidate, not only the winner: a user who needs to pin one has
        // to be able to find out what to pin it to, and this is the only place that knows.
        Log("[helper] candidate %ld: %s uuid=%s", qualifying, props.deviceName,
            haveUuid ? uuid : "(unavailable)");

        bool take;
        if (wantUuid && *wantUuid) take = haveUuid && _stricmp(uuid, wantUuid) == 0;
        else if (wantIndex >= 0) take = qualifying == wantIndex;
        else take = true;

        ++qualifying;
        if (!take || c.physical) continue;
        c.physical = p;
        // NVIDIA packs its own version into driverVersion; the Vulkan-wide major/minor/patch
        // split is not it. The driver version is the single most requested fact in every
        // report in this niche and this tree read it nowhere at all.
        c.driverMajor = props.driverVersion >> 22;
        c.driverMinor = (props.driverVersion >> 14) & 0xffu;
        Log("[helper] device: %s  driver %u.%02u", props.deviceName, c.driverMajor,
            c.driverMinor);
        if (!wantUuid && wantIndex < 0) break;  // first-wins: nothing after this can change it
    }
    if (!c.physical && (wantUuid || wantIndex >= 0)) {
        Log("[helper] DLSSNR_GPU_%s did not match any qualifying device; not falling back",
            wantUuid && *wantUuid ? "UUID" : "INDEX");
        // Refusing rather than falling back is the point: someone who pinned a GPU and
        // silently got a different one is worse off than someone who gets told.
        c.pinMissed = true;
        return false;
    }
    if (!c.physical) { Log("[helper] no NVIDIA device with NVX exts"); return false; }
    c.opticalFlow = HasDeviceExt(c.physical, VK_NV_OPTICAL_FLOW_EXTENSION_NAME);

    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &famCount, fams.data());
    bool haveQueue = false;
    for (uint32_t i = 0; i < famCount; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            c.queueFamily = i; haveQueue = true;
            if (fams[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) break;
        }
    }
    if (!haveQueue) { Log("[helper] no graphics+compute queue"); return false; }
    for (uint32_t i = 0; i < famCount; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) {
            c.opticalQueueFamily = i;
            break;
        }
    }
    if (c.opticalQueueFamily == UINT32_MAX) {
        Log("[helper] no optical-flow queue family");
        c.opticalFlow = false;
    }
    Log("[helper] queue family=%u flags=%#x optical=%u",
        c.queueFamily, fams[c.queueFamily].queueFlags, c.opticalQueueFamily);

    float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    qcis.push_back(qci);
    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX && c.opticalQueueFamily != c.queueFamily) {
        VkDeviceQueueCreateInfo qciFlow{};
        qciFlow.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qciFlow.queueFamilyIndex = c.opticalQueueFamily;
        qciFlow.queueCount = 1;
        qciFlow.pQueuePriorities = &prio;
        qcis.push_back(qciFlow);
    }
    std::vector<const char*> enabled;
    for (const char* e : { "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                           "VK_KHR_maintenance1", "VK_KHR_maintenance2", "VK_KHR_maintenance3",
                           "VK_KHR_maintenance4", "VK_KHR_buffer_device_address", "VK_KHR_push_descriptor",
                           "VK_KHR_synchronization2", "VK_EXT_memory_budget",
                           VK_NV_OPTICAL_FLOW_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                           VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME })
        if (HasDeviceExt(c.physical, e)) enabled.push_back(e);
    // Asked for only when someone asked for it: every other extension above buys a capability,
    // this one buys evidence and costs a command per marker to do so.
    if (CheckpointsEnabled() &&
        HasDeviceExt(c.physical, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
        enabled.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    c.sync2 = HasDeviceExt(c.physical, "VK_KHR_synchronization2");
    c.memoryBudget = HasDeviceExt(c.physical, "VK_EXT_memory_budget");
    c.pushDescriptors = HasDeviceExt(c.physical, "VK_KHR_push_descriptor");
    VkPhysicalDeviceOpticalFlowFeaturesNV flowFeatures{};
    flowFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    flowFeatures.opticalFlow = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features sync2Features{};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Features.synchronization2 = VK_TRUE;
    if (c.opticalFlow && c.sync2) flowFeatures.pNext = &sync2Features;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    if (c.opticalFlow) dci.pNext = &flowFeatures;
    else if (c.sync2) dci.pNext = &sync2Features;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)enabled.size();
    dci.ppEnabledExtensionNames = enabled.data();
    if (vkCreateDevice(c.physical, &dci, nullptr, &c.device) != VK_SUCCESS) { Log("[helper] vkCreateDevice failed"); return false; }
    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);
    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX)
        vkGetDeviceQueue(c.device, c.opticalQueueFamily, 0, &c.opticalQueue);

    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(c.physical, &props);
        c.timestampPeriod = props.limits.timestampPeriod > 0.0f ? props.limits.timestampPeriod : 1.0f;
        const uint32_t queryFamily = (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX)
            ? c.opticalQueueFamily : c.queueFamily;
        if (queryFamily < famCount) c.flowTimestampBits = fams[queryFamily].timestampValidBits;
        if (c.queueFamily < famCount) c.gfxTimestampBits = fams[c.queueFamily].timestampValidBits;
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = c.queueFamily;
    if (vkCreateCommandPool(c.device, &cpci, nullptr, &c.cmdPool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = c.cmdPool;
    cbai.commandBufferCount = 4;
    VkCommandBuffer bufs[4];
    if (vkAllocateCommandBuffers(c.device, &cbai, bufs) != VK_SUCCESS) return false;
    c.cmdScratch = bufs[0]; c.cmdCreate = bufs[1]; c.cmdEval = bufs[2]; c.cmdFlowPost = bufs[3];

    if (c.opticalFlow && c.opticalQueueFamily != UINT32_MAX) {
        VkCommandPoolCreateInfo fpci{};
        fpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        fpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        fpci.queueFamilyIndex = c.opticalQueueFamily;
        if (vkCreateCommandPool(c.device, &fpci, nullptr, &c.cmdPoolFlow) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo fcbai{};
        fcbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        fcbai.commandPool = c.cmdPoolFlow;
        fcbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(c.device, &fcbai, &c.cmdFlow) != VK_SUCCESS) return false;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (uint32_t i = 0; i < VkCtx::kFenceRing; ++i) {
        if (vkCreateFence(c.device, &fci, nullptr, &c.fences[i]) != VK_SUCCESS) return false;
    }
    if (c.opticalFlow && c.opticalQueue && vkCreateSemaphore) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(c.device, &sci, nullptr, &c.semPrep) != VK_SUCCESS ||
            vkCreateSemaphore(c.device, &sci, nullptr, &c.semFlow) != VK_SUCCESS) {
            Log("[helper] semaphore creation failed, NVOF stays synchronous");
            if (c.semPrep) { vkDestroySemaphore(c.device, c.semPrep, nullptr); c.semPrep = nullptr; }
            if (c.semFlow) { vkDestroySemaphore(c.device, c.semFlow, nullptr); c.semFlow = nullptr; }
        }
    }

    // GPU MVec deadzone pass needs storage access on both flow and MVec formats.
    if (vkGetPhysicalDeviceFormatProperties && vkCreateShaderModule && vkCreateComputePipelines &&
        vkCmdDispatch && vkCreateDescriptorSetLayout && vkCreateDescriptorPool &&
        vkAllocateDescriptorSets && vkUpdateDescriptorSets && kMVecDeadzoneSpvFixed5Len) {
        VkFormatProperties u16{};
        VkFormatProperties f16{};
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_UINT, &u16);
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_SFLOAT, &f16);
        c.mvComputeSupported =
            (u16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            (f16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    }
    Log("[helper] mvec gpu compute supported=%d", int(c.mvComputeSupported));

    // One builder, two pools. What decides whether timing exists is the queue family's
    // timestampValidBits and the entry points, and nothing else: which pass gets timed is
    // a separate question from whether timing is possible at all.
    const auto makeTimestampPool = [&](VkQueryPool* pool, VkBuffer* staging, VkDeviceMemory* mem,
                                       void** map) -> bool {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        if (vkCreateQueryPool(c.device, &qpi, nullptr, pool) != VK_SUCCESS) return false;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 16;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(c.device, &bci, nullptr, staging) != VK_SUCCESS) {
            vkDestroyQueryPool(c.device, *pool, nullptr); *pool = nullptr;
            return false;
        }
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(c.device, *staging, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
        if (mai.memoryTypeIndex != UINT32_MAX &&
            vkAllocateMemory(c.device, &mai, nullptr, mem) == VK_SUCCESS &&
            vkBindBufferMemory(c.device, *staging, *mem, 0) == VK_SUCCESS &&
            vkMapMemory(c.device, *mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS) {
            return true;
        }
        if (*mem) vkFreeMemory(c.device, *mem, nullptr);
        vkDestroyBuffer(c.device, *staging, nullptr);
        vkDestroyQueryPool(c.device, *pool, nullptr);
        *mem = nullptr; *staging = nullptr; *pool = nullptr; *map = nullptr;
        return false;
    };

    const bool timestampCalls = vkCreateQueryPool && vkCmdResetQueryPool &&
                                vkCmdCopyQueryPoolResults &&
                                (vkCmdWriteTimestamp2 || vkCmdWriteTimestamp);
    if (timestampCalls && c.opticalFlow && c.flowTimestampBits)
        c.flowQueryAvailable = makeTimestampPool(&c.flowQuery, &c.queryStaging, &c.queryMem, &c.queryMap);
    if (timestampCalls && c.gfxTimestampBits)
        c.evalQueryAvailable = makeTimestampPool(&c.evalQuery, &c.evalStaging, &c.evalMem, &c.evalMap);

    Log("[helper] timestamp queries flow=%d eval=%d flow_bits=%u gfx_bits=%u period=%.3f",
        int(c.flowQueryAvailable), int(c.evalQueryAvailable), c.flowTimestampBits,
        c.gfxTimestampBits, c.timestampPeriod);
    if (!c.evalQueryAvailable)
        Log("[helper] no GPU timing for the model; helperEvalMs will be CPU wall clock");

    // Staging is allocated on the first frame, at that frame's size, rather than at the largest frame
    // the protocol can carry. Every path that needs it grows it on demand already. Reserving the
    // maximum up front cost two host-visible buffers of kMaxFrame each -- which, once the protocol
    // grew to cover a supersampled 4K model raster, is a quarter of a gigabyte of pinned memory for a
    // game that may present at 1080p.
    return true;
}

static uint32_t FindMemoryType(VkCtx& c, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(c.instance, "vkGetPhysicalDeviceMemoryProperties");
    getMP(c.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static uint32_t FindHostMemoryType(VkCtx& c, uint32_t bits, bool preferCached) {
    VkPhysicalDeviceMemoryProperties mp{};
    PFN_vkGetPhysicalDeviceMemoryProperties getMP =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(c.instance, "vkGetPhysicalDeviceMemoryProperties");
    getMP(c.physical, &mp);
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        int score = 0;
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) score += preferCached ? 100 : 20;
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) score += 10;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) score -= 50;
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return best >= 0 ? (uint32_t)best : UINT32_MAX;
}

static bool CreateStaging(VkCtx& c, size_t bytes) {
    if (bytes <= c.stagingSize && c.uploadMap && c.readMap) return true;
    if (c.device) vkDeviceWaitIdle(c.device);
    auto destroy = [&]() {
        if (c.uploadStaging) vkDestroyBuffer(c.device, c.uploadStaging, nullptr);
        if (c.readStaging) vkDestroyBuffer(c.device, c.readStaging, nullptr);
        if (c.uploadMem) vkFreeMemory(c.device, c.uploadMem, nullptr);
        if (c.readMem) vkFreeMemory(c.device, c.readMem, nullptr);
        c.uploadStaging = c.readStaging = nullptr;
        c.uploadMem = c.readMem = nullptr;
        c.uploadMap = c.readMap = nullptr;
        c.stagingSize = 0;
    };
    destroy();

    auto make = [&](VkBuffer& buf, VkDeviceMemory& mem, void** map) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(c.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(c.device, buf, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
        if (mai.memoryTypeIndex == UINT32_MAX) {
            destroy();
            return false;
        }
        if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS ||
            vkBindBufferMemory(c.device, buf, mem, 0) != VK_SUCCESS ||
            vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
            destroy();
            return false;
        }
        return true;
    };

    if (!make(c.uploadStaging, c.uploadMem, &c.uploadMap) ||
        !make(c.readStaging, c.readMem, &c.readMap)) {
        destroy();
        return false;
    }
    c.stagingSize = bytes;
    return true;
}

// Import one shared-memory region as a buffer (VK_EXT_external_memory_host). The driver names the
// memory type that may back the pointer, so the type is taken from that intersection rather than
// scored on property flags like ordinary host-visible memory.
static bool ImportTransportOne(VkCtx& c, void* ptr, size_t bytes, VkBufferUsageFlags usage,
                               VkBuffer& buf, VkDeviceMemory& mem) {
    VkMemoryHostPointerPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (vkGetMemoryHostPointerPropertiesEXT(c.device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &props) != VK_SUCCESS)
        return false;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExternalMemoryBufferCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    bci.pNext = &ext;
    if (vkCreateBuffer(c.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, buf, &req);
    const uint32_t typeBits = req.memoryTypeBits & props.memoryTypeBits;
    if (!typeBits) { vkDestroyBuffer(c.device, buf, nullptr); buf = VK_NULL_HANDLE; return false; }
    uint32_t type = 0;
    while (!(typeBits & (1u << type))) ++type;
    VkImportMemoryHostPointerInfoEXT hpi{};
    hpi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    hpi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    hpi.pHostPointer = ptr;
    // The allocation size must be a multiple of the driver's import alignment (64 KiB on NVIDIA);
    // kMaxFrame already is, but the rounding keeps this correct for any region size.
    VkDeviceSize align = 65536;
    if (vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
        hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(c.physical, &p2);
        if (hostProps.minImportedHostPointerAlignment) align = hostProps.minImportedHostPointerAlignment;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &hpi;
    mai.allocationSize = (req.size + align - 1) & ~(align - 1);
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS ||
        vkBindBufferMemory(c.device, buf, mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(c.device, buf, nullptr);
        buf = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// Make the two pixel regions the GPU's own buffers. When this succeeds the proxy upload and the
// answer readback are device-side copies into and out of the file's pages -- no staging buffer and
// no host memcpy in the frame. When it fails (no extension, misaligned view, driver refuses) the
// staging paths below carry on exactly as before.
static bool ImportTransport(VkCtx& c, void* in, void* out, size_t bytes) {
    if (c.transportIn && c.transportInPtr == in && c.transportOutPtr == out && c.transportBytes == bytes)
        return true;
    if (c.transportIn) vkDestroyBuffer(c.device, c.transportIn, nullptr);
    if (c.transportOut) vkDestroyBuffer(c.device, c.transportOut, nullptr);
    if (c.transportInMem) vkFreeMemory(c.device, c.transportInMem, nullptr);
    if (c.transportOutMem) vkFreeMemory(c.device, c.transportOutMem, nullptr);
    c.transportIn = c.transportOut = VK_NULL_HANDLE;
    c.transportInMem = c.transportOutMem = VK_NULL_HANDLE;
    c.transportInPtr = c.transportOutPtr = nullptr;
    c.transportBytes = 0;
    if (!vkGetMemoryHostPointerPropertiesEXT || !in || !out || !bytes) return false;

    VkDeviceSize align = 0;
    if (vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
        hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &hostProps;
        vkGetPhysicalDeviceProperties2(c.physical, &props2);
        align = hostProps.minImportedHostPointerAlignment;
    }
    if (!align) align = 1;
    if ((reinterpret_cast<uintptr_t>(in) | reinterpret_cast<uintptr_t>(out)) % align) {
        Log("[helper] view is not %llu-byte aligned; keeping staging transport",
            (unsigned long long)align);
        return false;
    }
    if (!ImportTransportOne(c, in, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            c.transportIn, c.transportInMem))
        return false;
    if (!ImportTransportOne(c, out, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            c.transportOut, c.transportOutMem)) {
        vkDestroyBuffer(c.device, c.transportIn, nullptr);
        vkFreeMemory(c.device, c.transportInMem, nullptr);
        c.transportIn = VK_NULL_HANDLE;
        c.transportInMem = VK_NULL_HANDLE;
        return false;
    }
    c.transportInPtr = in;
    c.transportOutPtr = out;
    c.transportBytes = bytes;
    Log("[helper] transport imported: the shared-memory regions are the GPU's buffers");
    return true;
}

// ---------------------------------------------------------------------------
// Phase 5: dma-buf transport
// ---------------------------------------------------------------------------
// The proxy arrives as a file descriptor over the socket and becomes an image of this device over
// the proxy it reads and the answer it writes. Both are optional -- without the channel, the
// extension, or a successful export, the shared-memory transport above carries the frame. The
// images are CONCURRENT and cross the boundary through FOREIGN_EXT: the layer acquires the proxy
// from FOREIGN before its first write and releases it back before this process reads it, and the
// answer is released to FOREIGN before the answer number moves.

// Build an RGBA8 image in memory that can leave this process as a dma-buf, and hand out the fd.
// The fd stays open here: the layer opens its own reference through /proc, and the number must
// stay meaningful until the image is rebuilt.
static bool CreateExportable(VkCtx& c, GpuImage& img, int& exportFd, uint32_t w, uint32_t h,
                             VkFormat fmt, const char* what) {
    if (img.image && img.width == w && img.height == h && img.format == fmt && exportFd >= 0)
        return true;
    DestroyImage2D(c, img);
    if (exportFd >= 0) close(exportFd);
    exportFd = -1;
    if (!vkGetMemoryFdKHR || !w || !h) return false;

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &img.image) != VK_SUCCESS) return false;
    img.format = fmt;
    img.width = w;
    img.height = h;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, img.image, &req);
    uint32_t type = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { DestroyImage2D(c, img); return false; }
    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(c.device, &mai, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(c.device, img.image, img.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &img.view) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        return false;
    }
    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = img.memory;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (vkGetMemoryFdKHR(c.device, &gfi, &exportFd) != VK_SUCCESS) {
        DestroyImage2D(c, img);
        exportFd = -1;
        return false;
    }
    Log("[fd] %s exportable %ux%u fd=%d", what, w, h, exportFd);
    return true;
}

// Keep the proxy image at the current raster and publish its descriptor. The layer writes this
// image; this process reads it. Returns true when the image exists at this size.
static bool EnsureProxyOut(VkCtx& c, ShmHeader* hdr, uint32_t w, uint32_t h, VkFormat fmt) {
    const bool sizeChanged = c.proxyIn.image &&
                             (c.proxyIn.width != w || c.proxyIn.height != h || c.proxyIn.format != fmt);
    if (!CreateExportable(c, c.proxyIn, c.proxyExportFd, w, h, fmt, "proxy")) {
        c.proxyW = c.proxyH = 0;
        return false;
    }
    c.proxyW = w;
    c.proxyH = h;
    if (sizeChanged || c.proxySeq == 0) {
        ++c.proxyGen;
        ++c.proxySeq;
    }
    // Restate the descriptor whenever the header does not carry this sequence -- first frame,
    // rebuild, or a header re-initialised by another process under us.
    if (hdr->proxyExportSeq.load() != c.proxySeq) {
        hdr->proxyPid.store(c.linuxPid);
        hdr->proxyFd.store(uint32_t(c.proxyExportFd));
        hdr->proxyGen.store(c.proxyGen);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->proxyExportSeq.store(c.proxySeq);
    }
    return true;
}

static bool EnsureAnswerOut(VkCtx& c, ShmHeader* hdr, uint32_t w, uint32_t h, VkFormat fmt) {
    const bool sizeChanged = c.answerOut.image &&
                             (c.answerOut.width != w || c.answerOut.height != h || c.answerOut.format != fmt);
    if (!CreateExportable(c, c.answerOut, c.answerExportFd, w, h, fmt, "answer")) {
        if (c.answerOut.image == VK_NULL_HANDLE && hdr->answerExportSeq.load()) {
            ++c.answerGen;
            hdr->answerExportSeq.store(0);  // withdrawn: the layer must not open a stale number
        }
        return false;
    }
    if (sizeChanged || c.answerSeq == 0) {
        ++c.answerGen;
        ++c.answerSeq;
    }
    if (hdr->answerExportSeq.load() != c.answerSeq) {
        hdr->answerPid.store(c.linuxPid);
        hdr->answerFd.store(uint32_t(c.answerExportFd));
        hdr->answerGen.store(c.answerGen);
        std::atomic_thread_fence(std::memory_order_release);
        hdr->answerExportSeq.store(c.answerSeq);
    }
    return true;
}

// The buffer a colorIn upload should read from: the imported mapping when there is one, the
// staging copy otherwise. Callers that upload something other than the proxy pass uploadStaging
// explicitly.
static VkBuffer UploadSource(VkCtx& c) {
    return c.transportIn ? c.transportIn : c.uploadStaging;
}

static bool CreateImage2DUsage(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h,
                               VkImageUsageFlags usage, GpuImage& out) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    const uint32_t queueFamilies[] = { c.queueFamily, c.opticalQueueFamily };
    const bool crossQueue = c.opticalFlow && c.opticalQueueFamily != UINT32_MAX &&
                            c.opticalQueueFamily != c.queueFamily;
    ci.sharingMode = crossQueue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = crossQueue ? 2u : 0u;
    ci.pQueueFamilyIndices = crossQueue ? queueFamilies : nullptr;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { DestroyImage2D(c, out); return false; }
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &out.view) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    return true;
}

static bool CreateImage2D(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h, GpuImage& out) {
    return CreateImage2DUsage(c, fmt, w, h,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, out);
}

static bool CreateImage2DOpticalFlow(VkCtx& c, VkFormat fmt, uint32_t w, uint32_t h,
                                     VkOpticalFlowUsageFlagsNV ofUsage, GpuImage& out) {
    VkOpticalFlowImageFormatInfoNV ofInfo{};
    ofInfo.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
    ofInfo.usage = ofUsage;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ofInfo;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // The flow OUTPUT is viewed through a second format -- R16G16_UINT over
    // R16G16_SFIXED5_NV or R16G16_SFLOAT -- and a view whose format differs from its
    // image's is only legal when the image was created MUTABLE_FORMAT. It was not, so the
    // one view the entire deadzone pass reads through was an invalid view that NVIDIA's
    // driver happened to honour. Only the output needs it; the two input images are never
    // aliased and mutable format can cost compression, so they do not get it.
    if (ofUsage & VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV)
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    // STORAGE: the MVec deadzone compute pass reads the raw flow texels through
    // a size-compatible R16G16_UINT view (no image->image copy out of the
    // optical-flow output, which the driver cannot sample/copy as bits).
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const uint32_t queueFamilies[] = { c.queueFamily, c.opticalQueueFamily };
    const bool crossQueue = c.opticalFlow && c.opticalQueueFamily != UINT32_MAX &&
                            c.opticalQueueFamily != c.queueFamily;
    ci.sharingMode = crossQueue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = crossQueue ? 2u : 0u;
    ci.pQueueFamilyIndices = crossQueue ? queueFamilies : nullptr;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { DestroyImage2D(c, out); return false; }
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &out.view) != VK_SUCCESS) {
        DestroyImage2D(c, out);
        return false;
    }
    return true;
}

static void DestroyImage2D(VkCtx& c, GpuImage& img) {
    if (img.view) vkDestroyImageView(c.device, img.view, nullptr);
    if (img.image) vkDestroyImage(c.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(c.device, img.memory, nullptr);
    img = {};
}

static size_t ImageSizeBytes(VkCtx& c, GpuImage& img) {
    // vkGetImageSubresourceLayout is only defined for LINEAR (and DRM-modifier) tiling, and
    // every image this is called on is OPTIMAL. Asking anyway was a spec violation whose
    // answer had no defined meaning -- the driver's number was being preferred over our own
    // arithmetic precisely where it is least entitled to one. The per-format sizes below
    // were always the real answer.
    (void)c;
    switch (img.format) {
        case VK_FORMAT_R16G16_SFLOAT: return size_t(img.width) * img.height * 4;
        case VK_FORMAT_R16G16_SFIXED5_NV: return size_t(img.width) * img.height * 4;
        case VK_FORMAT_R32_SFLOAT: return size_t(img.width) * img.height * 4;
        default: return size_t(img.width) * img.height * 4;
    }
}

static void TransitionImage(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags ss, VkPipelineStageFlags ds);

static void TransitionImage2(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                             VkAccessFlags2 srcA, VkAccessFlags2 dstA,
                             VkPipelineStageFlags2 ss, VkPipelineStageFlags2 ds) {
    if (img.layout == dst) return;
    if (c.sync2 && vkCmdPipelineBarrier2) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = ss;
        b.srcAccessMask = srcA;
        b.dstStageMask = ds;
        b.dstAccessMask = dstA;
        b.oldLayout = img.layout;
        b.newLayout = dst;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { img.aspect(), 0, 1, 0, 1 };
        VkDependencyInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
        img.layout = dst;
        return;
    }
    TransitionImage(c, cb, img, dst,
                    VkAccessFlags(srcA & 0xFFFFFFFFu), VkAccessFlags(dstA & 0xFFFFFFFFu),
                    VkPipelineStageFlags(ss & 0xFFFFFFFFu), VkPipelineStageFlags(ds & 0xFFFFFFFFu));
}

static void TransitionImage(VkCtx& c, VkCommandBuffer cb, GpuImage& img, VkImageLayout dst,
                            VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    if (img.layout == dst) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = img.layout; b.newLayout = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = { img.aspect(), 0, 1, 0, 1 };
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = dst;
}

static bool BeginCmd(VkCommandBuffer cb) {
    if (vkResetCommandBuffer) vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS;
}

// Submits asynchronously onto the fence ring; returns the fence index (or -1).
// The caller waits only when it actually needs the result, so the CPU never
// blocks on intermediate stages (upload, NVOF prep, flow post).
static int SubmitAsync(VkCtx& c, VkCommandBuffer cb, VkQueue queue,
                       uint32_t waitCount, const VkSemaphore* waits,
                       VkPipelineStageFlags2 waitStage, VkSemaphore signal) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    if ((waitCount || signal) && (!c.sync2 || !vkQueueSubmit2)) return -1;
    if (waitCount || signal) {
        VkCommandBufferSubmitInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = cb;
        VkSemaphoreSubmitInfo wsi{};
        if (waitCount) {
            wsi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            wsi.semaphore = waits[0];
            wsi.stageMask = waitStage;
        }
        VkSemaphoreSubmitInfo ssi{};
        if (signal) {
            ssi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            ssi.semaphore = signal;
            ssi.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkSubmitInfo2 si2{};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si2.waitSemaphoreInfoCount = waitCount;
        si2.pWaitSemaphoreInfos = waitCount ? &wsi : nullptr;
        si2.commandBufferInfoCount = 1;
        si2.pCommandBufferInfos = &cbi;
        si2.signalSemaphoreInfoCount = signal ? 1u : 0u;
        si2.pSignalSemaphoreInfos = signal ? &ssi : nullptr;
        const uint32_t idx = c.fenceCursor;
        const VkResult sr = vkQueueSubmit2(queue, 1, &si2, c.fences[idx]);
        if (sr != VK_SUCCESS) {
            c.lastQueueResult = sr;
            Log("[vk] queue submit2 failed: %d", (int)sr);
            return -1;
        }
        c.fenceCursor = (c.fenceCursor + 1) % VkCtx::kFenceRing;
        return (int)idx;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    const uint32_t idx = c.fenceCursor;
    const VkResult sr = vkQueueSubmit(queue, 1, &si, c.fences[idx]);
    if (sr != VK_SUCCESS) {
        c.lastQueueResult = sr;
        Log("[vk] queue submit failed: %d", (int)sr);
        return -1;
    }
    c.fenceCursor = (c.fenceCursor + 1) % VkCtx::kFenceRing;
    return (int)idx;
}

// Generous rather than tight: a long pass chain at 4K on a slow part is genuinely slow,
// and cutting a working frame off is worse than the stall this bounds. A lost device
// already returns VK_ERROR_DEVICE_LOST from the wait rather than hanging; this covers the
// other case, a driver that stops without losing the device, where an infinite wait parks
// this process for the rest of the session with nothing in the log.
static constexpr uint64_t kFenceBudgetNs = 10ull * 1000ull * 1000ull * 1000ull;

// Device-local memory in use, in MiB, or 0 when the device cannot say.
//
// helperVramMB has been declared, saved and restored by ShmResetSettings since it was added
// and written by absolutely nothing -- so the VRAM budget the pass ceiling is supposed to
// respect has never had a measured input. VK_EXT_memory_budget is how a Vulkan process asks;
// heapUsage is this process's own use of each heap and heapBudget is what it may reasonably
// take, both from the driver rather than from arithmetic over our own allocations, which
// would miss everything the snippet allocates behind our back -- and the snippet's feature
// histories are the whole point of measuring.
static void DeviceLocalMemoryMB(VkCtx& c, uint32_t* usedMB, uint32_t* budgetMB) {
    if (usedMB) *usedMB = 0;
    if (budgetMB) *budgetMB = 0;
    if (!c.memoryBudget || !vkGetPhysicalDeviceMemoryProperties2) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{};
    mb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp{};
    mp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp.pNext = &mb;
    vkGetPhysicalDeviceMemoryProperties2(c.physical, &mp);
    VkDeviceSize used = 0, budget = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i) {
        if (!(mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        used += mb.heapUsage[i];
        budget += mb.heapBudget[i];
    }
    if (usedMB) *usedMB = uint32_t(used / (1024ull * 1024ull));
    if (budgetMB) *budgetMB = uint32_t(budget / (1024ull * 1024ull));
}

// After a lost device or a fence that never signalled, ask the queue what it had reached.
//
// The only call in Vulkan that answers "where was the GPU", and valid precisely in the state where
// nothing else is. Null unless the device enabled the extension, which happens only under
// DLSSNR_CHECKPOINTS=1.
static void DumpCheckpoints(VkCtx& c, VkQueue queue) {
    if (!vkGetQueueCheckpointDataNV || queue == VK_NULL_HANDLE) {
        if (CheckpointsEnabled())
            Log("[checkpoints] unavailable (the device did not enable "
                "VK_NV_device_diagnostic_checkpoints)");
        return;
    }
    uint32_t n = 0;
    vkGetQueueCheckpointDataNV(queue, &n, nullptr);
    if (!n) { Log("[checkpoints] the queue reported none"); return; }
    std::vector<VkCheckpointDataNV> data(n);
    for (auto& d : data) d.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    vkGetQueueCheckpointDataNV(queue, &n, data.data());
    Log("[checkpoints] %u marker(s) the GPU had reached:", n);
    for (uint32_t i = 0; i < n; ++i) {
        const char* marker = (const char*) data[i].pCheckpointMarker;
        Log("[checkpoints]   stage=0x%08x %s", (unsigned) data[i].stage, marker ? marker : "(null)");
    }
}

static bool WaitFence(VkCtx& c, int idx) {
    if (idx < 0) return false;
    const VkResult r = vkWaitForFences(c.device, 1, &c.fences[idx], VK_TRUE, kFenceBudgetNs);
    if (r != VK_SUCCESS) c.lastQueueResult = r;
    if (r == VK_ERROR_DEVICE_LOST) {
        static bool said = false;
        if (!said) {
            said = true;
            DumpCrumbs("device lost");
            DumpCheckpoints(c, c.queue);
        }
        return false;
    }
    if (r == VK_TIMEOUT) {
        // Not recoverable from here, and pretending otherwise is worse than stopping: the
        // command buffer this fence covers is one the next frame re-begins, and beginning
        // a buffer that is still executing is undefined. So the process stands down and
        // the layer sees a helper that is simply absent -- which is the case its whole
        // fail-open design is built for.
        if (!c.fenceTimedOut) {
            c.fenceTimedOut = true;
            Log("[helper] a fence did not signal within %llu ms; the GPU is not retiring "
                "our work. Stopping rather than reusing the command buffer it covers.",
                (unsigned long long)(kFenceBudgetNs / 1000000ull));
            DumpCrumbs("a fence did not signal");
            DumpCheckpoints(c, c.queue);
        }
        return false;
    }
    if (r != VK_SUCCESS) return false;
    vkResetFences(c.device, 1, &c.fences[idx]);
    return true;
}

static bool SubmitAndWaitQueue(VkCtx& c, VkCommandBuffer cb, VkQueue queue) {
    const int idx = SubmitAsync(c, cb, queue, 0, nullptr, 0, nullptr);
    return idx >= 0 && WaitFence(c, idx);
}

static bool SubmitAndWait(VkCtx& c, VkCommandBuffer cb) {
    return SubmitAndWaitQueue(c, cb, c.queue);
}

static bool UploadMappedPixels(VkCtx& c, GpuImage& img, size_t bytes,
                               VkBuffer srcOverride = VK_NULL_HANDLE) {
    const VkBuffer src = srcOverride ? srcOverride : c.uploadStaging;
    if (!src) return false;
    if (!srcOverride && (!c.uploadMap || bytes > c.stagingSize)) return false;
    if (!BeginCmd(c.cmdScratch)) return false;
    TransitionImage(c, c.cmdScratch, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyBufferToImage(c.cmdScratch, src, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return SubmitAndWait(c, c.cmdScratch);
}

static bool ReadbackPixels(VkCtx& c, GpuImage& img, size_t bytes) {
    if (c.transportOut) {
        // The answer lands in the shared-memory region itself; the layer's acquire fence on
        // seq_resp is the only ordering the bytes need beyond this submit.
        if (!BeginCmd(c.cmdEval)) return false;
        TransitionImage(c, c.cmdEval, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { img.aspect(), 0, 0, 1 };
        region.imageExtent = { img.width, img.height, 1 };
        vkCmdCopyImageToBuffer(c.cmdEval, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c.transportOut, 1, &region);
        return SubmitAndWait(c, c.cmdEval);
    }
    if (bytes > c.stagingSize && !CreateStaging(c, bytes)) return false;
    if (!c.readMap) return false;
    if (!BeginCmd(c.cmdEval)) return false;
    TransitionImage(c, c.cmdEval, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyImageToBuffer(c.cmdEval, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           c.readStaging, 1, &region);
    return SubmitAndWait(c, c.cmdEval);
}

static bool UploadPixels(VkCtx& c, GpuImage& img, const void* pixels, size_t bytes) {
    if (bytes > c.stagingSize && !CreateStaging(c, bytes)) return false;
    if (!c.uploadMap) return false;
    if (pixels != c.uploadMap) std::memcpy(c.uploadMap, pixels, bytes);
    return UploadMappedPixels(c, img, bytes);
}

// ---------------------------------------------------------------------------
// Per-size neural pipeline state
// ---------------------------------------------------------------------------
struct OpticalFlowState {
    bool enabled = false;
    VkOpticalFlowSessionNV session = nullptr;
    VkFormat inputFormat = VK_FORMAT_UNDEFINED;
    VkFormat flowFormat = VK_FORMAT_UNDEFINED;
    uint32_t grid = 1;
    uint32_t quality = kMVecBalanced;
    uint32_t attemptedQuality = 0;
    uint32_t attemptedPixelSize = kMVecPixels4;
    bool userDisabled = false;
    bool hasPrev = false;
    // The session keeps temporal hints of its own, and they are not ours to see or clear.
    // Set whenever the pair this engine is about to compare does not follow from the pair
    // it last saw, and consumed by the next execute.
    bool hintsStale = true;
    // What the device says it can do, read once at setup and logged. These have been
    // available in VkPhysicalDeviceOpticalFlowPropertiesNV since the extension shipped and
    // were queried and thrown away: the code read supportedOutputGridSizes out of the same
    // struct and ignored the four capability booleans beside it.
    bool costSupported = false;
    bool globalSupported = false;
    bool bidirSupported = false;
    VkImageView costBitsView = nullptr;  // r8ui view of the cost image
    VkImageView backBitsView = nullptr;  // R16G16_UINT view of the backward flow
    GpuImage back{};        // backward flow field, when the both-directions rung was taken
    bool backEnabled = false;
    uint32_t inputWidth = 0;    // the size the engine actually works at
    uint32_t inputHeight = 0;
    GpuImage cost{};        // per-block matching cost, at the flow grid, when enabled
    GpuImage globalFlow{};  // one vector for the whole frame, 1x1, when enabled
    VkFormat costFormat = VK_FORMAT_UNDEFINED;
    bool costEnabled = false;
    bool globalEnabled = false;
    bool currentToPrevious = true;
    bool flowTransferSrc = false;
    bool gpuConvertChecked = false;
    bool loggedFirstFlow = false;
    // Fully-GPU post pass: raw flow texels -> decode -> deadzone -> upscale -> MVec.
    bool gpuCompute = false;
    VkImageView outBitsView = nullptr;  // R16G16_UINT view of the NVOF output
    VkPipeline mvPipeline = nullptr;
    VkDescriptorPool mvPool = nullptr;
    VkDescriptorSet mvSet = nullptr;
    GpuImage prev{}, curr{}, out{};
};

static float ClampF(float v, float lo, float hi) {
    if (!(v >= lo)) return lo;
    if (v > hi) return hi;
    return v;
}

struct NeuralState {
    VkCtx vk{};
    NgxSnippet ngx{};

    // The proxy the layer sent (8-bit display-referred, or float16 normalised linear light when the
    // HDR path is on), and two surfaces the chain alternates between. Two, not one, because
    // a pass must read the previous pass's answer while writing its own: with a single surface the
    // model would be reading and writing the same image.
    GpuImage colorIn{}, workA{}, workB{}, mv{}, depth{};
    // DLSSNR.ControlMask, only under DLSSNR_PROBE_CONTROLMASK=1. A checkerboard, which is the
    // cheapest possible answer to "does binding this key move the picture at all" -- if half the
    // frame is masked and nothing changes, there is nothing to plumb. See the probe below.
    GpuImage probeMask{};
    // Where the chain lands before it leaves. The passes run at higher precision than the
    // transport, so the last one is brought down to the transport's format here, once.
    GpuImage colorOut{};
    // Phase 5: set every frame the imported proxy covers this raster, so the upload reads the
    // layer's exported memory instead of the shared-memory region.
    bool proxyActive = false;

    uint32_t w = 0, h = 0;
    bool ready = false;
    // The HDR proxy state this process has actually built: 1 when the crossing images, the chain
    // surfaces and the model's feature contract are all float16. It switches only between frames,
    // and the frame that switches is failed on purpose -- its bytes are still the old width.
    uint32_t hdrBuilt = 0;
    bool sdr16Built = true;
    // The model refused the float contract. Stay 8-bit until HDR is switched off and back on.
    bool hdrRejected = false;

    // Per-pass state. A pass owns a feature, the tuning that feature was built with, and whether it
    // still owes the model a history reset.
    NgxTuning tuning[kMaxPasses] = {};
    bool passNeedsReset[kMaxPasses] = {};
    uint32_t livePasses = 0;

    // A pass is dirty when the header's tuning for it no longer matches what its feature was built
    // with. It keeps answering with the old tuning until the replacement is ready, so a retune is a
    // swap inside one frame rather than a gap in the chain.
    bool passDirty[kMaxPasses] = {};
    NgxTuning lastSeenTuning[kMaxPasses] = {};
    // What each pass was last EVALUATED with, which since the six became live is a different
    // question from what it was BUILT with. A change here owes the model a history reset: its
    // accumulated state was produced under the old strengths, and carrying it across is what makes
    // a slider drag smear instead of simply landing.
    NgxTuning lastEvalTuning[kMaxPasses] = {};
    bool haveEvalTuning[kMaxPasses] = {};

    // Rebuilds are spaced rather than done at once: back-to-back NGX creation exhausts the driver's
    // latches and the model stops answering until the process restarts. The spacing is wall-clock
    // milliseconds from the header (0 = no spacing) rather than frames, because a frame-counted wait
    // crawls on a 30 fps game and races on a 144 fps one.
    double buildAfterMs = 0;
    double tuningChangedMs = 0;

    uint64_t evaluates = 0;

    // Motion vectors, from bmitch87's work. The layer hands over a finished swapchain image and
    // nothing else, so the field is estimated here with the optical-flow engine rather than read
    // from a game that has one.
    OpticalFlowState flow{};
    bool firstFrame = true;
    uint32_t mvecEnabled = 1;
    uint32_t mvecScaleMode = kMVecPixels;
    uint32_t mvecQuality = kMVecBalanced;
    uint32_t mvecPixelSize = kMVecPixels4;
    uint32_t appliedMvecScaleMode = 0xFFFFFFFFu;
    std::vector<uint8_t> prevLuma;
    uint32_t lumaW = 0, lumaH = 0;
    uint32_t sceneCutStreak = 0;
    uint32_t passPriceMB = 0;       // measured cost of the last pass built, device-local MiB
    uint32_t evalTooCheap = 0;      // consecutive frames below the per-megapixel GPU floor
    bool loggedDuplicate = false;   // say "identical frame" once per run, not per frame
    bool sceneCutPending = false;   // exactly one frame over the threshold so far
    bool sceneCutFired = false;     // this run of over-threshold frames has already fired
    uint32_t lastResetLogged = 0xFFFFFFFFu;
    bool mvecResetPending = false;
    bool pendingMvClear = false;  // scene cut: zero MVec inside the prep cmd
};

// How long to wait after a change before rebuilding, and between one rebuild and the next, is now
// the header's rebuildSettleMs -- wall-clock milliseconds, user-adjustable, 0 meaning no spacing.

static void SrcAccessForLayout(VkImageLayout layout, VkAccessFlags* a, VkPipelineStageFlags* s) {
    *a = 0; *s = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    switch (layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *a = VK_ACCESS_TRANSFER_WRITE_BIT; *s = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *a = VK_ACCESS_TRANSFER_READ_BIT; *s = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            *a = VK_ACCESS_SHADER_READ_BIT; *s = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; break;
        case VK_IMAGE_LAYOUT_GENERAL:
            *a = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            *s = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; break;
        default: break;
    }
}

static uint32_t FlowGridBitsToFactor(VkOpticalFlowGridSizeFlagsNV bit) {
    if (bit == VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV) return 1;
    if (bit == VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV) return 2;
    if (bit == VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV) return 4;
    if (bit == VK_OPTICAL_FLOW_GRID_SIZE_8X8_BIT_NV) return 8;
    return 0;
}

static VkOpticalFlowGridSizeFlagsNV ChooseFlowGrid(VkOpticalFlowGridSizeFlagsNV supported,
                                                   uint32_t w, uint32_t h, uint32_t requested) {
    const VkOpticalFlowGridSizeFlagsNV sizes[] = {
        VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_8X8_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV,
    };
    const uint32_t wanted = 1u << requested;
    for (uint32_t i = 0; i < 4; ++i) {
        VkOpticalFlowGridSizeFlagsNV bit = sizes[i];
        uint32_t g = FlowGridBitsToFactor(bit);
        if (g == wanted && (supported & bit) && (w % g) == 0 && (h % g) == 0) return bit;
    }
    VkOpticalFlowGridSizeFlagsNV fallback = VK_OPTICAL_FLOW_GRID_SIZE_UNKNOWN_NV;
    uint32_t fallbackDistance = UINT32_MAX;
    for (uint32_t i = 0; i < 4; ++i) {
        VkOpticalFlowGridSizeFlagsNV bit = sizes[i];
        uint32_t g = FlowGridBitsToFactor(bit);
        if ((supported & bit) && g && (w % g) == 0 && (h % g) == 0) {
            const uint32_t distance = g > wanted ? g - wanted : wanted - g;
            if (distance < fallbackDistance) {
                fallback = bit;
                fallbackDistance = distance;
            }
        }
    }
    return fallback;
}

static bool QueryOpticalFlowFormat(VkCtx& c, VkOpticalFlowUsageFlagsNV usage,
                                   const VkFormat* preferred, uint32_t preferredCount,
                                   VkFormat& out) {
    out = VK_FORMAT_UNDEFINED;
    if (!vkGetPhysicalDeviceOpticalFlowImageFormatsNV) return false;
    VkOpticalFlowImageFormatInfoNV info{};
    info.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV;
    info.usage = usage;
    uint32_t count = 0;
    if (vkGetPhysicalDeviceOpticalFlowImageFormatsNV(c.physical, &info, &count, nullptr) != VK_SUCCESS || !count)
        return false;
    // Every element's sType has to be set before the call, not just the info struct's: this
    // is an array the driver fills in, and it is entitled to read the type of each slot to
    // know what it may write there. Leaving them zeroed is what validation reports, and the
    // fact that NVIDIA's driver fills them anyway is luck, not contract.
    std::vector<VkOpticalFlowImageFormatPropertiesNV> props(count);
    for (auto& p : props) p.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV;
    if (vkGetPhysicalDeviceOpticalFlowImageFormatsNV(c.physical, &info, &count, props.data()) != VK_SUCCESS)
        return false;
    for (uint32_t i = 0; i < preferredCount; ++i) {
        for (auto& p : props) if (p.format == preferred[i]) { out = preferred[i]; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// GPU MVec post pass (decode + deadzone + upscale), zero host readback
// ---------------------------------------------------------------------------
// Bilinear or nearest when the grid is upsampled to full resolution. The shader has
// supported both since it was written and this was hardcoded to 1, so there was no way to
// try the other without a rebuild.
//
// Four independent projects prefer nearest, one of them with a measured 12-16 px
// disocclusion band behind it: across a motion boundary the neighbouring cells describe
// different surfaces, and interpolating them manufactures a vector no cell measured, which
// widens the band instead of narrowing it. Where the field is smooth it varies far more
// slowly than one cell and the two filters agree anyway.
//
// The default stays 1 regardless, because their grids are 1-4 px on 1080p video and ours is
// whatever the session negotiated at the game's raster -- at grid 4 with heavy
// magnification the blockiness argument is stronger for us than for them. This makes the
// experiment possible, not decided.
static uint32_t MVecBilinear() {
    static const uint32_t v = [] {
        const char* p = getenv("DLSSNR_MVEC_BILINEAR");
        return p && p[0] == '0' ? 0u : 1u;
    }();
    return v;
}

// Both gates are read from the environment and both ship inert.
//
// The cost thresholds default equal, which disables the ramp: NVOF cost is an unnormalised
// per-block matching cost whose scale depends on the grid and the performance level, so any
// threshold is per-configuration and a number invented here would silently delete real
// motion. The round trip defaults off because its tolerance has not been calibrated on game
// content -- the project it comes from measured it on video, where every object is real, and
// reported that on synthetic patterns it went the other way. Wired and off is a much better
// state than not wired: measuring either needs a variable, not a rebuild.
static float MVecEnvFloat(const char* name, float def) {
    const char* p = getenv(name);
    if (!p || !*p) return def;
    return (float)atof(p);
}

static float MVecCostLo() {
    static const float v = MVecEnvFloat("DLSSNR_MVEC_COST_LO", 0.0f);
    return v;
}

static float MVecCostHi() {
    static const float v = MVecEnvFloat("DLSSNR_MVEC_COST_HI", 0.0f);
    return v;
}

static bool MVecRoundTrip() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_MVEC_ROUNDTRIP");
        return p && p[0] == '1';
    }();
    return v;
}

// Slack at zero motion, plus this much per pixel of motion. The scaling is the point: a
// 20 px vector is allowed 1.75 px of round-trip error and a static one 0.75, which is what
// separates a large correct vector from a large wrong one. A fixed threshold cannot.
static float MVecRoundTripAbs() {
    static const float v = MVecEnvFloat("DLSSNR_MVEC_ROUNDTRIP_ABS", 0.75f);
    return v;
}

static float MVecRoundTripRel() {
    static const float v = MVecEnvFloat("DLSSNR_MVEC_ROUNDTRIP_REL", 0.05f);
    return v;
}

// Estimate the flow at a fixed target width rather than at the frame's own size.
//
// An absolute width and not a divisor, which is the better parameterisation of the same
// idea: a divisor of 2 gives a different flow cost at 1080p and at 4K, a width gives the
// same one. The projects this comes from default to 640 and report the trade as the same
// grid for half the cost with double the search range in output pixels.
//
// 0 means the frame's own size, and that is the default here: the range claim is measured on
// a different estimator (a D3D12 VME, not NVOF) and has not been reproduced on this one, so
// it is an experiment with a switch rather than a setting with a number. The header is the
// right home for it once somebody has measured what it does to the picture; an env var is
// the right home for it now.
static uint32_t MVecFlowWidth() {
    static const uint32_t v = [] {
        const char* p = getenv("DLSSNR_MVEC_FLOW_WIDTH");
        const long n = p && *p ? strtol(p, nullptr, 10) : 0;
        return n > 0 ? uint32_t(n) : 0u;
    }();
    return v;
}

static float MVecDeadzone() {
    static const float v = [] {
        const char* p = getenv("DLSSNR_MVEC_DEADZONE");
        float f = p && *p ? (float)atof(p) : 0.5f;
        if (!(f >= 0.0f)) f = 0.0f;
        if (f > 8.0f) f = 8.0f;
        return f;
    }();
    return v;
}

static constexpr uint32_t kMVecBindings = 4;

// One place that decides what each binding points at, used by both the pool path and the
// push path so they cannot drift. GENERAL throughout: these are STORAGE_IMAGE descriptors
// and a storage image may only be GENERAL.
static void MVecDescriptorImages(VkCtx& c, OpticalFlowState& f, NeuralState& ns,
                                 VkDescriptorImageInfo* out) {
    out[0] = { nullptr, f.outBitsView, VK_IMAGE_LAYOUT_GENERAL };
    out[1] = { nullptr, ns.mv.view, VK_IMAGE_LAYOUT_GENERAL };
    out[2] = { nullptr, f.costBitsView ? f.costBitsView : c.dummyCost.view, VK_IMAGE_LAYOUT_GENERAL };
    out[3] = { nullptr, f.backBitsView ? f.backBitsView : c.dummyBack.view,
               VK_IMAGE_LAYOUT_GENERAL };
}

static bool EnsureMVecComputeObjects(VkCtx& c) {
    if (c.mvPipeLayout) return true;
    if (!c.mvComputeSupported) return false;
    auto makeModule = [&](const uint32_t* code, size_t len, VkShaderModule& out) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = len * sizeof(uint32_t);
        smci.pCode = code;
        return vkCreateShaderModule(c.device, &smci, nullptr, &out) == VK_SUCCESS;
    };
    // Two modules from one source, one per flow format. The format branch is compile-time because a
    // spec-constant branch mispredicted on this driver and wrote NaN into MVec.
    if (!makeModule(kMVecDeadzoneSpvFixed5, kMVecDeadzoneSpvFixed5Len, c.mvShaderFixed5)) return false;
    if (!makeModule(kMVecDeadzoneSpvFloat, kMVecDeadzoneSpvFloatLen, c.mvShaderFloat)) {
        vkDestroyShaderModule(c.device, c.mvShaderFixed5, nullptr);
        c.mvShaderFixed5 = nullptr;
        return false;
    }
    // Four now: flow in, MVec out, cost, backward flow. The last two are read only when
    // their spec constants say so, but a descriptor has to be valid for every binding the
    // shader declares, so they are always bound -- to a 1x1 stand-in when the session did
    // not grant them.
    VkDescriptorSetLayoutBinding bindings[kMVecBindings] = {};
    for (uint32_t i = 0; i < kMVecBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = kMVecBindings; dli.pBindings = bindings;
    // With push descriptors the images are handed to the command buffer at record time, so
    // there is no pool, no set, and no question of updating a set that may still be in
    // flight. The pool path stays as the fallback -- unlike the project this came from we
    // should not refuse a driver over it, because the existing path works.
    if (c.pushDescriptors) dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    if (vkCreateDescriptorSetLayout(c.device, &dli, nullptr, &c.mvDescLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &c.mvDescLayout;
    if (vkCreatePipelineLayout(c.device, &pli, nullptr, &c.mvPipeLayout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(c.device, c.mvDescLayout, nullptr);
        c.mvDescLayout = nullptr;
        vkDestroyShaderModule(c.device, c.mvShaderFixed5, nullptr);
        vkDestroyShaderModule(c.device, c.mvShaderFloat, nullptr);
        c.mvShaderFixed5 = c.mvShaderFloat = nullptr;
        return false;
    }
    return true;
}

static void DestroyMVecComputePass(VkCtx& c, OpticalFlowState& f) {
    if (f.mvPipeline && vkDestroyPipeline) vkDestroyPipeline(c.device, f.mvPipeline, nullptr);
    f.mvPipeline = nullptr;
    if (f.mvPool && vkDestroyDescriptorPool) vkDestroyDescriptorPool(c.device, f.mvPool, nullptr);
    f.mvPool = nullptr;
    f.mvSet = nullptr;  // freed with the pool
    if (f.outBitsView && vkDestroyImageView) vkDestroyImageView(c.device, f.outBitsView, nullptr);
    f.outBitsView = nullptr;
    if (f.costBitsView && vkDestroyImageView) vkDestroyImageView(c.device, f.costBitsView, nullptr);
    f.costBitsView = nullptr;
    if (f.backBitsView && vkDestroyImageView) vkDestroyImageView(c.device, f.backBitsView, nullptr);
    f.backBitsView = nullptr;
    f.gpuCompute = false;
}

// Builds the per-size compute pipeline + descriptor set. The compute pass reads
// the raw NVOF texels through a size-compatible R16G16_UINT view of the session
// output image and writes filtered vectors straight into the MVec resource.
// The 1x1 stand-ins, made once and kept. Vulkan requires a valid descriptor for every
// binding the shader declares, whether or not the shader reads it, and the two optional
// bindings exist or not depending on the rung the session took. One tiny image each is a far
// smaller price than compiling a variant per combination.
static bool EnsureMVecDummies(VkCtx& c, VkFormat costFormat) {
    if (!c.dummyCost.image) {
        if (!CreateImage2DUsage(c, costFormat == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8_UINT : costFormat,
                                1, 1, VK_IMAGE_USAGE_STORAGE_BIT, c.dummyCost))
            return false;
    }
    if (!c.dummyBack.image) {
        // Created directly as R16G16_UINT rather than as a flow-format image with an alias
        // view over it. The real backward surface has to be aliased because the ENGINE
        // writes it in the flow format and the shader reads it as bits; nothing writes this
        // one, so there is no reason to give it two formats -- and giving it two would need
        // MUTABLE_FORMAT, which is the exact spec violation already fixed once in this file.
        if (!CreateImage2DUsage(c, VK_FORMAT_R16G16_UINT, 1, 1,
                                VK_IMAGE_USAGE_STORAGE_BIT, c.dummyBack))
            return false;
    }
    return true;
}

static bool BuildMVecComputePass(NeuralState& ns, uint32_t ow, uint32_t oh) {
    VkCtx& c = ns.vk;
    OpticalFlowState& f = ns.flow;
    DestroyMVecComputePass(c, f);
    if (!c.mvComputeSupported || !ns.mv.image || !f.out.image || !EnsureMVecComputeObjects(c)) return false;
    if (!EnsureMVecDummies(c, f.costFormat)) {
        Log("[mvec] could not create the stand-in images for the optional bindings");
        return false;
    }
    const bool fixed5 = f.flowFormat == VK_FORMAT_R16G16_SFIXED5_NV;
    if (!fixed5 && f.flowFormat != VK_FORMAT_R16G16_SFLOAT) return false;  // two modules, two formats
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = f.out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_UINT;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(c.device, &vi, nullptr, &f.outBitsView) != VK_SUCCESS) {
        Log("[mvec] R16G16_UINT view on NVOF output failed");
        return false;
    }
    // The backward field is the same format as the forward one and is read the same way, so
    // it gets the same size-compatible R16G16_UINT alias. The cost image is already an
    // integer format and only needs a plain view. Both are optional: a failure here means
    // that gate cannot run, not that the pass cannot.
    if (f.backEnabled && f.back.image) {
        vi.image = f.back.image;
        vi.format = VK_FORMAT_R16G16_UINT;
        if (vkCreateImageView(c.device, &vi, nullptr, &f.backBitsView) != VK_SUCCESS) {
            Log("[mvec] R16G16_UINT view on the backward flow failed; round trip unavailable");
            f.backBitsView = nullptr;
        }
    }
    if (f.costEnabled && f.cost.image) {
        vi.image = f.cost.image;
        vi.format = f.costFormat;
        if (vkCreateImageView(c.device, &vi, nullptr, &f.costBitsView) != VK_SUCCESS) {
            Log("[mvec] cost view failed; the cost ramp is unavailable");
            f.costBitsView = nullptr;
        }
    }
    struct SpecData {
        uint32_t grid, srcW, srcH, dstW, dstH;
        float deadzone;
        uint32_t bilinear;
        float costLo, costHi;
        uint32_t hasCost;
        uint32_t roundTrip;
        float roundTripAbs, roundTripRel;
    } data{};
    data.grid = f.grid ? f.grid : 1u;
    data.srcW = ow; data.srcH = oh;
    data.dstW = ns.mv.width; data.dstH = ns.mv.height;
    data.deadzone = MVecDeadzone();
    data.bilinear = MVecBilinear();
    data.costLo = MVecCostLo();
    data.costHi = MVecCostHi();
    data.hasCost = f.costEnabled && f.costBitsView ? 1u : 0u;
    data.roundTrip = f.backEnabled && f.backBitsView && MVecRoundTrip() ? 1u : 0u;
    data.roundTripAbs = MVecRoundTripAbs();
    data.roundTripRel = MVecRoundTripRel();
    // Constants 0-4 and 6-13; there is no constant 5 any more -- the format moved from a
    // specialization constant to the choice of module.
    VkSpecializationMapEntry entries[13] = {
        {0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4},
        {4, 16, 4}, {6, 20, 4}, {7, 24, 4},
        {8, 28, 4}, {9, 32, 4}, {10, 36, 4},
        {11, 40, 4}, {12, 44, 4}, {13, 48, 4},
    };
    VkSpecializationInfo sp{};
    sp.mapEntryCount = 13; sp.pMapEntries = entries;
    sp.dataSize = sizeof(data); sp.pData = &data;
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = fixed5 ? c.mvShaderFixed5 : c.mvShaderFloat;
    cpi.stage.pName = "main";
    cpi.stage.pSpecializationInfo = &sp;
    cpi.layout = c.mvPipeLayout;
    if (vkCreateComputePipelines(c.device, nullptr, 1, &cpi, nullptr, &f.mvPipeline) != VK_SUCCESS) {
        Log("[mvec] deadzone compute pipeline creation failed");
        DestroyMVecComputePass(c, f);
        return false;
    }
    // Only the pool path needs a pool. With push descriptors the images go to the command
    // buffer at record time, and the whole "is this set still in flight" question never
    // arises -- which is what makes a binding count that varies with the session's rung
    // cheap rather than a rebuild.
    if (!c.pushDescriptors) {
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMVecBindings };
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(c.device, &dpci, nullptr, &f.mvPool) != VK_SUCCESS) {
            DestroyMVecComputePass(c, f);
            return false;
        }
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = f.mvPool; dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &c.mvDescLayout;
        if (vkAllocateDescriptorSets(c.device, &dsai, &f.mvSet) != VK_SUCCESS) {
            DestroyMVecComputePass(c, f);
            return false;
        }
        VkDescriptorImageInfo info[kMVecBindings];
        MVecDescriptorImages(c, f, ns, info);
        VkWriteDescriptorSet writes[kMVecBindings] = {};
        for (uint32_t i = 0; i < kMVecBindings; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = f.mvSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &info[i];
        }
        vkUpdateDescriptorSets(c.device, kMVecBindings, writes, 0, nullptr);
    }
    f.gpuCompute = true;
    Log("[mvec] GPU deadzone pass ready grid=%u flow=%ux%u mvec=%ux%u fixed5=%u deadzone=%.3f",
        data.grid, ow, oh, data.dstW, data.dstH, fixed5 ? 1u : 0u, data.deadzone);
    Log("[mvec] upsample filter: %s  descriptors=%s", data.bilinear ? "bilinear" : "nearest",
        c.pushDescriptors ? "push" : "pool");
    Log("[mvec] cost ramp %s (lo=%.3f hi=%.3f)  round-trip gate %s (abs=%.2f rel=%.3f)",
        data.hasCost && data.costHi > data.costLo ? "ON" : "inert", data.costLo, data.costHi,
        data.roundTrip ? "ON" : "inert", data.roundTripAbs, data.roundTripRel);
    return true;
}

static void DestroyOpticalFlow(VkCtx& c, OpticalFlowState& f) {
    if (f.session && vkDestroyOpticalFlowSessionNV) {
        vkDestroyOpticalFlowSessionNV(c.device, f.session, nullptr);
        f.session = nullptr;
    }
    DestroyMVecComputePass(c, f);
    DestroyImage2D(c, f.prev);
    DestroyImage2D(c, f.curr);
    DestroyImage2D(c, f.out);
    DestroyImage2D(c, f.cost);
    DestroyImage2D(c, f.globalFlow);
    DestroyImage2D(c, f.back);
    f.enabled = false;
    f.inputFormat = f.flowFormat = VK_FORMAT_UNDEFINED;
    f.grid = 1;
    f.quality = kMVecBalanced;
    f.attemptedQuality = 0;
    f.attemptedPixelSize = kMVecPixels4;
    f.userDisabled = false;
    f.hasPrev = false;
    f.hintsStale = true;
    f.costEnabled = false;
    f.globalEnabled = false;
    f.backEnabled = false;
    f.costFormat = VK_FORMAT_UNDEFINED;
    f.currentToPrevious = true;
    f.flowTransferSrc = false;
    f.gpuConvertChecked = false;
    f.loggedFirstFlow = false;
}

static bool SetupOpticalFlow(VkCtx& c, NeuralState& ns, uint32_t w, uint32_t h, uint32_t quality,
                             uint32_t pixelSize) {
    OpticalFlowState& f = ns.flow;
    DestroyOpticalFlow(c, f);
    f.attemptedQuality = quality;
    f.attemptedPixelSize = pixelSize;
    // The size the ENGINE works at, which is the frame's own unless asked otherwise. Aspect
    // preserved and both axes rounded to even, because the grid has to divide the raster
    // evenly and an odd intermediate fails that for every grid but 1.
    const uint32_t fullW = w, fullH = h;
    const uint32_t targetW = MVecFlowWidth();
    if (targetW && targetW < fullW) {
        const double scale = double(targetW) / double(fullW);
        uint32_t fw = uint32_t(std::lround(double(fullW) * scale / 2.0) * 2);
        uint32_t fh = uint32_t(std::lround(double(fullH) * scale / 2.0) * 2);
        if (fw >= 64 && fh >= 64) {
            w = fw;
            h = fh;
            Log("[mvec] estimating flow at %ux%u for a %ux%u frame (target width %u)", w, h,
                fullW, fullH, targetW);
        } else {
            Log("[mvec] flow target width %u would give %ux%u, too small; using the frame size",
                targetW, fw, fh);
        }
    }
    f.inputWidth = w;
    f.inputHeight = h;
    if (!c.opticalFlow || !c.opticalQueue || !c.cmdFlow || !vkCreateOpticalFlowSessionNV ||
        !vkBindOpticalFlowSessionImageNV || !vkCmdOpticalFlowExecuteNV ||
        !vkGetPhysicalDeviceOpticalFlowImageFormatsNV) {
        Log("[mvec] NV optical flow unavailable ext=%d queue=%p cmd=%p create=%p bind=%p exec=%p formats=%p",
            int(c.opticalFlow), (void*)c.opticalQueue, (void*)c.cmdFlow,
            (void*)vkCreateOpticalFlowSessionNV, (void*)vkBindOpticalFlowSessionImageNV,
            (void*)vkCmdOpticalFlowExecuteNV, (void*)vkGetPhysicalDeviceOpticalFlowImageFormatsNV);
        return false;
    }

    VkOpticalFlowGridSizeFlagsNV supported = VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV;
    if (vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceOpticalFlowPropertiesNV props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &props;
        vkGetPhysicalDeviceProperties2(c.physical, &props2);
        if (props.supportedOutputGridSizes) supported = props.supportedOutputGridSizes;
        f.costSupported = props.costSupported == VK_TRUE;
        f.globalSupported = props.globalFlowSupported == VK_TRUE;
        f.bidirSupported = props.bidirectionalFlowSupported == VK_TRUE;
        Log("[mvec] device caps: cost=%d global_flow=%d bidirectional=%d hint=%d grids=%#x",
            int(f.costSupported), int(f.globalSupported), int(f.bidirSupported),
            int(props.hintSupported == VK_TRUE), (unsigned)props.supportedOutputGridSizes);
        if (w < props.minWidth || h < props.minHeight || w > props.maxWidth || h > props.maxHeight) {
            Log("[mvec] size %ux%u outside NVOF limits %ux%u..%ux%u",
                w, h, props.minWidth, props.minHeight, props.maxWidth, props.maxHeight);
            return false;
        }
    }

    VkOpticalFlowGridSizeFlagsNV gridBit = ChooseFlowGrid(supported, w, h, pixelSize);
    f.grid = FlowGridBitsToFactor(gridBit);
    f.quality = quality;
    if (!f.grid) { Log("[mvec] no supported flow grid for %ux%u", w, h); return false; }

    const VkFormat inputPreferred[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    const VkFormat flowPreferred[] = { VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFIXED5_NV };
    if (!QueryOpticalFlowFormat(c, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, inputPreferred,
                                uint32_t(sizeof(inputPreferred) / sizeof(inputPreferred[0])), f.inputFormat) ||
        !QueryOpticalFlowFormat(c, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, flowPreferred,
                                uint32_t(sizeof(flowPreferred) / sizeof(flowPreferred[0])), f.flowFormat)) {
        Log("[mvec] NVOF format query failed");
        return false;
    }
    if (f.flowFormat != VK_FORMAT_R16G16_SFLOAT && f.flowFormat != VK_FORMAT_R16G16_SFIXED5_NV) {
        Log("[mvec] unsupported flow format %d, falling back to zero MVec", (int)f.flowFormat);
        return false;
    }

    f.flowTransferSrc = false;
    if (vkGetPhysicalDeviceFormatProperties) {
        VkFormatProperties src{};
        vkGetPhysicalDeviceFormatProperties(c.physical, f.flowFormat, &src);
        f.flowTransferSrc = (src.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    }
    const char* dirEnv = getenv("DLSSNR_MVEC_DIRECTION");
    f.currentToPrevious = !(dirEnv && dirEnv[0] == '0');

    const uint32_t ow = w / f.grid;
    const uint32_t oh = h / f.grid;
    if (!CreateImage2DOpticalFlow(c, f.inputFormat, w, h, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, f.prev) ||
        !CreateImage2DOpticalFlow(c, f.inputFormat, w, h, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, f.curr) ||
        !CreateImage2DOpticalFlow(c, f.flowFormat, ow, oh, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, f.out)) {
        Log("[mvec] NVOF image creation failed");
        DestroyOpticalFlow(c, f);
        return false;
    }
    const size_t outBytes = ImageSizeBytes(c, f.out);
    std::vector<uint8_t> zeros(outBytes, 0);
    if (!UploadPixels(c, f.out, zeros.data(), zeros.size())) {
        Log("[mvec] failed to clear NVOF output");
        DestroyOpticalFlow(c, f);
        return false;
    }

    // The engine can produce more than a forward vector field, and we have been asking for
    // the least it offers: flags were hardcoded to 0 and costFormat to UNDEFINED, on a device
    // that reports cost, global flow and bidirectional flow all supported.
    //
    // A ladder rather than a single request, because a capability the device advertises can
    // still be refused for this particular session -- a grid, a format and a raster it does
    // not like in combination. Each rung asks for less than the one above and the first that
    // creates wins, so a driver that refuses the extras still gets exactly the session it
    // got before.
    //
    // BOTH_DIRECTIONS is deliberately not a rung yet. It costs a second full flow surface
    // every frame and nothing reads it until the round-trip gate exists; a capability with
    // no consumer is a cost with no benefit. The device says it is available, which is the
    // fact that gate needs.
    const bool wantCost = f.costSupported;
    const bool wantGlobal = f.globalSupported;
    if (wantCost) {
        const VkFormat costPreferred[] = { VK_FORMAT_R8_UINT, VK_FORMAT_R32_UINT };
        if (!QueryOpticalFlowFormat(c, VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, costPreferred,
                                    uint32_t(sizeof(costPreferred) / sizeof(costPreferred[0])),
                                    f.costFormat)) {
            f.costFormat = VK_FORMAT_UNDEFINED;
        }
    }

    // Both directions is only asked for when something will read it: it costs a second full
    // flow surface every frame, and the round-trip gate is the only consumer.
    const bool wantBoth = f.bidirSupported && MVecRoundTrip();

    // The order is measured, not assumed, and the measurement is the reason this is not the
    // obvious ladder. On the driver this was developed against (RTX 5090, Blackwell), asking
    // for ENABLE_COST and BOTH_DIRECTIONS together does not fail -- the session creates, every
    // bind succeeds, validation is clean -- and the engine then writes ZEROS into both the
    // forward and the backward field. Silently. Sampling the surfaces directly is the only
    // way it shows:
    //
    //     cost + global                 forward OK
    //     bidirectional alone           forward OK, backward OK (exactly negated)
    //     global + bidirectional        forward OK, backward OK
    //     cost + bidirectional          both fields ZERO
    //     cost + global + bidirectional both fields ZERO
    //
    // So cost and bidirectional never appear on the same rung. Which family is tried first
    // depends on what is asked for: the round-trip gate needs the backward field, everything
    // else is better served by the cost buffer.
    //
    // This is also the answer to why a ladder at all. A capability the device advertises can
    // be refused for a particular session -- but it can also be accepted and then not
    // delivered, which no return code will tell you about.
    struct Rung { bool cost; bool global; bool both; const char* name; };
    const Rung bidirFirst[] = {
        { false, true,  true,  "global+bidirectional" },
        { false, false, true,  "bidirectional" },
        { true,  true,  false, "cost+global" },
        { true,  false, false, "cost" },
        { false, true,  false, "global" },
        { false, false, false, "forward only" },
    };
    const Rung costFirst[] = {
        { true,  true,  false, "cost+global" },
        { true,  false, false, "cost" },
        { false, true,  false, "global" },
        { false, false, false, "forward only" },
    };
    const Rung* ladder = wantBoth ? bidirFirst : costFirst;
    const size_t ladderCount = wantBoth ? sizeof(bidirFirst) / sizeof(bidirFirst[0])
                                        : sizeof(costFirst) / sizeof(costFirst[0]);

    const char* takenRung = "forward only";
    for (size_t ri = 0; ri < ladderCount; ++ri) {
        const Rung& rung = ladder[ri];
        if (rung.cost && (!wantCost || f.costFormat == VK_FORMAT_UNDEFINED)) continue;
        if (rung.global && !wantGlobal) continue;
        if (rung.both && !wantBoth) continue;

        VkOpticalFlowSessionCreateInfoNV sci{};
        sci.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV;
        sci.width = w;
        sci.height = h;
        sci.imageFormat = f.inputFormat;
        sci.flowVectorFormat = f.flowFormat;
        sci.costFormat = rung.cost ? f.costFormat : VK_FORMAT_UNDEFINED;
        sci.outputGridSize = gridBit;
        sci.hintGridSize = VK_OPTICAL_FLOW_GRID_SIZE_UNKNOWN_NV;
        sci.performanceLevel = quality == kMVecFast ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV
            : quality == kMVecQuality ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV
                                              : VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV;
        sci.flags = 0;
        if (rung.cost) sci.flags |= VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_COST_BIT_NV;
        if (rung.global) sci.flags |= VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_GLOBAL_FLOW_BIT_NV;
        if (rung.both) sci.flags |= VK_OPTICAL_FLOW_SESSION_CREATE_BOTH_DIRECTIONS_BIT_NV;

        if (vkCreateOpticalFlowSessionNV(c.device, &sci, nullptr, &f.session) != VK_SUCCESS) {
            f.session = VK_NULL_HANDLE;
            continue;
        }
        f.costEnabled = rung.cost;
        f.globalEnabled = rung.global;
        f.backEnabled = rung.both;
        takenRung = rung.name;
        break;
    }
    if (!f.session) {
        Log("[mvec] vkCreateOpticalFlowSessionNV failed on every rung");
        DestroyOpticalFlow(c, f);
        return false;
    }

    // The extra surfaces the rung asked for. A failure here drops back to a plain forward
    // session rather than failing the whole flow path: the extras are diagnostics and
    // future inputs, and the vector field is the thing that matters.
    if (f.costEnabled &&
        !CreateImage2DOpticalFlow(c, f.costFormat, ow, oh, VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, f.cost)) {
        Log("[mvec] cost image creation failed; continuing without it");
        f.costEnabled = false;
    }
    if (f.globalEnabled &&
        !CreateImage2DOpticalFlow(c, f.flowFormat, 1, 1, VK_OPTICAL_FLOW_USAGE_GLOBAL_FLOW_BIT_NV,
                                  f.globalFlow)) {
        Log("[mvec] global-flow image creation failed; continuing without it");
        f.globalEnabled = false;
    }
    if (f.backEnabled &&
        !CreateImage2DOpticalFlow(c, f.flowFormat, ow, oh, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
                                  f.back)) {
        Log("[mvec] backward-flow image creation failed; continuing without it");
        f.backEnabled = false;
    }

    GpuImage& refImg = f.currentToPrevious ? f.prev : f.curr;
    GpuImage& inImg = f.currentToPrevious ? f.curr : f.prev;
    VkResult bindRef = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, refImg.view, VK_IMAGE_LAYOUT_GENERAL);
    VkResult bindIn = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, inImg.view, VK_IMAGE_LAYOUT_GENERAL);
    VkResult bindOut = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, f.out.view, VK_IMAGE_LAYOUT_GENERAL);
    if (f.costEnabled) {
        if (vkBindOpticalFlowSessionImageNV(c.device, f.session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_COST_NV, f.cost.view,
                VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS) {
            Log("[mvec] could not bind the cost image; continuing without it");
            f.costEnabled = false;
        }
    }
    if (f.globalEnabled) {
        if (vkBindOpticalFlowSessionImageNV(c.device, f.session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_GLOBAL_FLOW_NV, f.globalFlow.view,
                VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS) {
            Log("[mvec] could not bind the global-flow image; continuing without it");
            f.globalEnabled = false;
        }
    }
    if (f.backEnabled) {
        if (vkBindOpticalFlowSessionImageNV(c.device, f.session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_BACKWARD_FLOW_VECTOR_NV, f.back.view,
                VK_IMAGE_LAYOUT_GENERAL) != VK_SUCCESS) {
            Log("[mvec] could not bind the backward-flow image; continuing without it");
            f.backEnabled = false;
        }
    }
    if (bindRef != VK_SUCCESS || bindIn != VK_SUCCESS || bindOut != VK_SUCCESS) {
        Log("[mvec] failed to bind NVOF session images ref=%d input=%d out=%d",
            (int)bindRef, (int)bindIn, (int)bindOut);
        DestroyOpticalFlow(c, f);
        return false;
    }

    // The deadzone compute pass is the only flow conversion: it runs entirely on the GPU for both
    // flow formats, so there is no host readback path to fall back to. If it cannot be built,
    // estimated motion vectors are unavailable -- the model runs with zeroed vectors.
    const char* compEnv = getenv("DLSSNR_MVEC_COMPUTE");
    if (compEnv && compEnv[0] == '0') {
        Log("[mvec] GPU deadzone pass disabled by env; estimated motion vectors unavailable");
        DestroyOpticalFlow(c, f);
        return false;
    }
    if (!BuildMVecComputePass(ns, ow, oh)) {
        Log("[mvec] GPU deadzone pass unavailable; estimated motion vectors unavailable");
        DestroyOpticalFlow(c, f);
        return false;
    }

    f.enabled = true;
    Log("[mvec] NV optical flow enabled size=%ux%u grid=%u quality=%u input=%d flow=%d dir=%d xfer=%d",
        w, h, f.grid, quality, (int)f.inputFormat, (int)f.flowFormat,
        int(f.currentToPrevious), int(f.flowTransferSrc));
    // "external_hints" rather than "hints": this field is sci.hintGridSize, an external
    // hint GRID that we genuinely do not supply. It is not the session's own temporal
    // hints, which are per-execute and were on unconditionally until they were named here.
    Log("[mvec] session grid=%u rung=%s cost=%s global_flow=%s external_hints=off",
        f.grid, takenRung, f.costEnabled ? "on" : "off", f.globalEnabled ? "on" : "off");
    // Measured at 1080p on this project's reference card: 0.98 ms of GPU at grid 4, 2.57 ms
    // at grid 2, 8.24 ms at grid 1. The finest grid costs eight times the default and
    // NVIDIA's own published comparison does not find it the highest-quality mode, so a
    // session that ends up there should say so rather than quietly spending the time.
    if (f.grid == 1)
        Log("[mvec] grid 1 is the most expensive setting by a wide margin (about 8x grid 4 "
            "at 1080p here) and is not known to be the highest quality; 4 is the default");
    return true;
}

static bool DebugMVecEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_MVEC_DEBUG");
        return p && p[0] == '1';
    }();
    return v;
}

// Sample one NVOF output surface at the centre and report its magnitude statistics. Used for
// the forward field and, when the bidirectional rung was taken, for the backward one -- the
// round-trip gate is only as good as the field it compares against, and "the gate rejected
// everything" and "the backward field is empty" look identical from the outside.
static void LogFlowSurface(NeuralState& ns, GpuImage& img, const char* what) {
    OpticalFlowState& f = ns.flow;
    if (!img.image || !ns.vk.readMap || !ns.vk.readStaging) return;
    const uint32_t rw = img.width < 16u ? img.width : 16u;
    const uint32_t rh = img.height < 16u ? img.height : 16u;
    if (!rw || !rh) return;
    if (!BeginCmd(ns.vk.cmdScratch)) return;
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageOffset = { int32_t(img.width / 2 - rw / 2), int32_t(img.height / 2 - rh / 2), 0 };
    region.imageExtent = { rw, rh, 1 };
    vkCmdCopyImageToBuffer(ns.vk.cmdScratch, img.image, img.layout,
                           ns.vk.readStaging, 1, &region);
    if (!SubmitAndWait(ns.vk, ns.vk.cmdScratch)) return;
    const size_t n = size_t(rw) * rh;
    double mx = 0.0, sum = 0.0;
    size_t nz = 0;
    float sx = 0.0f, sy = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float x, y;
        if (f.flowFormat == VK_FORMAT_R16G16_SFIXED5_NV) {
            const uint16_t* src = (const uint16_t*)ns.vk.readMap;
            x = float(int16_t(src[i * 2 + 0])) / 32.0f;
            y = float(int16_t(src[i * 2 + 1])) / 32.0f;
        } else {
            const uint16_t* src = (const uint16_t*)ns.vk.readMap;
            x = HalfToFloat(src[i * 2 + 0]);
            y = HalfToFloat(src[i * 2 + 1]);
        }
        if (i == 0) { sx = x; sy = y; }
        const double mag = std::sqrt(double(x) * x + double(y) * y);
        if (mag > mx) mx = mag;
        sum += mag;
        if (mag > 0.01) ++nz;
    }
    Log("[mvec] debug %s %ux%u max=%.3f mean=%.3f nonzero=%zu/%zu first=%.3f,%.3f", what, rw, rh,
        mx, sum / double(n), nz, n, sx, sy);
}

static void LogFlowStats(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    if (!f.enabled || !f.flowTransferSrc || !ns.vk.readMap || !ns.vk.readStaging) return;
    if (f.backEnabled && f.back.image) LogFlowSurface(ns, f.back, "backward");
    const uint32_t rw = f.out.width < 16u ? f.out.width : 16u;
    const uint32_t rh = f.out.height < 16u ? f.out.height : 16u;
    if (!rw || !rh) return;
    if (!BeginCmd(ns.vk.cmdScratch)) return;
    VkBufferImageCopy region{};
    region.imageSubresource = { f.out.aspect(), 0, 0, 1 };
    region.imageOffset = { int32_t(f.out.width / 2 - rw / 2), int32_t(f.out.height / 2 - rh / 2), 0 };
    region.imageExtent = { rw, rh, 1 };
    vkCmdCopyImageToBuffer(ns.vk.cmdScratch, f.out.image, f.out.layout,
                           ns.vk.readStaging, 1, &region);
    if (!SubmitAndWait(ns.vk, ns.vk.cmdScratch)) return;
    const size_t count = size_t(rw) * rh;
    double maxMag = 0.0, sumMag = 0.0;
    size_t nonzero = 0;
    if (f.flowFormat == VK_FORMAT_R16G16_SFIXED5_NV) {
        const uint16_t* src = (const uint16_t*)ns.vk.readMap;
        for (size_t i = 0; i < count; ++i) {
            float x = float(int16_t(src[i * 2 + 0])) / 32.0f;
            float y = float(int16_t(src[i * 2 + 1])) / 32.0f;
            double mag = std::sqrt(double(x) * x + double(y) * y);
            if (mag > maxMag) maxMag = mag;
            sumMag += mag;
            if (mag > 0.01) ++nonzero;
        }
    } else if (f.flowFormat == VK_FORMAT_R16G16_SFLOAT) {
        const float* src = (const float*)ns.vk.readMap;
        for (size_t i = 0; i < count; ++i) {
            float x = src[i * 2 + 0];
            float y = src[i * 2 + 1];
            double mag = std::sqrt(double(x) * x + double(y) * y);
            if (mag > maxMag) maxMag = mag;
            sumMag += mag;
            if (mag > 0.01) ++nonzero;
        }
    } else {
        return;
    }
    const uint16_t* raw = (const uint16_t*)ns.vk.readMap;
    Log("[mvec] debug flow center=%ux%u max=%.3f mean=%.3f nonzero=%zu/%zu raw=%u,%u,%u,%u",
        rw, rh, maxMag, count ? sumMag / double(count) : 0.0, nonzero, count,
        count > 0 ? raw[0] : 0, count > 0 ? raw[1] : 0,
        count > 1 ? raw[2] : 0, count > 1 ? raw[3] : 0);
}

static void LogMVecStats(NeuralState& ns) {
    if (!DebugMVecEnabled() || !ns.vk.readMap || !ns.vk.readStaging) return;
    const uint32_t rw = ns.mv.width < 16u ? ns.mv.width : 16u;
    const uint32_t rh = ns.mv.height < 16u ? ns.mv.height : 16u;
    if (!rw || !rh) return;
    if (!BeginCmd(ns.vk.cmdScratch)) return;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { ns.mv.aspect(), 0, 0, 1 };
    region.imageOffset = { int32_t(ns.mv.width / 2 - rw / 2), int32_t(ns.mv.height / 2 - rh / 2), 0 };
    region.imageExtent = { rw, rh, 1 };
    vkCmdCopyImageToBuffer(cb, ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ns.vk.readStaging, 1, &region);
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!SubmitAndWait(ns.vk, cb)) return;
    const size_t count = size_t(rw) * rh;
    const uint16_t* raw = (const uint16_t*)ns.vk.readMap;
    double maxMag = 0.0, sumMag = 0.0;
    size_t nonzero = 0;
    for (size_t i = 0; i < count; ++i) {
        float x = HalfToFloat(raw[i * 2 + 0]);
        float y = HalfToFloat(raw[i * 2 + 1]);
        double mag = std::sqrt(double(x) * x + double(y) * y);
        if (mag > maxMag) maxMag = mag;
        sumMag += mag;
        if (mag > 0.01) ++nonzero;
    }
    Log("[mvec] debug MVec center=%ux%u max=%.3f mean=%.3f nonzero=%zu/%zu raw=%.3f,%.3f,%.3f,%.3f",
        rw, rh, maxMag, count ? sumMag / double(count) : 0.0, nonzero, count,
        count > 0 ? HalfToFloat(raw[0]) : 0.0f, count > 0 ? HalfToFloat(raw[1]) : 0.0f,
        count > 1 ? HalfToFloat(raw[2]) : 0.0f, count > 1 ? HalfToFloat(raw[3]) : 0.0f);
}

static bool MVecFiniteCheck(NeuralState& ns) {
    if (!ns.vk.readMap || !ns.vk.readStaging) return false;
    const uint32_t rw = ns.mv.width < 8u ? ns.mv.width : 8u;
    const uint32_t rh = ns.mv.height < 8u ? ns.mv.height : 8u;
    if (!rw || !rh) return false;
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { ns.mv.aspect(), 0, 0, 1 };
    region.imageOffset = { int32_t(ns.mv.width / 2 - rw / 2), int32_t(ns.mv.height / 2 - rh / 2), 0 };
    region.imageExtent = { rw, rh, 1 };
    vkCmdCopyImageToBuffer(cb, ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ns.vk.readStaging, 1, &region);
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!SubmitAndWait(ns.vk, cb)) return false;
    const size_t count = size_t(rw) * rh;
    const uint16_t* src = (const uint16_t*)ns.vk.readMap;
    for (size_t i = 0; i < count * 2; ++i) {
        float v = HalfToFloat(src[i]);
        if (!std::isfinite(v)) {
            Log("[mvec] GPU MVec check invalid idx=%zu raw=%u decoded=%.3f first=%.3f,%.3f",
                i, src[i], v, HalfToFloat(src[0]), HalfToFloat(src[1]));
            return false;
        }
    }
    return true;
}

static void ResetFlowTimestampQueries(VkCtx& c, VkCommandBuffer cb) {
    if (c.flowQueryAvailable && vkCmdResetQueryPool) {
        vkCmdResetQueryPool(cb, c.flowQuery, 0, 2);
    }
}

static void WriteFlowTimestampBegin(VkCtx& c, VkCommandBuffer cb) {
    if (!c.flowQueryAvailable) return;
    if (vkCmdWriteTimestamp) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, c.flowQuery, 0);
    } else if (c.sync2 && vkCmdWriteTimestamp2) {
        // ALL_COMMANDS, not the optical-flow stage: both flow timestamps are recorded into
        // graphics-queue buffers (prep and post), which bracket the NVOF submit rather than
        // containing it.
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c.flowQuery, 0);
    }
}

static void WriteFlowTimestampEnd(VkCtx& c, VkCommandBuffer cb) {
    if (!c.flowQueryAvailable) return;
    if (vkCmdWriteTimestamp) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, c.flowQuery, 1);
    } else if (c.sync2 && vkCmdWriteTimestamp2) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c.flowQuery, 1);
    }
}

static void CopyFlowTimestampResults(VkCtx& c, VkCommandBuffer cb) {
    if (!c.flowQueryAvailable) return;
    vkCmdCopyQueryPoolResults(cb, c.flowQuery, 0, 2, c.queryStaging, 0, 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

// The model's own GPU cost. No ticket ring and no non-blocking read here, deliberately:
// each pass already ends in SubmitAndWait, so the CPU has finished waiting by the time
// these are read and the measurement adds no stall of its own. A ring would be the right
// answer only if reading forced a wait that did not already exist.
static void ResetEvalTimestampQueries(VkCtx& c, VkCommandBuffer cb) {
    if (c.evalQueryAvailable && vkCmdResetQueryPool) vkCmdResetQueryPool(cb, c.evalQuery, 0, 2);
}

static void WriteEvalTimestamp(VkCtx& c, VkCommandBuffer cb, uint32_t index) {
    if (!c.evalQueryAvailable) return;
    if (vkCmdWriteTimestamp) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, c.evalQuery, index);
    } else if (c.sync2 && vkCmdWriteTimestamp2) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c.evalQuery, index);
    }
}

static void CopyEvalTimestampResults(VkCtx& c, VkCommandBuffer cb) {
    if (!c.evalQueryAvailable) return;
    vkCmdCopyQueryPoolResults(cb, c.evalQuery, 0, 2, c.evalStaging, 0, 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

// Milliseconds, or -1 when the pair is not a usable measurement. The scope is what this
// brackets -- our barriers plus whatever the snippet recorded into our command buffer --
// and not necessarily only the model's work: if the snippet submits to another queue,
// these timestamps do not see it.
static double ReadEvalTimestampMs(VkCtx& c) {
    if (!c.evalQueryAvailable || !c.evalMap) return -1.0;
    const uint64_t* q = (const uint64_t*)c.evalMap;
    if (q[0] == 0 || q[1] == 0 || q[1] < q[0]) return -1.0;
    return double(q[1] - q[0]) * double(c.timestampPeriod) / 1000000.0;
}

static double ReadFlowTimestampMs(VkCtx& c) {
    if (!c.flowQueryAvailable || !c.queryMap) return -1.0;
    const uint64_t* q = (const uint64_t*)c.queryMap;
    if (q[0] == 0 || q[1] == 0 || q[1] < q[0]) return -1.0;
    return double(q[1] - q[0]) * double(c.timestampPeriod) / 1000000.0;
}

static bool ForceMvShaderRead(NeuralState& ns);

// The proxy arrives in the layer's exported memory, released to FOREIGN with the request. Acquire
// it from there into a transfer source and copy it into colorIn, all inside the caller's command
// buffer -- the same submit the upload used to ride along in. The image's own layout tracking is
// reset to GENERAL afterwards because the producer rewrites it in that layout next frame.
static void RecordProxyToColorIn(NeuralState& ns, VkCommandBuffer cb) {
    VkImageMemoryBarrier acq{};
    acq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acq.srcAccessMask = 0;
    acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    acq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acq.image = ns.vk.proxyIn.image;
    acq.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &acq);
    ns.vk.proxyIn.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    TransitionImage(ns.vk, cb, ns.colorIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = { ns.vk.proxyIn.aspect(), 0, 0, 1 };
    cp.dstSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
    cp.extent = { ns.colorIn.width, ns.colorIn.height, 1 };
    vkCmdCopyImage(cb, ns.vk.proxyIn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ns.colorIn.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    // Hand the proxy back before the producer's next write touches it.
    VkImageMemoryBarrier prel{};
    prel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    prel.dstAccessMask = 0;
    prel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    prel.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    prel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    prel.image = ns.vk.proxyIn.image;
    prel.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &prel);
    ns.vk.proxyIn.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// colorIn into one of the engine's input surfaces. A straight copy at the usual size, and a
// linear blit when the flow is being estimated smaller -- a copy cannot resize. Same shape
// as the chain's own 16-to-8-bit step, which blits for the same reason.
static void CopyOrScaleIntoFlowInput(NeuralState& ns, VkCommandBuffer cb, GpuImage& dst) {
    if (dst.width == ns.colorIn.width && dst.height == ns.colorIn.height) {
        VkImageCopy copy{};
        copy.srcSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
        copy.dstSubresource = { dst.aspect(), 0, 0, 1 };
        copy.extent = { ns.colorIn.width, ns.colorIn.height, 1 };
        vkCmdCopyImage(cb, ns.colorIn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        return;
    }
    VkImageBlit blit{};
    blit.srcSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
    blit.dstSubresource = { dst.aspect(), 0, 0, 1 };
    blit.srcOffsets[1] = { int32_t(ns.colorIn.width), int32_t(ns.colorIn.height), 1 };
    blit.dstOffsets[1] = { int32_t(dst.width), int32_t(dst.height), 1 };
    vkCmdBlitImage(cb, ns.colorIn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
}

static bool RunOpticalFlow(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    if (!f.enabled) return true;

    const bool async = ns.vk.semPrep && ns.vk.semFlow;
    if (!f.gpuCompute || !f.outBitsView || !f.mvPipeline || !async) {
        // The deadzone compute pass is the only conversion now; without it (or without the
        // semaphores that chain the three submits) there is nothing to run this frame. The caller
        // disables estimated motion vectors on failure and the model runs with zeroed vectors.
        Log("[mvec] GPU deadzone pass unavailable at run time");
        return false;
    }

    // ---- cmdPrep (graphics): upload colorIn, feed NVOF inputs ----
    // The proxy comes from the imported mapping when there is one, from staging otherwise; either
    // way this submit also carries the scene-cut MVec clear so no extra host round-trip is needed.
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    if (ns.proxyActive) {
        RecordProxyToColorIn(ns, cb);
    } else {
        TransitionImage(ns.vk, cb, ns.colorIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy upRegion{};
        upRegion.imageSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
        upRegion.imageExtent = { ns.colorIn.width, ns.colorIn.height, 1 };
        vkCmdCopyBufferToImage(cb, UploadSource(ns.vk), ns.colorIn.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upRegion);
    }
    TransitionImage(ns.vk, cb, ns.colorIn, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (ns.pendingMvClear) {
        VkAccessFlags mvSrcA; VkPipelineStageFlags mvSrcS;
        SrcAccessForLayout(ns.mv.layout, &mvSrcA, &mvSrcS);
        TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mvSrcA,
                        VK_ACCESS_TRANSFER_WRITE_BIT, mvSrcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range = { ns.mv.aspect(), 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &zero, 1, &range);
        TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ns.pendingMvClear = false;
    }

    if (!f.hasPrev) {
        TransitionImage(ns.vk, cb, f.prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        CopyOrScaleIntoFlowInput(ns, cb, f.prev);
        if (!SubmitAndWait(ns.vk, cb)) return false;
        f.hasPrev = true;
        return true;
    }

    {
        VkAccessFlags currSrcA; VkPipelineStageFlags currSrcS;
        SrcAccessForLayout(f.curr.layout, &currSrcA, &currSrcS);
        TransitionImage(ns.vk, cb, f.curr, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        currSrcA, VK_ACCESS_TRANSFER_WRITE_BIT, currSrcS,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
        CopyOrScaleIntoFlowInput(ns, cb, f.curr);
    }

    ResetFlowTimestampQueries(ns.vk, cb);

    // These three barriers run on the GRAPHICS queue and used to name
    // VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV as their destination, which is not a stage
    // this queue family supports -- a stage mask has to be legal for the queue the command
    // buffer is submitted to, not for the queue that eventually consumes the data.
    //
    // Naming the consumer here was never what ordered the two queues anyway: the NVOF
    // execute is a separate submit on the optical-flow queue that waits on semPrep, and a
    // binary semaphore's signal makes these writes available and its wait makes them
    // visible. So the barrier's job on this side is the layout transition and making prior
    // work available, which ALL_COMMANDS plus MEMORY_READ/WRITE states conservatively and
    // legally. The images are CONCURRENT across the two families, so no ownership transfer
    // is owed either.
    {
        VkAccessFlags prevSrcA; VkPipelineStageFlags prevSrcS;
        SrcAccessForLayout(f.prev.layout, &prevSrcA, &prevSrcS);
        TransitionImage2(ns.vk, cb, f.prev, VK_IMAGE_LAYOUT_GENERAL,
                         prevSrcA, VK_ACCESS_2_MEMORY_READ_BIT,
                         prevSrcS, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    TransitionImage2(ns.vk, cb, f.curr, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    TransitionImage2(ns.vk, cb, f.out, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    // The extra surfaces the rung granted are written by the engine exactly as f.out is, so
    // they need the same layout. A no-op after the first frame -- the transition helper
    // tracks what it last set.
    if (f.costEnabled && f.cost.image)
        TransitionImage2(ns.vk, cb, f.cost, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (f.backEnabled && f.back.image)
        TransitionImage2(ns.vk, cb, f.back, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    WriteFlowTimestampBegin(ns.vk, cb);
    const int prepFence = SubmitAsync(ns.vk, cb, ns.vk.queue, 0, nullptr,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                      async ? ns.vk.semPrep : nullptr);
    if (prepFence < 0) return false;
    if (!async && !WaitFence(ns.vk, prepFence)) return false;

    // ---- NVOF execute on the dedicated optical-flow queue (no CPU wait) ----
    if (!BeginCmd(ns.vk.cmdFlow)) { WaitFence(ns.vk, prepFence); return false; }
    VkCommandBuffer fcb = ns.vk.cmdFlow;
    VkOpticalFlowExecuteInfoNV exec{};
    exec.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV;
    // The flag is negative, so a zeroed flags word means hints ENABLED -- which is what
    // this was, always. The engine carries temporal hints from its previous execute, and
    // across a scene cut, a session rebuild or a raster change those hints describe a
    // scene that is no longer on screen. We already go to the trouble of dropping our own
    // flow history and clearing MVec on a cut; the one piece of state we cannot see is the
    // one piece we never cleared.
    const bool discardHints = f.hintsStale || ns.mvecResetPending;
    if (discardHints) exec.flags = VK_OPTICAL_FLOW_EXECUTE_DISABLE_TEMPORAL_HINTS_BIT_NV;
    vkCmdOpticalFlowExecuteNV(fcb, f.session, &exec);
    f.hintsStale = false;
    // TOP_OF_PIPE: the wait must cover every command in this buffer (the NVOF
    // session reads its inputs through driver-private stages). ALL_COMMANDS is
    // invalid in pWaitDstStageMask on the optical-flow-only queue (VUID-00066)
    // and the driver silently drops it; TOP_OF_PIPE releases only after the
    // signaling submit's full first sync scope, ordering everything.
    const int flowFence = SubmitAsync(ns.vk, fcb, ns.vk.opticalQueue,
                                      async ? 1u : 0u, async ? &ns.vk.semPrep : nullptr,
                                      VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                                      async ? ns.vk.semFlow : nullptr);
    if (flowFence < 0) { WaitFence(ns.vk, prepFence); return false; }
    if (!async && !WaitFence(ns.vk, flowFence)) { WaitFence(ns.vk, prepFence); return false; }

    // ---- cmdPost (graphics): harvest flow + GPU deadzone pass ----
    if (!BeginCmd(ns.vk.cmdFlowPost)) {
        WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence);
        return false;
    }
    cb = ns.vk.cmdFlowPost;
    WriteFlowTimestampEnd(ns.vk, cb);
    CopyFlowTimestampResults(ns.vk, cb);

    // The compute pass reads the raw NVOF texels in-place through the
    // R16G16_UINT view, decodes, deadzone-clamps and upscales straight into
    // the MVec resource. No host readback, no image->image bit copy.
    // Same again in the other direction: this buffer is on the graphics queue, so the SOURCE
    // cannot name the optical-flow stage either. semFlow is what made the NVOF writes
    // available and visible here; this barrier only has to transition the layout and order
    // the compute read after it.
    TransitionImage2(ns.vk, cb, f.out, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    TransitionImage2(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    if (f.costEnabled && f.cost.image)
        TransitionImage2(ns.vk, cb, f.cost, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    if (f.backEnabled && f.back.image)
        TransitionImage2(ns.vk, cb, f.back, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    // The stand-ins are never written and never read, but they ARE bound, and a bound
    // storage image has to be in the layout its descriptor claims.
    if (ns.vk.dummyCost.image)
        TransitionImage2(ns.vk, cb, ns.vk.dummyCost, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    if (ns.vk.dummyBack.image)
        TransitionImage2(ns.vk, cb, ns.vk.dummyBack, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f.mvPipeline);
    if (ns.vk.pushDescriptors) {
        VkDescriptorImageInfo info[kMVecBindings];
        MVecDescriptorImages(ns.vk, f, ns, info);
        VkWriteDescriptorSet writes[kMVecBindings] = {};
        for (uint32_t i = 0; i < kMVecBindings; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &info[i];
        }
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ns.vk.mvPipeLayout, 0,
                                  kMVecBindings, writes);
    } else {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ns.vk.mvPipeLayout,
                                0, 1, &f.mvSet, 0, nullptr);
    }
    vkCmdDispatch(cb, (ns.mv.width + 7) / 8, (ns.mv.height + 7) / 8, 1);
    TransitionImage2(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    TransitionImage2(ns.vk, cb, f.curr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_2_MEMORY_READ_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    TransitionImage2(ns.vk, cb, f.prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_2_MEMORY_READ_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    {
        VkImageCopy copy{};
        copy.srcSubresource = { f.curr.aspect(), 0, 0, 1 };
        copy.dstSubresource = { f.prev.aspect(), 0, 0, 1 };
        copy.extent = { f.curr.width, f.curr.height, 1 };
        vkCmdCopyImage(cb, f.curr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       f.prev.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    const int postFence = SubmitAsync(ns.vk, cb, ns.vk.queue,
                                      async ? 1u : 0u, async ? &ns.vk.semFlow : nullptr,
                                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, nullptr);
    if (postFence < 0) {
        WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence);
        return false;
    }
    // Single host stall for the whole flow stage: prep/flow/post are chained by
    // semaphores, so waiting on the last fence covers all three. The prep/flow
    // fences must still be reset here: the ring reuses slots, and an unreset
    // signaled fence would make a later WaitFence return before its work ran.
    if (!WaitFence(ns.vk, postFence)) { WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence); return false; }
    if (async) { WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence); }

    if (!f.loggedFirstFlow) {
        f.loggedFirstFlow = true;
        Log("[mvec] first optical-flow pass completed");
    }
    if (TimeEnabled()) {
        const double flowMs = ReadFlowTimestampMs(ns.vk);
        static int flowFrame = 0;
        if (++flowFrame % TimeInterval() == 0) {
            if (flowMs >= 0.0) {
                Log("[time] flow_gpu=%.2f ms", flowMs);
                if (flowMs > 2.0) Log("[mvec] warning flow_gpu=%.2f ms exceeds 2ms target", flowMs);
            } else if (ns.vk.flowQueryAvailable && ns.vk.queryMap) {
                const uint64_t* q = (const uint64_t*)ns.vk.queryMap;
                Log("[time] flow_gpu unavailable q0=%llu q1=%llu period=%.3f",
                    (unsigned long long)q[0], (unsigned long long)q[1], ns.vk.timestampPeriod);
            }
        }
    }
    if (DebugMVecEnabled()) {
        static int dbgFrame = 0;
        if (dbgFrame < 5) {
            ++dbgFrame;
            LogFlowStats(ns);
        }
    }

    if (!f.gpuConvertChecked) {
        f.gpuConvertChecked = true;
        if (DebugMVecEnabled() && !MVecFiniteCheck(ns)) {
            Log("[mvec] GPU deadzone pass produced invalid MVec, disabling compute path");
            f.gpuCompute = false;
            if (!ForceMvShaderRead(ns)) return false;
            ns.mvecResetPending = true;
        }
    }
    if (DebugMVecEnabled()) {
        static int dbgMVec = 0;
        if (dbgMVec < 5) {
            ++dbgMVec;
            LogMVecStats(ns);
        }
    }

    return true;
}

static bool ForceMvShaderRead(NeuralState& ns) {
    if (ns.mv.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return true;
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    TransitionImage(ns.vk, ns.vk.cmdScratch, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return SubmitAndWait(ns.vk, ns.vk.cmdScratch);
}

static bool ClearMotionVectors(NeuralState& ns) {
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    VkAccessFlags srcA; VkPipelineStageFlags srcS;
    SrcAccessForLayout(ns.mv.layout, &srcA, &srcS);
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, srcA,
                    VK_ACCESS_TRANSFER_WRITE_BIT, srcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range = { ns.mv.aspect(), 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return SubmitAndWait(ns.vk, cb);
}

static int ReactivateMotionVectors(NeuralState& ns, uint32_t w, uint32_t h, uint32_t quality) {
    if (vkDeviceWaitIdle && vkDeviceWaitIdle(ns.vk.device) != VK_SUCCESS) {
        Log("[mvec] device wait failed during reactivation");
        return -1;
    }
    DestroyOpticalFlow(ns.vk, ns.flow);
    ns.flow.userDisabled = false;
    if (!ClearMotionVectors(ns)) {
        Log("[mvec] clear MVec failed during reactivation");
        return -1;
    }
    ns.firstFrame = true;
    ns.mvecResetPending = true;
    ns.prevLuma.clear();
    ns.lumaW = ns.lumaH = 0;
    ns.sceneCutStreak = 0;
    ns.sceneCutPending = false;
    ns.sceneCutFired = false;
    ns.lastResetLogged = 0xFFFFFFFFu;
    if (!SetupOpticalFlow(ns.vk, ns, w, h, quality, ns.mvecPixelSize)) {
        ns.flow.attemptedQuality = quality;
        ns.flow.userDisabled = false;
        Log("[mvec] reactivation setup failed, using zero MVec");
        return 0;
    }
    Log("[mvec] reactivated quality=%u grid=%u", quality, ns.flow.grid);
    return 1;
}

// Returns whether this frame is a scene cut, and reports separately whether it is a
// DUPLICATE of the previous one.
//
// The two look identical to a run-length rule and are opposites. A cut says the history is
// worthless; a duplicate is a confident statement that nothing moved, which is the strongest
// possible evidence that the history is still good. Both produce a zero motion field and
// only one of them should reset anything.
//
// The test is the mean absolute luma difference over the grid being exactly zero. That is
// an integer mean, so zero means the average difference is under one part in 255 across the
// whole frame -- which is a duplicate or close enough to one that estimating flow between
// the pair is a full NVOF execute spent producing a field we already know is zero. No
// invented float threshold: the quantisation is the threshold.
static bool DetectSceneCut(NeuralState& ns, ShmHeader* hdr, const uint8_t* in, uint32_t w, uint32_t h,
                           uint32_t fmt, bool* duplicate) {
    if (duplicate) *duplicate = false;
    // The header is the control; the environment only seeds it, once, at startup (see
    // SeedSceneCutFromEnv). Read every frame rather than latched in a static, because the whole
    // point of moving it into the header is that it can be moved while a game is running.
    const int threshold = hdr ? int(hdr->sceneCutThreshold.load()) : int(kSceneCutThresholdDefault);
    if (threshold <= 0 || !in || !w || !h) return false;

    const uint32_t gw = w < 64 ? w : 64;
    const uint32_t gh = h < 36 ? h : 36;
    const size_t n = size_t(gw) * gh;
    bool sizeChanged = ns.prevLuma.size() != n || ns.lumaW != gw || ns.lumaH != gh;
    if (sizeChanged) {
        ns.prevLuma.assign(n, 0);
        ns.lumaW = gw; ns.lumaH = gh;
        ns.sceneCutStreak = 0;
        ns.sceneCutPending = false;
        ns.sceneCutFired = false;
    }

    uint64_t sum = 0;
    for (uint32_t y = 0; y < gh; ++y) {
        uint32_t py = uint32_t((uint64_t(y) * h + gh / 2) / gh);
        for (uint32_t x = 0; x < gw; ++x) {
            uint32_t px = uint32_t((uint64_t(x) * w + gw / 2) / gw);
            const uint8_t* p = in + (size_t(py) * w + px) * 4;
            uint32_t r = fmt == 0 ? p[2] : p[0];
            uint32_t g = fmt == 0 ? p[1] : p[1];
            uint32_t b = fmt == 0 ? p[0] : p[2];
            uint8_t luma = uint8_t((r * 77 + g * 150 + b * 29) >> 8);
            size_t idx = size_t(y) * gw + x;
            uint8_t prev = ns.prevLuma[idx];
            sum += luma > prev ? luma - prev : prev - luma;
            ns.prevLuma[idx] = luma;
        }
    }
    if (sizeChanged) return false;
    int mean = int(sum / n);
    // Published whether or not it crossed. A threshold with no score beside it cannot be tuned:
    // this is the number the user is choosing against.
    if (hdr) hdr->sceneCutScore.store(uint32_t(mean));
    if (mean == 0 && duplicate) *duplicate = true;
    // What the shape of the signal means, which the old "two consecutive frames over the
    // threshold" rule had backwards:
    //
    //   a clean cut  -> ONE frame over (the first frame of the new scene differs from the
    //                   last frame of the old one), then back under, because the new scene
    //                   is compared against itself from then on. The old rule required two
    //                   in a row, so the one event this function is named for was the one
    //                   event it filtered out.
    //   a flash      -> two or more over: into the flash and back out of it.
    //   a fade, a fast pan -> a sustained run over. The old rule fired on EVERY frame of
    //                   such a run, which is the over-firing NVIDIA's programming guide
    //                   §3.13 warns costs temporal flickering and heavy aliasing.
    //
    // So: confirm an isolated cut one frame later (high, then low), and fire a sustained
    // run exactly once at its second frame. Every case now fires once and no case fires
    // never. The isolated cut's reset therefore lands on the frame after the cut rather
    // than on the cut itself -- one frame late, against never, and §3.13's argument is
    // that a late true positive is the cheaper error.
    const bool over = mean >= threshold;
    if (over) ++ns.sceneCutStreak; else ns.sceneCutStreak = 0;

    bool cut = false;
    const char* why = "";
    if (!over && ns.sceneCutPending) { cut = true; why = "isolated"; }
    if (over && ns.sceneCutStreak == 2 && !ns.sceneCutFired) {
        cut = true;
        why = "sustained";
        ns.sceneCutFired = true;
    }
    ns.sceneCutPending = (ns.sceneCutStreak == 1);
    if (!over) ns.sceneCutFired = false;

    if (cut) {
        if (hdr) hdr->sceneCutCount.fetch_add(1);
        Log("[mvec] scene cut detected (%s) mean=%d threshold=%d streak=%u", why, mean, threshold,
            ns.sceneCutStreak);
    }
    if (duplicate && *duplicate && !ns.loggedDuplicate) {
        ns.loggedDuplicate = true;
        Log("[mvec] identical frame received; skipping the flow estimate while it repeats");
    }
    if (duplicate && !*duplicate) ns.loggedDuplicate = false;
    return cut;
}

static NgxTuning TuningFor(const ShmHeader* h, uint32_t pass) {
    const PassTuning p = ShmResolvePass(h, pass);
    NgxTuning t;
    t.intensity = ClampF(p.intensity, 0.0f, 4.0f);
    t.localTone = ClampF(p.localTone, 0.0f, 4.0f);
    t.localStructure = ClampF(p.localStructure, 0.0f, 4.0f);
    t.skinStructure = ClampF(p.skinStructure, -1.0f, 4.0f);
    // Style clamped to the three the model actually has, measured rather than assumed.
    //
    // Sweeping 0..6 through this model over identical synthetic frames: 0 and 1 each produce their
    // own picture, and 2, 3, 4, 5 and 6 produce byte-identical output -- three runs each, the same
    // three sums every time. The DLL clamps out-of-range styles to its last one and says nothing.
    //
    // So clamping here changes no behaviour and makes the log tell the truth: the "create tuning"
    // line now reports the style the model was given rather than the number the header held, which
    // is the whole reason every other field on this struct is clamped in this function too.
    t.style = p.style <= 2u ? p.style : 2u;
    // Preset is NOT clamped, on the same evidence and for the opposite reason.
    //
    // 'DLSSNR.Hint.Render.Preset' is a real key and the parameter probe shows the DLL reading it at
    // create on every build -- but 0, 1, 2, 3, 4, 7 and 15 all produced byte-identical output here.
    // Read and inert is not the same as out of range, and inventing a 0..3 bound would assert a
    // boundary this measurement does not support in either direction. The GUI says what was
    // measured instead.
    t.preset = p.preset;
    t.autoMask = p.autoMask ? 1u : 0u;
    t.uiCorrection = p.uiCorrection ? 1u : 0u;
    return t;
}

static void PublishStatus(ShmMap& shm, NeuralState& ns, uint32_t state) {
    if (!shm.hdr) return;
    shm.hdr->helperState.store(state);
    shm.hdr->modelUp.store(ns.ngx.ready && !ns.ngx.disabled ? 1u : 0u);
    shm.hdr->helperFeatures.store(ns.livePasses);
    ShmStore64(shm.hdr->helperFramesLo, shm.hdr->helperFramesHi, ns.evaluates);
}

static float HalfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (!mant) {
            bits = sign;
        } else {
            exp = 1;
            while (!(mant & 0x400u)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | ((exp + 112u) << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// What the model is told the motion field's numbers mean.
// MVecScaleX/Y MULTIPLY the stored vector to give pixels. That is the direction the DLSS
// integration contract defines -- a field in UV space is accompanied by a scale of the render
// width, not its reciprocal -- and it is the direction our own default is consistent with: the
// deadzone shader writes full-resolution pixels and this writes 1.0 beside them.
//
// Both other modes had it inverted. "UV 0..1" wrote 1/w, which converts pixels TO UV rather than UV
// to pixels, and "Normalised" wrote 2/w where -1..1 needs w/2. Reciprocals of the conversion each
// one names, so selecting either did not mis-scale the field a little -- at 1920 wide it scaled it
// by about a millionth, which is a field of zeros with extra steps.
//
// They stay because they describe what the FIELD holds, and a field that is not ours may hold
// either. Ours always holds pixels, which the tooltip now says.
static void ApplyMotionScale(NgxSnippet& ngx, uint32_t mode, uint32_t w, uint32_t h) {
    float sx = float(w) * 0.5f, sy = float(h) * 0.5f;   // -1..1 across the frame
    if (mode == kMVecPixels) { sx = 1.0f; sy = 1.0f; }
    else if (mode == kMVecUv01) { sx = float(w); sy = float(h); }
    NgxSetMotionScale(ngx, sx, sy);
}

static bool EnsureNeural(NeuralState& ns, ShmMap& shm, uint32_t w, uint32_t h) {
    const bool wantSdr16 = !ns.hdrBuilt && shm.hdr->sdr16Multipass.load() != 0;
    if (ns.ready && ns.w == w && ns.h == h && ns.sdr16Built == wantSdr16) return true;
    if (ns.ngx.disabled) return false;

    if (ns.ngx.snippet) NgxReleaseAllPasses(ns.ngx, ns.vk.device);
    DestroyOpticalFlow(ns.vk, ns.flow);
    DestroyImage2D(ns.vk, ns.colorIn);
    DestroyImage2D(ns.vk, ns.colorOut);
    DestroyImage2D(ns.vk, ns.workA);
    DestroyImage2D(ns.vk, ns.workB);
    DestroyImage2D(ns.vk, ns.mv);
    DestroyImage2D(ns.vk, ns.depth);
    ns.livePasses = 0;
    std::memset(ns.passDirty, 0, sizeof(ns.passDirty));

    const VkFormat chainFmt = ns.hdrBuilt ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    // The passes run between these two at sixteen bits, not eight.
    //
    // BindPass ping-pongs workA and workB, so with more than one pass the picture is written and read
    // back once per pass. At eight bits that is a fresh rounding of all three channels every time,
    // and the three round independently -- so a smooth surface picks up a small hue error that the
    // next pass cannot tell from detail and therefore enhances. Three passes compounds it into
    // coloured speckle, which is why the fault appears at two passes and above and never at one, why
    // it sits on flat ground rather than edges, and why it moves colour without moving brightness.
    //
    // Sixteen-bit unorm, not float: the same [0,1] range and the same interpretation, so nothing
    // downstream has to be told about it, and the model is handed the same numbers it always was with
    // eight times the room between them. The transport stays eight-bit; the chain is brought down to
    // it once, at the end, instead of once per pass.
    const VkFormat workFmt = ns.hdrBuilt ? VK_FORMAT_R16G16B16A16_SFLOAT
                                         : wantSdr16 ? VK_FORMAT_R16G16B16A16_UNORM
                                                     : VK_FORMAT_R8G8B8A8_UNORM;
    ns.ngx.hdrActive = ns.hdrBuilt != 0;
    ns.sdr16Built = wantSdr16;
    if (!CreateImage2D(ns.vk, chainFmt, w, h, ns.colorIn) ||
        !CreateImage2D(ns.vk, chainFmt, w, h, ns.colorOut) ||
        !CreateImage2D(ns.vk, workFmt, w, h, ns.workA) ||
        !CreateImage2D(ns.vk, workFmt, w, h, ns.workB) ||
        !CreateImage2D(ns.vk, VK_FORMAT_R16G16_SFLOAT, w, h, ns.mv) ||
        !CreateImage2D(ns.vk, VK_FORMAT_R32_SFLOAT, w, h, ns.depth)) {
        Log("[helper] image creation failed at %ux%u", w, h);
        ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                       "could not allocate the model's surfaces");
        return false;
    }

    // Depth stays zero: a present-time layer has none, and the model treats a flat depth buffer as
    // "no parallax to reason about" rather than as a lie about the scene.
    //
    // Motion is initialized after the first successful model frame. Some games create transient
    // probe swapchains during startup; creating an NVOF session for those rasters can race the
    // game's own Vulkan initialization on Proton-GE. The first frame already has a valid zero field,
    // and ProcessFrame enables NVOF on the following frame once this raster has proved stable.
    ns.flow.attemptedQuality = UINT32_MAX;

    std::vector<uint8_t> zeros(size_t(w) * h * 4, 0);
    if (!UploadPixels(ns.vk, ns.mv, zeros.data(), zeros.size())) return false;

    // DLSSNR_DEPTH_FILL is the probe that settles whether the depth channel is worth building a
    // proxy for. Default 0.0, which is what has always been uploaded; set it to 0.5 or 1.0 and see
    // whether the output digest moves at all. A flat buffer is a flat buffer whatever its value, so
    // if the model's answer is identical at 0.0, 0.5 and 1.0 then this channel is not reading
    // anything a present-time layer could supply, and no depth proxy is worth building.
    //
    // Kept rather than deleted after the answer, because the answer belongs to THIS model build and
    // whoever swaps the model can re-run it in one line.
    // The ControlMask probe, built only when asked for.
    //
    // MVE-04 and DXL-05 both want this key plumbed through as a user-configurable region. Before
    // any of that, the question is whether binding it does anything at all, and a checkerboard
    // answers it in one run: at 64-pixel squares half the frame is masked, so any effect at all is
    // unmissable in the digest. OMP-07 is why the use case is a region a USER draws rather than a
    // HUD this code detects -- an image-based HUD detector was measured and rejected by the project
    // our composition descends from, and our position after UI compositing is strictly worse.
    if (ProbeControlMask()) {
        DestroyImage2D(ns.vk, ns.probeMask);
        if (CreateImage2D(ns.vk, VK_FORMAT_R8_UNORM, w, h, ns.probeMask)) {
            const int mode = ProbeControlMask();
            std::vector<uint8_t> board(size_t(w) * h);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    board[size_t(y) * w + x] =
                        mode == 2 ? 0 : mode == 3 ? 255
                                                  : (((x >> 6) ^ (y >> 6)) & 1 ? 255 : 0);
            if (UploadPixels(ns.vk, ns.probeMask, board.data(), board.size()))
                Log("[probe] DLSSNR_PROBE_CONTROLMASK=%d: binding %s to DLSSNR.ControlMask", mode,
                    mode == 2 ? "an all-zero mask" : mode == 3 ? "an all-255 mask"
                                                              : "a 64px checkerboard");
            else
                Log("[probe] control mask upload failed");
        } else {
            Log("[probe] control mask image could not be created");
        }
    }

    {
        float fill = 0.0f;
        if (const char* e = getenv("DLSSNR_DEPTH_FILL"); e && *e) {
            fill = strtof(e, nullptr);
            Log("[helper] DLSSNR_DEPTH_FILL=%s: filling the depth channel with %.3f instead of 0",
                e, double(fill));
        }
        std::vector<uint8_t> depthFill(size_t(w) * h * 4);
        for (size_t i = 0; i < depthFill.size(); i += 4) std::memcpy(&depthFill[i], &fill, 4);
        if (!UploadPixels(ns.vk, ns.depth, depthFill.data(), depthFill.size())) return false;
    }

    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    TransitionImage(ns.vk, ns.vk.cmdScratch, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (ns.depth.image) {
        TransitionImage(ns.vk, ns.vk.cmdScratch, ns.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    TransitionImage(ns.vk, ns.vk.cmdScratch, ns.workA, VK_IMAGE_LAYOUT_GENERAL, 0,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    TransitionImage(ns.vk, ns.vk.cmdScratch, ns.workB, VK_IMAGE_LAYOUT_GENERAL, 0,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!SubmitAndWait(ns.vk, ns.vk.cmdScratch)) return false;

    // Pass 0's feature is built with pass 0's tuning, here, because the model reads it at create.
    // Pass 0 is created inside NgxLoadAndInit, so its tuning has to be in the parameter block before
    // that call rather than recorded after it. Setting it afterwards is what made pass 0 always come
    // up with defaults while this side believed it had the user's values.
    NgxTuning first = TuningFor(shm.hdr, 0);

    if (!BeginCmd(ns.vk.cmdCreate)) return false;
    bool ok = NgxLoadAndInit(ns.ngx, ns.vk.instance, ns.vk.physical, ns.vk.device, w, h, ns.vk.cmdCreate, first);

    // The model states the driver it needs; compare against the driver we have, once. A floor
    // read out of the binary beats a hardcoded table and stays correct when the model is
    // swapped -- which is the whole problem with the published floors for this feature, which
    // disagree with each other by six major versions.
    //
    // A warning and not a refusal: this is the model's own opinion about a Windows driver
    // number, read on Linux through Wine, and our failure mode if it is right is already
    // fail-open. Being told why beats being stopped.
    static bool driverChecked = false;
    if (!driverChecked && g_modelMinDriver > 0.0 && ns.vk.driverMajor) {
        driverChecked = true;
        const double have = double(ns.vk.driverMajor) + double(ns.vk.driverMinor) / 100.0;
        if (have < g_modelMinDriver)
            Log("[helper] driver %u.%02u is BELOW the %.2f this model declares it needs; if the "
                "model refuses to initialise, that is the first thing to change",
                ns.vk.driverMajor, ns.vk.driverMinor, g_modelMinDriver);
        else
            Log("[helper] driver %u.%02u meets the model's declared minimum of %.2f",
                ns.vk.driverMajor, ns.vk.driverMinor, g_modelMinDriver);
    }
    if (!SubmitAndWait(ns.vk, ns.vk.cmdCreate) || !ok) {
        if (ns.hdrBuilt) {
            // The float contract was refused. That is the model saying no, not the model being
            // dead: drop back to 8-bit and let the next frame rebuild everything the SDR way.
            Log("[helper] model refused the float16 contract at %ux%u; falling back to 8-bit", w, h);
            ns.hdrRejected = true;
            ns.hdrBuilt = 0;
            ns.ngx.hdrActive = false;
            if (ns.vk.dmaBuf) {
                EnsureProxyOut(ns.vk, shm.hdr, w, h, VK_FORMAT_R8G8B8A8_UNORM);
                EnsureAnswerOut(ns.vk, shm.hdr, w, h, VK_FORMAT_R8G8B8A8_UNORM);
            }
            shm.hdr->proxyFormat.store(kProxyRgba8);
            ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                           "model refused float input; 8-bit proxy in use");
            return false;
        }
        Log("[helper] snippet init/create failed at %ux%u", w, h);
        ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                       "the model would not initialise; see the helper log");
        ns.ngx.disabled = true;
        PublishStatus(shm, ns, kHelperModelFailed);
        return false;
    }

    ns.tuning[0] = first;
    ns.lastSeenTuning[0] = first;
    ns.passNeedsReset[0] = true;
    ns.livePasses = 1;
    ns.w = w;
    ns.h = h;
    ns.ready = true;
    ns.tuningChangedMs = NowMs();
    ns.buildAfterMs = NowMs() + shm.hdr->rebuildSettleMs.load();

    // The motion field's units go with the field, so they are set as soon as there is a feature to
    // tell. The per-pass resource binding happens in BindPass; this is the part that does not change
    // between passes.
    ApplyMotionScale(ns.ngx, ns.mvecScaleMode, w, h);
    ns.appliedMvecScaleMode = ns.mvecScaleMode;
    ns.firstFrame = true;
    ns.prevLuma.clear();
    ns.lumaW = ns.lumaH = 0;
    Log("[helper] neural ready %ux%u", w, h);
    ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes, "");
    PublishStatus(shm, ns, kHelperRunning);
    return true;
}

// Bind one pass's input and output. The proxy the layer sent is read by pass 0 and never written by
// the chain, so what the composition later differences against is the whole chain's edit rather than
// the last pass's edit against the one before it.
static void BindPass(NeuralState& ns, uint32_t pass, GpuImage*& in, GpuImage*& out) {
    if (pass == 0) {
        in = &ns.colorIn;
        out = &ns.workA;
    } else if (pass % 2 == 1) {
        in = &ns.workA;
        out = &ns.workB;
    } else {
        in = &ns.workB;
        out = &ns.workA;
    }
}

static void FillResource(NVSDK_NGX_Resource_VK& r, GpuImage& img, bool rw) {
    r = {};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW;
    r.ReadWrite = rw;
    r.Resource.ImageViewInfo.ImageView = img.view;
    r.Resource.ImageViewInfo.Image = img.image;
    r.Resource.ImageViewInfo.SubresourceRange = { img.aspect(), 0, 1, 0, 1 };
    r.Resource.ImageViewInfo.Format = img.format;
    r.Resource.ImageViewInfo.Width = img.width;
    r.Resource.ImageViewInfo.Height = img.height;
}

// Bring the built features into line with what the header asks for.
//
// A retuned pass is replaced on its own -- retuning pass 2 no longer tears down passes 0 and 1 --
// and it keeps answering with the tuning it was built with until the replacement is ready, so a
// slider change is a swap inside one frame rather than a gap in the chain. Builds are spaced by the
// header's rebuildSettleMs rather than done back to back: NGX creation is expensive and back-to-back
// creation exhausts the driver's latches, after which the model stops answering until the process
// restarts. A spacing of 0 means no wait at all -- everything pending is built within this call.
static void MaintainPasses(NeuralState& ns, ShmMap& shm, uint32_t wanted) {
    if (!ns.ready || ns.ngx.disabled) return;

    const uint32_t spacing = shm.hdr->rebuildSettleMs.load();
    const double now = NowMs();

    // Has anything the model latches at creation changed?
    //
    // Compared by value rather than by watching tuningSeq. The sequence is a hint, not the truth: a
    // header reset returns it to zero while this process still remembers a larger number, and the
    // change that follows then looks like no change at all. Seven atomic loads per live pass per
    // frame is nothing next to a control that silently stops working. Every new value re-arms the
    // wait, so dragging a slider debounces rather than rebuilding at each tick.
    //
    // "What the model latches at creation" is now the PRESET and nothing else -- measured; see
    // NgxTuning::SameCreateParams for the numbers. The other six act at evaluate on the feature
    // that is already running, so rebuilding for them bought a device-wide stall and a fresh
    // temporal history in exchange for a value the running feature was going to read anyway.
    //
    // DLSSNR_REBUILD_ON_TUNING=1 restores the old behaviour for every field. It exists so that
    // "the picture smears for a moment when I drag a slider now" can be tested rather than argued
    // about, and it should be deleted once nobody has reported that for a release.
    static const bool rebuildOnEverything = [] {
        const char* p = getenv("DLSSNR_REBUILD_ON_TUNING");
        return p && p[0] == '1';
    }();
    auto scanDirty = [&]() -> int {
        int first = -1;
        for (uint32_t i = 0; i < ns.livePasses; ++i) {
            const NgxTuning t = TuningFor(shm.hdr, i);
            if (!(t == ns.lastSeenTuning[i])) { ns.lastSeenTuning[i] = t; ns.tuningChangedMs = now; }
            ns.passDirty[i] = rebuildOnEverything ? !(t == ns.tuning[i])
                                                  : !t.SameCreateParams(ns.tuning[i]);
            if (ns.passDirty[i] && first < 0) first = (int)i;
        }
        return first;
    };

    // With a spacing set, the buildAfterMs gate lets exactly one action through per call; with 0,
    // every pending action runs here in order.
    for (uint32_t guard = 0; guard < 2 * kMaxPasses; ++guard) {
        const int dirty = scanDirty();

        if (wanted < ns.livePasses) {
            vkDeviceWaitIdle(ns.vk.device);
            for (uint32_t i = wanted; i < ns.livePasses; ++i) {
                NgxReleasePass(ns.ngx, i, ns.vk.device);
                ns.passDirty[i] = false;
            }
            ns.livePasses = wanted;
            ns.buildAfterMs = now + spacing;
            continue;
        }

        if (now - ns.tuningChangedMs < (double)spacing || now < ns.buildAfterMs) break;

        if (dirty >= 0) {
            const uint32_t pass = (uint32_t)dirty;
            Log("[helper] pass %u retuned; rebuilding it (spacing %u ms)", pass, spacing);
            vkDeviceWaitIdle(ns.vk.device);
            NgxReleasePass(ns.ngx, pass, ns.vk.device);
            const NgxTuning t = TuningFor(shm.hdr, pass);
            NgxSetCreateTuning(ns.ngx, t);
            if (BeginCmd(ns.vk.cmdCreate)) {
                const bool built = NgxCreatePass(ns.ngx, pass, ns.w, ns.h, ns.vk.cmdCreate);
                SubmitAndWait(ns.vk, ns.vk.cmdCreate);
                if (built) {
                    ns.tuning[pass] = t;
                    ns.lastSeenTuning[pass] = t;
                    ns.passDirty[pass] = false;
                    ns.passNeedsReset[pass] = true;
                } else {
                    // The chain skips the hole and the next settle retries the build.
                    Log("[helper] pass %u rebuild failed; skipping it until it builds", pass);
                }
            }
            ns.buildAfterMs = now + spacing;
            continue;
        }

        // Frames where nothing is built fail open: the layer presents the game's own frame, which is
        // the right answer while the model has no feature to answer with.
        if (wanted > ns.livePasses) {
            const uint32_t pass = ns.livePasses;

            // Price the next pass before building it. A feature's history is driver-sized at
            // the model's working resolution and several at 4K are not free, and our ceiling
            // is thirty -- six times what the fork this idea came from allows. Refusing on a
            // measured price beats discovering the limit by hitting it, because hitting it
            // does not necessarily look like a failed allocation (see below).
            uint32_t usedBefore = 0, budgetBefore = 0;
            DeviceLocalMemoryMB(ns.vk, &usedBefore, &budgetBefore);
            if (ns.passPriceMB && budgetBefore && usedBefore + ns.passPriceMB > budgetBefore) {
                Log("[helper] not building pass %u: %u MiB used of %u MiB budget and the last "
                    "pass cost %u MiB. Holding the chain at %u.",
                    pass, usedBefore, budgetBefore, ns.passPriceMB, ns.livePasses);
                shm.hdr->helperPassCeiling.store(ns.livePasses);
                ns.buildAfterMs = now + spacing;
                break;
            }

            const NgxTuning t = TuningFor(shm.hdr, pass);
            NgxSetCreateTuning(ns.ngx, t);
            if (!BeginCmd(ns.vk.cmdCreate)) return;
            const bool built = NgxCreatePass(ns.ngx, pass, ns.w, ns.h, ns.vk.cmdCreate);
            ns.vk.lastQueueResult = VK_SUCCESS;
            const bool submitted = SubmitAndWait(ns.vk, ns.vk.cmdCreate);

            // A VRAM failure inside the snippet does not have to arrive as a null handle.
            // It has been reported arriving as VK_ERROR_DEVICE_LOST from the submit -- which
            // is also what every other catastrophe looks like, so without saying it here a
            // ramp that simply ran out of memory reads as a driver bug. The submit's result
            // was previously discarded entirely on this path.
            if (!submitted && ns.vk.lastQueueResult == VK_ERROR_DEVICE_LOST) {
                Log("[helper] device lost while building pass %u. On this path that is most "
                    "likely the model running out of video memory rather than a dead device: "
                    "%u MiB were in use of a %u MiB budget. Holding the chain at %u.",
                    pass, usedBefore, budgetBefore, ns.livePasses);
                shm.hdr->helperPassCeiling.store(ns.livePasses);
                ns.buildAfterMs = now + spacing;
                break;
            }

            if (built) {
                uint32_t usedAfter = 0;
                DeviceLocalMemoryMB(ns.vk, &usedAfter, nullptr);
                if (usedAfter > usedBefore) {
                    ns.passPriceMB = usedAfter - usedBefore;
                    Log("[helper] pass %u cost %u MiB (%u -> %u of %u MiB budget)", pass,
                        ns.passPriceMB, usedBefore, usedAfter, budgetBefore);
                }
                ns.tuning[pass] = t;
                ns.lastSeenTuning[pass] = t;
                ns.passNeedsReset[pass] = true;
                ns.livePasses = pass + 1;
            } else {
                // A later pass failing is a ceiling, not a fault: the chain simply runs at what fits.
                Log("[helper] pass %u would not build; holding the chain at %u", pass, ns.livePasses);
                shm.hdr->helperPassCeiling.store(ns.livePasses);
                ns.buildAfterMs = now + spacing;
                break;
            }
            ns.buildAfterMs = now + spacing;
            continue;
        }

        break;
    }
}

// The first field of /proc/self/stat is the kernel's pid for this process -- the number /proc is
// keyed by, which is what the layer needs to open an exported fd. Wine's Z: drive is the host's
// root, so the file is reachable through the ordinary file API.
static uint32_t ReadLinuxPid() {
    HANDLE h = CreateFileA("Z:\\proc\\self\\stat", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[256];
    DWORD got = 0;
    const bool ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0;
    CloseHandle(h);
    if (!ok) return 0;
    buf[got] = '\0';
    return (uint32_t)strtoul(buf, nullptr, 10);
}

static bool ProcessFrame(NeuralState& ns, ShmMap& shm) {
    g_crumbs.Drop("FrameEnter", shm.hdr->seq_req.load());
    // The outermost event; Evaluate and Readback nest inside it.
    dlssnr::TraceScope traceFrame(g_trace, "Frame", shm.hdr->seq_req.load());
    uint32_t w = shm.hdr->width.load(), h = shm.hdr->height.load();
    if (!w || !h || w > kMaxW || h > kMaxH) return false;
    // Echo the raster this call answers before seq_resp announces it, so a swapchain waiting on a
    // different request (another swapchain's, or its own before a resize) can refuse an answer that
    // was not made for it instead of copying the wrong number of bytes.
    shm.hdr->answeredW.store(w);
    shm.hdr->answeredH.store(h);

    const size_t px = size_t(w) * h;

    // The HDR decision, read from the same header statement that announced this frame's bytes.
    // hdrActive is the layer's intent (and the echo that lets this process build float surfaces at
    // all); hdrEncode is the width of the pixels already sitting in the region -- the two differ by
    // the frames it takes to switch, and every copy below sizes itself by hdrEncode, never by hope.
    const uint32_t hdrActive = shm.hdr->hdrActive.load() ? 1u : 0u;
    const uint32_t hdrEncode = shm.hdr->hdrEncode.load() ? 1u : 0u;
    // The same picture as last time: the layer's idle repaint re-asking about a frame it is
    // holding. A repeat is not a new sample of a moving scene, and treating it as one made
    // the answer for a held frame depend on how many times it had been re-asked.
    const bool frameRepeat = shm.hdr->frameRepeat.load() != 0;
    const bool wantHdr = hdrActive && !ns.hdrRejected &&
                         (!ns.ngx.snippet || ns.ngx.hdrCapable);
    const size_t bytes = px * (hdrEncode ? 8 : 4);

    // The switch. One frame's worth of refusal, and every surface -- crossing, chain and feature
    // contract -- moves together on the next. Refusing the frame that switches is what keeps a
    // half-moved pipeline from ever reading bytes as the wrong width: the layer presents its own
    // frame for that one, and the log says why.
    const bool hdrSwitching = wantHdr != (ns.hdrBuilt != 0);
    if (hdrSwitching) {
        if (vkDeviceWaitIdle) vkDeviceWaitIdle(ns.vk.device);
        NgxReleaseAllPasses(ns.ngx, ns.vk.device);
        ns.livePasses = 0;
        std::memset(ns.passDirty, 0, sizeof(ns.passDirty));
        DestroyImage2D(ns.vk, ns.colorIn);
        DestroyImage2D(ns.vk, ns.workA);
        DestroyImage2D(ns.vk, ns.workB);
        ns.hdrBuilt = wantHdr ? 1u : 0u;
        ns.ready = false;
        if (!wantHdr) ns.hdrRejected = false;  // re-arm the attempt for the next time it is asked
    }
    const VkFormat xferFmt = ns.hdrBuilt ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    // Phase 5: the proxy image is this process's own memory, exported so the layer can write it.
    // Publishing it is unconditional while the channel is up; reading it through the fd path is
    // not -- that needs the layer's flag, which says the layer has imported the descriptor and is
    // writing through it on exactly this frame.
    // Both images are published while the channel is up -- the answer as much as the proxy,
    // because the layer can only import what has been named, and importing is what makes it ask
    // for the fd path. The echo below is what turns the fd path on: the layer restates the
    // sequence it has taken a reference at, and reading or writing through the fd requires this
    // process's current sequence to be the one echoed back.
    if (ns.vk.dmaBuf) {
        EnsureProxyOut(ns.vk, shm.hdr, w, h, xferFmt);
        EnsureAnswerOut(ns.vk, shm.hdr, w, h, xferFmt);
    }
    ns.proxyActive = ns.vk.dmaBuf && ns.vk.proxyIn.image && ns.vk.proxyW == w && ns.vk.proxyH == h &&
                     ns.vk.proxySeq != 0 && shm.hdr->layerProxySeq.load() == ns.vk.proxySeq;

    // The crossing images now carry the new format and their export sequences have moved, so the
    // name published here is the name of what the layer will actually import. The frame that
    // switches is still refused: its bytes are the old width, and presenting the game's own frame
    // for one frame beats reading them wrong.
    if (hdrSwitching) {
        shm.hdr->proxyFormat.store(ns.hdrBuilt ? kProxyRgba16F : kProxyRgba8);
        Log("[helper] hdr %s at %ux%u (encoding %s); this frame passes through",
            ns.hdrBuilt ? "on: float16" : "off: 8-bit", w, h, hdrEncode ? "float16" : "8-bit");
        return false;
    }

    if (!ShmNeuralEnabled(shm.hdr)) {
        // The pass-through answer is a copy of the shared-memory proxy -- which the dma-buf path
        // leaves unwritten. Failing the frame gives the same result the bypass intends: the game's
        // own frame, presented as it is.
        if (ns.proxyActive) return false;
        std::memcpy(shm.outPixels, shm.inPixels, bytes);
        return true;
    }

    const uint32_t wanted = ShmPasses(shm.hdr);
    const bool time = TimeEnabled();
    const double t0 = NowMs();

    // What the header asks of the motion field, before the feature is built, because a change to the
    // quality has to be answered by rebuilding the flow session rather than by writing a parameter.
    const uint32_t prevMvecEnabled = ns.mvecEnabled;
    ns.mvecEnabled = ShmMVecEnabled(shm.hdr) ? 1u : 0u;
    ns.mvecScaleMode = ShmMVecScaleMode(shm.hdr);
    ns.mvecQuality = ShmMVecQuality(shm.hdr);
    ns.mvecPixelSize = ShmMVecPixelSize(shm.hdr);
    const bool mvecJustDisabled = prevMvecEnabled && !ns.mvecEnabled;
    const bool mvecJustEnabled = !prevMvecEnabled && ns.mvecEnabled;

    if (!EnsureNeural(ns, shm, w, h)) return false;
    MaintainPasses(ns, shm, wanted);
    if (!ns.ready || ns.livePasses == 0) return false;
    shm.hdr->proxyFormat.store(ns.hdrBuilt ? kProxyRgba16F : kProxyRgba8);

    // Every pass failed to build under the float contract: the model said no after all. Drop back;
    // the switch block rebuilds the whole chain 8-bit on the next frame.
    if (ns.hdrBuilt && wanted > 0) {
        bool anyBuilt = false;
        for (uint32_t i = 0; i < kMaxPasses; ++i) if (ns.ngx.features[i]) anyBuilt = true;
        if (!anyBuilt) {
            Log("[helper] no pass built under the float contract; falling back to 8-bit");
            ns.hdrRejected = true;
            return false;
        }
    }

    if (ns.appliedMvecScaleMode != ns.mvecScaleMode) {
        ApplyMotionScale(ns.ngx, ns.mvecScaleMode, w, h);
        ns.appliedMvecScaleMode = ns.mvecScaleMode;
    }
    if (!ns.mvecEnabled && (ns.flow.enabled || mvecJustDisabled)) {
        if (vkDeviceWaitIdle && vkDeviceWaitIdle(ns.vk.device) != VK_SUCCESS) {
            Log("[mvec] device wait failed during disable");
            return false;
        }
        DestroyOpticalFlow(ns.vk, ns.flow);
        ns.flow.userDisabled = true;
        if (!ClearMotionVectors(ns)) { Log("[frame] clear MVec failed"); return false; }
        ns.mvecResetPending = true;
        ns.lastResetLogged = 0xFFFFFFFFu;
        if (mvecJustDisabled) Log("[mvec] disabled by the header");
    }
    if (mvecJustEnabled) {
        if (ReactivateMotionVectors(ns, w, h, ns.mvecQuality) < 0) return false;
    } else if (ns.mvecEnabled && !ns.firstFrame && !ns.flow.enabled &&
               (ns.flow.userDisabled || ns.flow.attemptedQuality != ns.mvecQuality)) {
        ns.flow.userDisabled = false;
        if (!SetupOpticalFlow(ns.vk, ns, w, h, ns.mvecQuality, ns.mvecPixelSize)) {
            Log("[helper] estimated motion vectors unavailable");
        } else {
            ns.firstFrame = true;
            ns.mvecResetPending = true;
            ns.lastResetLogged = 0xFFFFFFFFu;
        }
    }
    if (ns.flow.enabled && (ns.flow.quality != ns.mvecQuality ||
                            ns.flow.attemptedPixelSize != ns.mvecPixelSize)) {
        const int rc = ReactivateMotionVectors(ns, w, h, ns.mvecQuality);
        if (rc < 0) return false;
        if (rc == 0) Log("[helper] estimated motion vectors disabled after a quality change");
    }

    // The proxy the layer encoded. It is already in the chain's own format -- 8-bit display-referred,
    // or float16 normalised linear light under the HDR path -- so there is nothing to swizzle and
    // nothing to convert.
    //
    // When the transport is imported, the proxy is already in the GPU's buffer -- the region this
    // process maps and the region the layer's GPU wrote are the same pages. Without the import the
    // frame is copied into staging first, as before.
    if (!ns.proxyActive && !ns.vk.transportIn) {
        if (bytes > ns.vk.stagingSize && !CreateStaging(ns.vk, bytes)) return false;
        std::memcpy(ns.vk.uploadMap, shm.inPixels, bytes);
    }

    // A cut is not motion. Carrying a flow field across one hands the model a field describing a
    // scene that is no longer on screen, which is worse than handing it nothing. The CPU detector
    // reads the shared-memory region, which the dma-buf path leaves unwritten, so it stands down
    // while the fd path is live -- one frame of stale flow across a cut is the price of the copy.
    bool duplicateFrame = false;
    const bool sceneCut = !ns.proxyActive && !hdrEncode &&
                          DetectSceneCut(ns, shm.hdr, shm.inPixels, w, h, 1, &duplicateFrame);
    if (sceneCut && !ns.firstFrame) {
        // The model's own temporal history is the point. Clearing the flow field and
        // leaving DLSSNR.Reset alone told the model to keep reprojecting across the cut
        // with a field we had just zeroed -- the exact case a reset exists for, and the
        // only one of the three pieces of history that was not being dropped.
        ns.mvecResetPending = true;
        if (ns.flow.enabled) {
            ns.flow.hasPrev = false;
            ns.flow.hintsStale = true;  // and the engine's own hints, which we cannot see
            ns.pendingMvClear = true;   // zeroed inside the flow prep submit, GPU-side
        }
    }

    if (frameRepeat && !ns.firstFrame) {
        // Reset every pass, so a held frame gets the same answer however many times it is
        // re-asked and a slider drag becomes a sequence of independent answers to one
        // picture rather than a walk. Without this, every measurement taken on a held frame
        // is unreproducible by construction, which is most of the measurements worth taking.
        ns.mvecResetPending = true;
        ns.flow.hintsStale = true;
    }

    const double tUpload = time ? NowMs() : 0.0;
    // Sampled unconditionally, unlike tUpload/tEval which exist only under DLSSNR_TIME: the
    // published upload and readback figures should not depend on a debugging variable being
    // set, which is exactly how helperEvalMsBits came to publish a constant zero.
    const double tUploadEnd = NowMs();
    // Estimating flow between a frame and itself costs a full NVOF execute to produce a
    // field we already know is zero, so a repeat skips it and takes one of the plain upload
    // paths below instead -- the upload normally rides along inside the flow prep submit.
    //
    // Two ways to be the same frame twice. frameRepeat is the layer saying so, which it can
    // only do for its own idle repaint. duplicateFrame is this side noticing: a paused game,
    // an occluded window or a menu presents the same picture through the ordinary path and
    // says nothing about it. Unlike a repaint, a duplicate does NOT reset the model -- the
    // history is about a scene that has not changed, which is exactly when it is worth most.
    const bool sameFrameAgain = frameRepeat || duplicateFrame;
    g_crumbs.Drop("Upload", uint32_t(bytes >> 10));
    if (ns.flow.enabled && !sameFrameAgain) {
        // The colorIn upload is merged into the flow prep command buffer.
        if (!RunOpticalFlow(ns)) {
            Log("[mvec] disabling estimated motion vectors after a flow failure");
            DestroyOpticalFlow(ns.vk, ns.flow);
            ns.flow.attemptedQuality = ns.mvecQuality;
            ns.flow.userDisabled = false;
            if (!ForceMvShaderRead(ns)) { Log("[frame] force MVec layout failed"); return false; }
            ns.mvecResetPending = true;
            ns.lastResetLogged = 0xFFFFFFFFu;
        }
    } else if (ns.proxyActive) {
        if (!BeginCmd(ns.vk.cmdScratch)) return false;
        RecordProxyToColorIn(ns, ns.vk.cmdScratch);
        if (!SubmitAndWait(ns.vk, ns.vk.cmdScratch)) return false;
    } else if (!UploadMappedPixels(ns.vk, ns.colorIn, bytes, UploadSource(ns.vk))) {
        return false;
    }

    const uint32_t passes = std::min(wanted, ns.livePasses);
    GpuImage* last = nullptr;
    double evalGpuMs = 0.0;   // summed over the chain; -1 readings are not counted
    uint32_t timedPasses = 0;

    for (uint32_t pass = 0; pass < passes && !SkipEvaluate(); ++pass) {
        // A failed rebuild leaves a hole; the chain runs without that pass rather than losing the
        // frame with it.
        if (!ns.ngx.features[pass]) continue;
        GpuImage *in = nullptr, *out = nullptr;
        BindPass(ns, pass, in, out);

        NVSDK_NGX_Resource_VK rc{}, ro{}, rm{}, rd{};
        FillResource(rc, *in, false);
        FillResource(ro, *out, true);
        FillResource(rm, ns.mv, false);
        FillResource(rd, ns.depth, false);
        // What the motion field actually holds this frame. Zero when it carries nothing --
        // vectors disabled, the session gone, a cut just cleared it, or the model has no
        // field to reproject with yet -- rather than a full rectangle of zeros presented as
        // measurements.
        const bool mvecUsable = ns.flow.enabled && ns.mvecEnabled && !ns.pendingMvClear;
        NVSDK_NGX_Resource_VK rmask{};
        const bool haveMask = ProbeControlMask() && ns.probeMask.image;
        if (haveMask) FillResource(rmask, ns.probeMask, false);
        NgxSetResources(ns.ngx, rc, ro, rm, rd, w, h,
                        mvecUsable ? ns.mv.width : 0u,
                        mvecUsable ? ns.mv.height : 0u,
                        haveMask ? &rmask : nullptr);

        // This pass's own tuning, written immediately before this pass's evaluate.
        //
        // The model reads intensity, local tone, local structure, skin structure, style and
        // the auto mask at EVERY evaluate -- only the preset is latched at create. Writing
        // them only at create therefore did not make them latched: it left every pass
        // evaluating with whatever the most recently built feature had put in the shared
        // parameter block, so in a chain with per-pass overrides each pass ran with some
        // other pass's values. Writing the resolved per-pass tuning here is what makes a
        // per-pass override mean anything.
        //
        // Sharpness is set alongside it out of habit and does nothing: this model has no
        // sharpness parameter. See NgxSetSharpness.
        const NgxTuning et = TuningFor(shm.hdr, pass);
        NgxSetEvaluateTuning(ns.ngx, et);
        // A strength that just moved invalidates the history built under the old one. Raised once,
        // on the first frame that carries the new value, not for as long as it differs from the
        // build -- which after the rebuild went away would have been forever.
        if (!ns.haveEvalTuning[pass] || !(et == ns.lastEvalTuning[pass])) {
            if (ns.haveEvalTuning[pass]) {
                ns.passNeedsReset[pass] = true;
                static uint32_t said = 0;
                if (said < 4) {
                    ++said;
                    Log("[params] pass %u retuned live; raising the model's history reset for one "
                        "frame (no rebuild)", pass);
                }
            }
            ns.lastEvalTuning[pass] = et;
            ns.haveEvalTuning[pass] = true;
        }
        const PassTuning ps = ShmResolvePass(shm.hdr, pass);
        NgxSetSharpness(ns.ngx, ClampF(ps.sharpness, 0.0f, 1.0f));

        if (!BeginCmd(ns.vk.cmdEval)) return false;
        ResetEvalTimestampQueries(ns.vk, ns.vk.cmdEval);
        TransitionImage(ns.vk, ns.vk.cmdEval, *in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        TransitionImage(ns.vk, ns.vk.cmdEval, *out, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // A pass owes the model a history reset when its feature was just built, and every pass owes
        // one when the motion field changed under it -- a model told to reproject with a field that
        // no longer describes the same thing smears until it is told to start over.
        const bool reset = ns.passNeedsReset[pass] || ns.mvecResetPending || ns.firstFrame;
        NgxSetReset(ns.ngx, reset, reset && ns.lastResetLogged != ns.mvecScaleMode);
        if (reset) ns.lastResetLogged = ns.mvecScaleMode;
        ns.passNeedsReset[pass] = false;

        WriteEvalTimestamp(ns.vk, ns.vk.cmdEval, 0);
        // The one call that has actually been seen to never come back. The watchdog prints this
        // ring when it fires, so the crumb before this one names the pass that wedged.
        g_crumbs.Drop("Evaluate", pass);
        // Either side of the snippet's own recording, so a device lost inside the model's dispatches
        // is distinguishable from one lost in our transitions around them. The marker is the same
        // literal the crumb used, so the two dumps read as one path.
        if (vkCmdSetCheckpointNV) vkCmdSetCheckpointNV(ns.vk.cmdEval, (const void*) "EvaluateEnter");
        if (!NgxEvaluatePass(ns.ngx, pass, ns.vk.cmdEval)) {
            vkEndCommandBuffer(ns.vk.cmdEval);
            return false;
        }
        if (vkCmdSetCheckpointNV) vkCmdSetCheckpointNV(ns.vk.cmdEval, (const void*) "EvaluateLeave");
        WriteEvalTimestamp(ns.vk, ns.vk.cmdEval, 1);
        CopyEvalTimestampResults(ns.vk, ns.vk.cmdEval);
        g_crumbs.Drop("EvalSubmit", pass);
        {
            dlssnr::TraceScope t(g_trace, "Evaluate", pass, w);
            if (!SubmitAndWait(ns.vk, ns.vk.cmdEval)) return false;
        }
        const double passGpuMs = ReadEvalTimestampMs(ns.vk);
        if (passGpuMs >= 0.0) { evalGpuMs += passGpuMs; ++timedPasses; }
        last = out;
    }
    if (SkipEvaluate()) {
        // The frame as it arrived is the frame that goes back. Everything either side of the
        // evaluate still runs: the import or upload above, the conversion and readback below, the
        // sequence handshake, the status. Only the model is absent.
        //
        // The GPU cost floor below does not fire on this, and should not: with no evaluate there
        // are no timestamp samples, and a check that reported "the model is not doing the work"
        // here would only be repeating the switch back.
        static bool said = false;
        if (!said) {
            said = true;
            Log("[helper] DLSSNR_SKIP_EVALUATE=1: answering with the frame that came in. The model "
                "is not running and nothing is being enhanced.");
        }
        last = &ns.colorIn;
    }
    ns.mvecResetPending = false;
    ns.firstFrame = false;
    const double tEval = time ? NowMs() : 0.0;
    const double tEvalEnd = NowMs();  // unconditional twin, for the published figures

    if (!last) return false;

    // Down to the transport's format, once, now that the passes are finished.
    //
    // A blit rather than a copy, because the chain is sixteen bits and the transport is eight and
    // vkCmdCopyImage cannot convert. Skipped entirely when the two already match -- one pass on the
    // HDR path, or any build where the work format equals the chain format -- so that case is
    // byte-identical to before and pays nothing.
    if (last->format != ns.colorOut.format) {
        if (!BeginCmd(ns.vk.cmdEval)) return false;
        VkAccessFlags srcA; VkPipelineStageFlags srcS;
        SrcAccessForLayout(last->layout, &srcA, &srcS);
        TransitionImage(ns.vk, ns.vk.cmdEval, *last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcA,
                        VK_ACCESS_TRANSFER_READ_BIT, srcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        TransitionImage(ns.vk, ns.vk.cmdEval, ns.colorOut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl{};
        bl.srcSubresource = { last->aspect(), 0, 0, 1 };
        bl.dstSubresource = { ns.colorOut.aspect(), 0, 0, 1 };
        bl.srcOffsets[1] = { int32_t(w), int32_t(h), 1 };
        bl.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
        vkCmdBlitImage(ns.vk.cmdEval, last->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ns.colorOut.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_NEAREST);
        if (!SubmitAndWait(ns.vk, ns.vk.cmdEval)) return false;
        last = &ns.colorOut;
    }

    // Phase 5: the answer goes back the way the proxy came -- a copy into exportable memory,
    // released to FOREIGN. The shared-memory write stops exactly when the layer's flag says it is
    // reading the fd, and the flag is honoured on the frame it was set for. If the exportable
    // image has gone away while the flag was up, the answer has no destination: fail the frame so
    // the layer presents the raw one, and the withdrawn export clears the flag on the next.
    // The two directions are independent: the answer can ride the fd path on a frame where the
    // proxy still rides shared memory, and the echo says so per direction.
    const bool wantFdAnswer = ns.vk.answerSeq != 0 &&
                              shm.hdr->layerAnswerSeq.load() == ns.vk.answerSeq;
    const bool fdAnswer = wantFdAnswer && EnsureAnswerOut(ns.vk, shm.hdr, w, h, xferFmt);
    if (wantFdAnswer && !fdAnswer) return false;
    if (fdAnswer) {
        if (!BeginCmd(ns.vk.cmdScratch)) return false;
        VkCommandBuffer cb = ns.vk.cmdScratch;
        VkAccessFlags srcA;
        VkPipelineStageFlags srcS;
        SrcAccessForLayout(last->layout, &srcA, &srcS);
        TransitionImage(ns.vk, cb, *last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcA,
                        VK_ACCESS_TRANSFER_READ_BIT, srcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // Acquire the answer surface from FOREIGN: the layer sampled it last frame and released it
        // back before presenting, so its caches are clean and the layout is whatever the layer left.
        VkImageMemoryBarrier aacq{};
        aacq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aacq.srcAccessMask = 0;
        aacq.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        aacq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aacq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        aacq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        aacq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aacq.image = ns.vk.answerOut.image;
        aacq.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &aacq);
        ns.vk.answerOut.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageCopy cp{};
        cp.srcSubresource = { last->aspect(), 0, 0, 1 };
        cp.dstSubresource = { ns.vk.answerOut.aspect(), 0, 0, 1 };
        cp.extent = { w, h, 1 };
        vkCmdCopyImage(cb, last->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ns.vk.answerOut.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        TransitionImage(ns.vk, cb, ns.vk.answerOut, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        VkImageMemoryBarrier rel{};
        rel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rel.dstAccessMask = 0;
        rel.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        rel.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        rel.image = ns.vk.answerOut.image;
        rel.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &rel);
        ns.vk.answerOut.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!SubmitAndWait(ns.vk, cb)) return false;
    } else {
        g_crumbs.Drop("Readback", uint32_t(bytes >> 10));
        dlssnr::TraceScope t(g_trace, "Readback", uint32_t(bytes >> 10));
        if (!ReadbackPixels(ns.vk, *last, bytes)) return false;
        if (!ns.vk.transportOut) std::memcpy(shm.outPixels, ns.vk.readMap, bytes);
    }
    g_crumbs.Drop("FrameDone");
    const double tDone = time ? NowMs() : 0.0;
    // Always sampled, not only under DLSSNR_TIME. These two fields have existed, been saved
    // and restored by ShmResetSettings, and been written by nothing at all -- and the way
    // helperEvalMsBits came to publish a constant zero was exactly this: a figure derived
    // from marks that only exist when a debugging variable is set.
    const double tDoneUncond = NowMs();
    const double uploadMs = tUploadEnd - t0;
    const double readbackMs = tDoneUncond - tEvalEnd;

    ++ns.evaluates;
    if (shm.hdr) {
        ShmStore64(shm.hdr->helperFramesLo, shm.hdr->helperFramesHi, ns.evaluates);
        // Sampled rather than per frame: this is a driver query, and VRAM does not move
        // between feature builds. Published here rather than from PublishStatus, which only
        // runs when the raster is set up -- so the figure there was whatever was in use
        // before the chain grew, which is the one moment it is guaranteed to be wrong.
        if (ns.evaluates % 30 == 1) {
            uint32_t used = 0;
            DeviceLocalMemoryMB(ns.vk, &used, nullptr);
            shm.hdr->helperVramMB.store(used);
        }
        // GPU time for the model where the device can measure it, CPU wall clock around
        // the submit-and-fence otherwise. The two are not the same number -- the CPU one
        // includes our own barriers and the queue wait -- so anything comparing this
        // against a threshold has to know which it got; the startup log says which.
        //
        // The fallback is worth naming: tUpload and tEval are only sampled under
        // DLSSNR_TIME, so before the GPU path existed this field published a constant
        // zero for everybody who had not set that variable.
        const double reportedEvalMs =
            timedPasses > 0 ? evalGpuMs : (time ? tEval - tUpload : 0.0);
        shm.hdr->helperEvalMsBits.store(FloatToBits(float(reportedEvalMs)));
        shm.hdr->helperUploadMsBits.store(FloatToBits(float(uploadMs)));
        shm.hdr->helperReadbackMsBits.store(FloatToBits(float(readbackMs)));

        // Did the model do any work?
        //
        // A pass that creates, evaluates, returns success and hands back its input unchanged
        // is invisible to every other counter here: features built, frames delivered, no
        // errors. What it cannot fake is GPU cost. The floor is per megapixel because the
        // pass scales with pixels, and it is derived from THIS machine rather than borrowed:
        // measured at 3.10 ms for one pass at 1080p, which is 1.50 ms/Mpx, so a floor at a
        // third of that is far below anything healthy and far above zero.
        //
        // A missing measurement is never a verdict: with no GPU timer there are no samples
        // and the check does not fire. And it only reports -- fail-open still applies, and a
        // model that has stopped working is not a reason to stop presenting frames.
        //
        // The constant is derived from this machine, not borrowed, and it is deliberately
        // well under the measured cost rather than near it:
        //
        //     1080p, 1 pass    3.10 ms   1.50 ms/Mpx
        //     1080p, 4 passes 12.59 ms   1.52 ms/Mpx
        //     4K,    3 passes 22.61 ms   0.91 ms/Mpx
        //
        // Cost per megapixel FALLS with raster -- the same sublinear shape the VRAM
        // measurement showed -- so the floor has to clear the cheapest case, not the average.
        // 0.30 leaves a factor of three at 4K and a factor of five at 1080p, which is far
        // below anything healthy and far above the zero a model that is not running costs.
        if (timedPasses > 0 && passes > 0) {
            const double mpx = double(w) * double(h) / 1000000.0;
            const double floorMs = 0.30 * mpx * double(passes);
            if (evalGpuMs < floorMs) {
                if (++ns.evalTooCheap == 60) {
                    Log("[helper] the model is costing %.2f ms of GPU for %u pass(es) at %ux%u, "
                        "below the %.2f ms floor. It is being evaluated and is not doing the "
                        "work; the picture is probably its input unchanged.",
                        evalGpuMs, passes, w, h, floorMs);
                    ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                                   "the model is running far below its expected GPU cost");
                }
            } else if (ns.evalTooCheap) {
                ns.evalTooCheap = 0;
            }
        }
        shm.hdr->helperFeatures.store(ns.livePasses);
        shm.hdr->modelUp.store(1);
    }

    if (time) {
        static int frameNo = 0;
        if (++frameNo % TimeInterval() == 0) {
            // Per round trip, which is this process's only unit -- the helper never sees a
            // present. Divide by the frame rate and the answer is wrong by however far the
            // layer's layerPresents has run ahead of its layerFrames.
            Log("[time] per round trip: passes=%u/%u flow=%s upload=%.2f eval=%.2f (gpu %.2f over %u) "
                "readback=%.2f total=%.2f ms",
                passes, wanted, ns.flow.enabled ? "on" : "off",
                tUpload - t0, tEval - tUpload, evalGpuMs, timedPasses, tDone - tEval, tDone - t0);
        }
    }
    return true;
}

// The hang watchdog. Guarded() catches a fault and returns; it cannot catch a call that
// never comes back, and a wedged NGX call looks from the outside exactly like a helper that
// is merely busy -- the layer times out, marks the channel dead, retries every five seconds,
// and the game runs on with nobody able to say why the model stopped.
//
// This thread only watches. It does not try to cancel anything: the stuck thread is inside
// the driver holding state we cannot take back, so there is no recovery to attempt, only a
// diagnosis to publish and a process to end. _exit rather than a clean return for the same
// reason the faulted teardown path exists -- do not ask code that has stopped answering to
// help you shut down.
static ShmMap* g_watchdogShm = nullptr;
static unsigned long long g_watchdogBudgetMs = 0;

static DWORD WINAPI NgxWatchdogThread(LPVOID) {
    for (;;) {
        Sleep(250);
        ShmMap* shm = g_watchdogShm;
        if (shm && shm->hdr && shm->hdr->quit.load()) return 0;
        const unsigned long long ms = NgxCallOutstandingMs();
        if (ms < g_watchdogBudgetMs) continue;
        Log("[watchdog] NGX %s has not returned after %llu ms; the model has stopped "
            "answering. Ending the helper -- restart it to try again.",
            NgxCallName(), ms);
        // Printed before anything else, because _exit follows and this is the only record of what
        // the frame had already done when the call stopped coming back.
        DumpCrumbs("the model stopped answering");
        // Written before _exit, not after: a hung NGX call is exactly the session a trace is worth
        // having, and this path never unwinds.
        if (const char* path = g_trace.Write()) Log("[trace] wrote %s", path);
        if (shm && shm->hdr) {
            ShmStoreString(shm->hdr->helperReasonSeq, shm->hdr->helperReason, kReasonBytes,
                           "the model stopped responding; restart the helper");
            shm->hdr->helperState.store(kHelperModelFailed);
            shm->hdr->modelUp.store(0);
            shm->hdr->seq_ok.store(0);
            // Answer the outstanding request, so the layer stops waiting on a frame that is
            // never coming rather than spending its whole budget on it.
            std::atomic_thread_fence(std::memory_order_release);
            shm->hdr->seq_resp.store(shm->hdr->seq_req.load());
        }
        _exit(4);
    }
}

int main() {
    // Wine delivers OutputDebugString through a debug-print exception. Install the
    // filter before the first log call so that exception can never reach an older or
    // runner-installed handler while the helper is starting.
    InstallGuard();
    Log("=== dlssnr_helper starting ===");
    // The trace directory arrives as a Linux path and this process is a PE under Wine, so it is
    // translated the same way the mapping's is: the host filesystem is Wine's Z: drive.
    //
    // A char buffer rather than std::string: appending a char to one instantiates
    // basic_string::_M_replace_cold here, and this target links libstdc++ statically with
    // -nostdlib++, where that arrives as a duplicate definition of a different size. Same reason
    // the watchdog is a Win32 thread.
    {
        const char* dir = getenv("DLSSNR_TRACE_DIR");
        if (!dir || !*dir) dir = getenv("XDG_STATE_HOME");
        char win[512] = {0};
        if (dir && dir[0] == '/') {
            win[0] = 'Z'; win[1] = ':';
            size_t n = 2;
            for (const char* c = dir; *c && n + 1 < sizeof(win); ++c, ++n)
                win[n] = (*c == '/') ? '\\' : *c;
            win[n] = '\0';
        }
        g_trace.Init("helper", win[0] ? win : nullptr);
    }
    if (g_trace.Enabled())
        Log("=== DLSSNR_TRACE=1: recording a frame trace; it is written on quit and by the "
            "watchdog ===");
    if (SkipEvaluate())
        Log("=== kill switch active: DLSSNR_SKIP_EVALUATE -- the model will not run ===");
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string dir(exePath);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) {
            dir.resize(slash);
            SetCurrentDirectoryA(dir.c_str());
        }
    }
    g_layerModule = GetModuleHandleW(nullptr);

    ShmMap shm{};
    if (!ShmOpen(shm)) return 2;

    if (const char* v = getenv("DLSSNR_Passes"); v && *v) {
        uint32_t p = uint32_t(atoi(v));
        if (p >= 1 && p <= kMaxPasses) {
            shm.hdr->passes.store(p);
            shm.hdr->controlSeq.fetch_add(1);
        }
    } else if (const char* v = getenv("DLSSNR_PASSES"); v && *v) {
        uint32_t p = uint32_t(atoi(v));
        if (p >= 1 && p <= kMaxPasses) {
            shm.hdr->passes.store(p);
            shm.hdr->controlSeq.fetch_add(1);
        }
    }

    NeuralState ns{};
    if (!CreateContext(ns.vk)) {
        shm.hdr->helperState.store(kHelperNoVulkan);
        ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                       ns.vk.pinMissed
                           ? "DLSSNR_GPU_UUID/DLSSNR_GPU_INDEX named a device that is not present"
                           : "no NVIDIA device with the NVX extensions");
        return 3;
    }
    // Best-effort: if the view landed aligned and the driver accepts the import, the frame carries
    // no host copies. Otherwise the staging paths are used and everything still works.
    ImportTransport(ns.vk, shm.inPixels, shm.outPixels, kMaxFrame);

    // Phase 5: the dma-buf exchange, if the driver can carry it. The layer takes a reference on
    // exported memory by duplicating descriptors out of this process, so it needs the real Linux pid --
    // GetCurrentProcessId answers with the Win32 one, which means nothing to procfs. The kernel's
    // own record of the process says otherwise, and Wine can read it like any file.
    {
        const char* env = getenv("DLSSNR_DMABUF");
        const bool want = !(env && !_stricmp(env, "0"));
        if (want && vkGetMemoryFdKHR &&
            HasDeviceExt(ns.vk.physical, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
            HasDeviceExt(ns.vk.physical, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
            ns.vk.linuxPid = ReadLinuxPid();
            if (ns.vk.linuxPid) {
                ns.vk.dmaBuf = true;
                Log("[fd] dma-buf exchange ready (linux pid %u)", ns.vk.linuxPid);
            } else {
                Log("[fd] could not read the linux pid; shared memory transport stays");
            }
        }
    }
    // The hang watchdog. Guarded() catches a fault and returns; it cannot catch a call that
    // never comes back, and a wedged NGX call looks from the outside exactly like a helper
    // that is merely busy -- the layer times out, marks the channel dead, retries every five
    // seconds, and the game runs on with nobody able to say why the model stopped.
    //
    // This thread only watches. It does not try to cancel anything: the stuck thread is
    // inside the driver holding state we cannot take back, so there is no recovery to
    // attempt, only a diagnosis to publish and a process to end. _exit rather than a clean
    // return for the same reason the faulted teardown path exists -- do not ask code that
    // has stopped answering to help you shut down.
    {
        const char* env = getenv("DLSSNR_NGX_WATCHDOG_MS");
        g_watchdogBudgetMs = env ? strtoull(env, nullptr, 10) : 30000ull;
        g_watchdogShm = &shm;
        if (g_watchdogBudgetMs) {
            // A Win32 thread rather than std::thread: this target links libstdc++
            // statically with -nostdlib++, and pulling in std::thread's machinery made the
            // linker report a duplicate section of a different size. The helper is a PE and
            // already speaks Win32 everywhere else.
            HANDLE h = CreateThread(nullptr, 0, NgxWatchdogThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            else Log("[watchdog] could not start; NGX hangs will not be detected");
        } else {
            Log("[watchdog] disabled by DLSSNR_NGX_WATCHDOG_MS=0");
        }
    }

    SeedSceneCutFromEnv(shm.hdr);
    shm.hdr->helperState.store(kHelperRunning);
    Log("[helper] context ready, waiting for frames");

    uint32_t lastReq = shm.hdr->seq_resp.load();
    double lastFrameMs = 0.0;  // wall clock of the last frame processed, for the gap rule
    while (!shm.hdr->quit.load()) {
        // Restated every pass, not announced once.
        //
        // Four processes can re-initialise this header -- the layer, the interface, the control tool
        // and this one -- and a freshly initialised header says there is no helper. A helper that
        // announced itself only when it attached would be erased by any of them and never correct
        // the record, after which every game passes its frames through while this process sits here
        // waiting for frames that are no longer being sent. One atomic store per pass ends that.
        shm.hdr->helperState.store(ns.ngx.disabled ? kHelperModelFailed : kHelperRunning);

        uint32_t req = shm.hdr->seq_req.load();

        // The counter only ever climbs, so a smaller value than last time means the header was
        // re-initialised under us. Resynchronise rather than treat the difference as a new frame.
        if (req < lastReq) {
            Log("[helper] shared memory was re-initialised; resynchronising at %u", req);
            lastReq = req;
            shm.hdr->seq_resp.store(req);
            continue;
        }
        // A hole in the sequence is a discontinuity, and until now only the counter going
        // BACKWARDS had a branch. The layer skips the round trip on a timeout, stands down
        // for five seconds after four consecutive ones, latches inert on device loss, and
        // is simply off whenever the user toggles the pass -- so on the far side of any of
        // those the model would resume with history from before the interruption. A
        // present-time pass has no reset signal from the game; its own continuity is the
        // only one it can observe, so a gap has to be the reset condition.
        if (req > lastReq + 1) {
            Log("[helper] %u frames were skipped (%u -> %u); resetting the model's history",
                req - lastReq - 1, lastReq, req);
            ns.mvecResetPending = true;
            ns.flow.hasPrev = false;
            ns.flow.hintsStale = true;
        } else if (req == lastReq + 1 && lastFrameMs > 0.0 && NowMs() - lastFrameMs > 250.0) {
            // And a gap in TIME with no gap in the count: an alt-tab, a stall or a paused
            // player holds seq_req still rather than skipping it, and the next frame is
            // just as unrelated to the last one as a skipped run would have been.
            Log("[helper] %.0f ms since the last frame; resetting the model's history",
                NowMs() - lastFrameMs);
            ns.mvecResetPending = true;
            ns.flow.hasPrev = false;
            ns.flow.hintsStale = true;
        }

        if (req == lastReq) {
            for (int i = 0; i < 20000; ++i) {
                if (shm.hdr->quit.load() || shm.hdr->seq_req.load() != lastReq) break;
                CpuYield();
            }
            if (!shm.hdr->quit.load() && shm.hdr->seq_req.load() == lastReq) {
                shm.hdr->heartbeat.fetch_add(1);
                Sleep(1);
            }
            continue;
        }
        // The acquire pairs with the layer's release before seq_req: the proxy it wrote (by GPU into the
        // imported region, or by memcpy) is visible here before we read it.
        std::atomic_thread_fence(std::memory_order_acquire);
        bool ok = ProcessFrame(ns, shm);
        if (ns.vk.fenceTimedOut) {
            ShmStoreString(shm.hdr->helperReasonSeq, shm.hdr->helperReason, kReasonBytes,
                           "the GPU stopped retiring the helper's work; restart the helper");
            shm.hdr->helperState.store(kHelperModelFailed);
            shm.hdr->seq_ok.store(0);
            std::atomic_thread_fence(std::memory_order_release);
            shm.hdr->seq_resp.store(req);
            break;
        }
        if (!ok) Log("[helper] frame %u failed (w=%u h=%u)", req, shm.hdr->width.load(), shm.hdr->height.load());
        shm.hdr->seq_ok.store(ok ? req : 0);
        // The release pairs with the layer's acquire on seq_resp: the answer -- written by the GPU
        // into the imported region or by the staging memcpy -- is visible before the number that
        // announces it.
        std::atomic_thread_fence(std::memory_order_release);
        shm.hdr->seq_resp.store(req);
        lastReq = req;
        lastFrameMs = NowMs();
        if (ns.ngx.disabled) {
            shm.hdr->quit.store(1);
            break;
        }
    }

    if (shm.hdr->quit.load()) Log("[helper] quit requested");
    else if (ns.ngx.disabled) Log("[helper] neural disabled");
    shm.hdr->helperState.store(kHelperStopped);
    Log("[helper] shutting down");
    // Before the teardown, not after: NgxTeardown has been observed to wedge, and a trace written
    // on the far side of a call that may never return is a trace that is sometimes not written.
    if (const char* path = g_trace.Write()) Log("[trace] wrote %s", path);
    if (ns.ngx.snippet) NgxTeardown(ns.ngx, ns.vk.device);
    vkDeviceWaitIdle(ns.vk.device);
    DestroyImage2D(ns.vk, ns.vk.proxyIn);
    DestroyImage2D(ns.vk, ns.vk.answerOut);
    if (ns.vk.proxyExportFd >= 0) close(ns.vk.proxyExportFd);
    if (ns.vk.answerExportFd >= 0) close(ns.vk.answerExportFd);
    DestroyOpticalFlow(ns.vk, ns.flow);
    if (ns.vk.mvPipeLayout && vkDestroyPipelineLayout) vkDestroyPipelineLayout(ns.vk.device, ns.vk.mvPipeLayout, nullptr);
    if (ns.vk.mvDescLayout && vkDestroyDescriptorSetLayout) vkDestroyDescriptorSetLayout(ns.vk.device, ns.vk.mvDescLayout, nullptr);
    if (ns.vk.mvShaderFixed5 && vkDestroyShaderModule) vkDestroyShaderModule(ns.vk.device, ns.vk.mvShaderFixed5, nullptr);
    if (ns.vk.mvShaderFloat && vkDestroyShaderModule) vkDestroyShaderModule(ns.vk.device, ns.vk.mvShaderFloat, nullptr);
    if (ns.vk.semPrep && vkDestroySemaphore) vkDestroySemaphore(ns.vk.device, ns.vk.semPrep, nullptr);
    if (ns.vk.semFlow && vkDestroySemaphore) vkDestroySemaphore(ns.vk.device, ns.vk.semFlow, nullptr);
    if (ns.vk.cmdPoolFlow) vkDestroyCommandPool(ns.vk.device, ns.vk.cmdPoolFlow, nullptr);
    if (ns.vk.flowQuery) vkDestroyQueryPool(ns.vk.device, ns.vk.flowQuery, nullptr);
    if (ns.vk.queryStaging) vkDestroyBuffer(ns.vk.device, ns.vk.queryStaging, nullptr);
    if (ns.vk.queryMem) vkFreeMemory(ns.vk.device, ns.vk.queryMem, nullptr);
    if (ns.vk.debugMessenger) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)g_gipa(ns.vk.instance,
                                                                   "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(ns.vk.instance, ns.vk.debugMessenger, nullptr);
    }
    DestroyImage2D(ns.vk, ns.vk.dummyCost);
    DestroyImage2D(ns.vk, ns.vk.dummyBack);
    if (ns.vk.evalQuery) vkDestroyQueryPool(ns.vk.device, ns.vk.evalQuery, nullptr);
    if (ns.vk.evalStaging) vkDestroyBuffer(ns.vk.device, ns.vk.evalStaging, nullptr);
    if (ns.vk.evalMem) vkFreeMemory(ns.vk.device, ns.vk.evalMem, nullptr);
    if (ns.vk.uploadStaging) vkDestroyBuffer(ns.vk.device, ns.vk.uploadStaging, nullptr);
    if (ns.vk.readStaging) vkDestroyBuffer(ns.vk.device, ns.vk.readStaging, nullptr);
    if (ns.vk.uploadMem) vkFreeMemory(ns.vk.device, ns.vk.uploadMem, nullptr);
    if (ns.vk.readMem) vkFreeMemory(ns.vk.device, ns.vk.readMem, nullptr);
    if (vkDestroyFence) {
        for (uint32_t i = 0; i < VkCtx::kFenceRing; ++i)
            if (ns.vk.fences[i]) vkDestroyFence(ns.vk.device, ns.vk.fences[i], nullptr);
    }
    if (ns.vk.device) vkDestroyDevice(ns.vk.device, nullptr);
    if (ns.vk.instance) vkDestroyInstance(ns.vk.instance, nullptr);
    return 0;
}
