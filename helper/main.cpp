// dlssnr_helper.exe — Windows-side DLSSNR (Feature 18) service.
// Owns its own Vulkan device + the nvngx_dlssnr.dll snippet (verified
// standalone_runner sequence), waits on the shared-memory frame queue written
// by VK_LAYER_NV_dlssnr, runs the neural pass, and returns processed frames.
#include "ngx_snippet.h"
#include "guard.h"
#include "logging.h"
#include "../common/shm_protocol.h"
#include "mvec_deadzone_spv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
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
    s.base = MapViewOfFile(s.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
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
VK_FN(vkCmdCopyImage) VK_FN(vkCmdPipelineBarrier) VK_FN(vkDeviceWaitIdle)
VK_FN(vkGetPhysicalDeviceProperties2) VK_FN(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
VK_FN(vkCreateOpticalFlowSessionNV) VK_FN(vkDestroyOpticalFlowSessionNV)
VK_FN(vkBindOpticalFlowSessionImageNV) VK_FN(vkCmdOpticalFlowExecuteNV) VK_FN(vkCmdBlitImage)
VK_FN(vkCmdPipelineBarrier2) VK_FN(vkCmdWriteTimestamp2)
VK_FN(vkCreateQueryPool) VK_FN(vkDestroyQueryPool) VK_FN(vkCmdResetQueryPool)
VK_FN(vkCmdWriteTimestamp) VK_FN(vkCmdCopyQueryPoolResults)
VK_FN(vkCreateSemaphore) VK_FN(vkDestroySemaphore) VK_FN(vkCmdClearColorImage)
VK_FN(vkCreateShaderModule) VK_FN(vkDestroyShaderModule)
VK_FN(vkCreatePipelineLayout) VK_FN(vkDestroyPipelineLayout)
VK_FN(vkCreateComputePipelines) VK_FN(vkDestroyPipeline)
VK_FN(vkCmdBindPipeline) VK_FN(vkCmdDispatch) VK_FN(vkCmdBindDescriptorSets)
VK_FN(vkCreateDescriptorSetLayout) VK_FN(vkDestroyDescriptorSetLayout)
VK_FN(vkCreateDescriptorPool) VK_FN(vkDestroyDescriptorPool)
VK_FN(vkAllocateDescriptorSets) VK_FN(vkUpdateDescriptorSets)
#undef VK_FN



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
    VkQueryPool flowQuery = nullptr;
    VkBuffer queryStaging = nullptr;
    VkDeviceMemory queryMem = nullptr;
    void* queryMap = nullptr;
    bool flowQueryAvailable = false;
    float timestampPeriod = 1.0f;
    uint32_t flowTimestampBits = 0;
    // Persistent MVec deadzone compute objects (pipeline itself is per-size).
    bool mvComputeSupported = false;
    VkShaderModule mvShaderModule = nullptr;
    VkDescriptorSetLayout mvDescLayout = nullptr;
    VkPipelineLayout mvPipeLayout = nullptr;
};

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

static uint32_t FindMemoryType(VkCtx& c, uint32_t bits, VkMemoryPropertyFlags want);
static uint32_t FindHostMemoryType(VkCtx& c, uint32_t bits, bool preferCached);
static bool CreateStaging(VkCtx& c, size_t bytes);
static uint16_t FloatToHalf(float f);
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
    LOAD(vkGetPhysicalDeviceProperties2) LOAD(vkGetPhysicalDeviceOpticalFlowImageFormatsNV)
    LOAD(vkCreateOpticalFlowSessionNV) LOAD(vkDestroyOpticalFlowSessionNV)
    LOAD(vkBindOpticalFlowSessionImageNV) LOAD(vkCmdOpticalFlowExecuteNV) LOAD(vkCmdBlitImage)
    LOAD(vkCmdPipelineBarrier2) LOAD(vkCmdWriteTimestamp2)
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
#undef LOAD

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(c.instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> phys(devCount);
    vkEnumeratePhysicalDevices(c.instance, &devCount, phys.data());
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.vendorID != 0x10DE) continue;
        if (!HasDeviceExt(p, "VK_NVX_binary_import") || !HasDeviceExt(p, "VK_NVX_image_view_handle")) continue;
        c.physical = p;
        Log("[helper] device: %s", props.deviceName);
        break;
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
                           "VK_KHR_synchronization2", VK_NV_OPTICAL_FLOW_EXTENSION_NAME })
        if (HasDeviceExt(c.physical, e)) enabled.push_back(e);
    c.sync2 = HasDeviceExt(c.physical, "VK_KHR_synchronization2");
    VkPhysicalDeviceOpticalFlowFeaturesNV flowFeatures{};
    flowFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
    flowFeatures.opticalFlow = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = c.opticalFlow ? &flowFeatures : nullptr;
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
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
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
        vkAllocateDescriptorSets && vkUpdateDescriptorSets && kMVecDeadzoneSpvLen) {
        VkFormatProperties u16{};
        VkFormatProperties f16{};
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_UINT, &u16);
        vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_SFLOAT, &f16);
        c.mvComputeSupported =
            (u16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
            (f16.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    }
    Log("[helper] mvec gpu compute supported=%d", int(c.mvComputeSupported));

    if (c.opticalFlow && c.flowTimestampBits && vkCreateQueryPool && vkCmdResetQueryPool &&
        vkCmdCopyQueryPoolResults && (vkCmdWriteTimestamp2 || vkCmdWriteTimestamp)) {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        if (vkCreateQueryPool(c.device, &qpi, nullptr, &c.flowQuery) == VK_SUCCESS) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = 16;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(c.device, &bci, nullptr, &c.queryStaging) == VK_SUCCESS) {
                VkMemoryRequirements req{};
                vkGetBufferMemoryRequirements(c.device, c.queryStaging, &req);
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
                if (mai.memoryTypeIndex != UINT32_MAX &&
                    vkAllocateMemory(c.device, &mai, nullptr, &c.queryMem) == VK_SUCCESS &&
                    vkBindBufferMemory(c.device, c.queryStaging, c.queryMem, 0) == VK_SUCCESS &&
                    vkMapMemory(c.device, c.queryMem, 0, VK_WHOLE_SIZE, 0, &c.queryMap) == VK_SUCCESS) {
                    c.flowQueryAvailable = true;
                } else {
                    if (c.queryMem) vkFreeMemory(c.device, c.queryMem, nullptr);
                    if (c.queryStaging) vkDestroyBuffer(c.device, c.queryStaging, nullptr);
                    if (c.flowQuery) vkDestroyQueryPool(c.device, c.flowQuery, nullptr);
                    c.queryMem = nullptr; c.queryStaging = nullptr; c.flowQuery = nullptr; c.queryMap = nullptr;
                }
            } else if (c.flowQuery) {
                vkDestroyQueryPool(c.device, c.flowQuery, nullptr);
                c.flowQuery = nullptr;
            }
        }
    }
    Log("[helper] flow timestamp query=%d bits=%u period=%.3f",
        int(c.flowQueryAvailable), c.flowTimestampBits, c.timestampPeriod);

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
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = FindHostMemoryType(c, req.memoryTypeBits, true);
        if (mai.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(c.device, &mai, nullptr, &mem) != VK_SUCCESS ||
            vkBindBufferMemory(c.device, buf, mem, 0) != VK_SUCCESS) return false;
        return vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
    };

    if (!make(c.uploadStaging, c.uploadMem, &c.uploadMap) ||
        !make(c.readStaging, c.readMem, &c.readMap)) {
        destroy();
        return false;
    }
    c.stagingSize = bytes;
    return true;
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
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    return vkCreateImageView(c.device, &vi, nullptr, &out.view) == VK_SUCCESS;
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
    // STORAGE: the MVec deadzone compute pass reads the raw flow texels through
    // a size-compatible R16G16_UINT view (no image->image copy out of the
    // optical-flow output, which the driver cannot sample/copy as bits).
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    out.format = fmt; out.width = w; out.height = h; out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(c.device, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(c.device, out.image, out.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { out.aspect(), 0, 1, 0, 1 };
    return vkCreateImageView(c.device, &vi, nullptr, &out.view) == VK_SUCCESS;
}

static void DestroyImage2D(VkCtx& c, GpuImage& img) {
    if (img.view) vkDestroyImageView(c.device, img.view, nullptr);
    if (img.image) vkDestroyImage(c.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(c.device, img.memory, nullptr);
    img = {};
}

static size_t ImageSizeBytes(VkCtx& c, GpuImage& img) {
    if (vkGetImageSubresourceLayout) {
        VkSubresourceLayout sl{};
        VkImageSubresource sub{};
        sub.aspectMask = img.aspect();
        vkGetImageSubresourceLayout(c.device, img.image, &sub, &sl);
        if (sl.size) return sl.size;
    }
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
                       const VkPipelineStageFlags* waitStages, VkSemaphore signal) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = waitStages;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (signal) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signal; }
    const uint32_t idx = c.fenceCursor;
    const VkResult sr = vkQueueSubmit(queue, 1, &si, c.fences[idx]);
    if (sr != VK_SUCCESS) { Log("[vk] queue submit failed: %d", (int)sr); return -1; }
    c.fenceCursor = (c.fenceCursor + 1) % VkCtx::kFenceRing;
    return (int)idx;
}

static bool WaitFence(VkCtx& c, int idx) {
    if (idx < 0) return false;
    if (vkWaitForFences(c.device, 1, &c.fences[idx], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    vkResetFences(c.device, 1, &c.fences[idx]);
    return true;
}

static bool SubmitAndWaitQueue(VkCtx& c, VkCommandBuffer cb, VkQueue queue) {
    const int idx = SubmitAsync(c, cb, queue, 0, nullptr, nullptr, nullptr);
    return idx >= 0 && WaitFence(c, idx);
}

static bool SubmitAndWait(VkCtx& c, VkCommandBuffer cb) {
    return SubmitAndWaitQueue(c, cb, c.queue);
}

static bool UploadMappedPixels(VkCtx& c, GpuImage& img, size_t bytes) {
    if (!c.uploadMap || bytes > c.stagingSize) return false;
    if (!BeginCmd(c.cmdScratch)) return false;
    TransitionImage(c, c.cmdScratch, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { img.aspect(), 0, 0, 1 };
    region.imageExtent = { img.width, img.height, 1 };
    vkCmdCopyBufferToImage(c.cmdScratch, c.uploadStaging, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return SubmitAndWait(c, c.cmdScratch);
}

static bool ReadbackPixels(VkCtx& c, GpuImage& img, size_t bytes) {
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
    bool userDisabled = false;
    bool hasPrev = false;
    bool gpuBlit = false;
    bool gpuBlitLinear = false;
    bool cpuOnly = false;
    bool hybrid = false;
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
    GpuImage prev{}, curr{}, out{}, flowFloat{};
};

static float ClampF(float v, float lo, float hi) {
    if (!(v >= lo)) return lo;
    if (v > hi) return hi;
    return v;
}

struct NeuralState {
    VkCtx vk{};
    NgxSnippet ngx{};

    // The proxy the layer sent, and two surfaces the chain alternates between. Two, not one, because
    // a pass must read the previous pass's answer while writing its own: with a single surface the
    // model would be reading and writing the same image.
    GpuImage colorIn{}, workA{}, workB{}, mv{}, depth{};

    uint32_t w = 0, h = 0;
    bool ready = false;

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
    uint32_t appliedMvecScaleMode = 0xFFFFFFFFu;
    std::vector<uint8_t> prevLuma;
    uint32_t lumaW = 0, lumaH = 0;
    uint32_t sceneCutStreak = 0;
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
                                                   uint32_t w, uint32_t h, uint32_t) {
    const VkOpticalFlowGridSizeFlagsNV sizes[] = {
        VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_8X8_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_2X2_BIT_NV,
        VK_OPTICAL_FLOW_GRID_SIZE_1X1_BIT_NV,
    };
    for (uint32_t i = 0; i < 4; ++i) {
        VkOpticalFlowGridSizeFlagsNV bit = sizes[i];
        uint32_t g = FlowGridBitsToFactor(bit);
        if ((supported & bit) && g && (w % g) == 0 && (h % g) == 0) return bit;
    }
    return VK_OPTICAL_FLOW_GRID_SIZE_UNKNOWN_NV;
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
    std::vector<VkOpticalFlowImageFormatPropertiesNV> props(count);
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

static bool EnsureMVecComputeObjects(VkCtx& c) {
    if (c.mvPipeLayout) return true;
    if (!c.mvComputeSupported) return false;
    VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = kMVecDeadzoneSpvLen * sizeof(uint32_t);
    smci.pCode = kMVecDeadzoneSpv;
    if (vkCreateShaderModule(c.device, &smci, nullptr, &c.mvShaderModule) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 2; dli.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(c.device, &dli, nullptr, &c.mvDescLayout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &c.mvDescLayout;
    if (vkCreatePipelineLayout(c.device, &pli, nullptr, &c.mvPipeLayout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(c.device, c.mvDescLayout, nullptr);
        c.mvDescLayout = nullptr;
        vkDestroyShaderModule(c.device, c.mvShaderModule, nullptr);
        c.mvShaderModule = nullptr;
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
    f.gpuCompute = false;
}

// Builds the per-size compute pipeline + descriptor set. The compute pass reads
// the raw NVOF texels through a size-compatible R16G16_UINT view of the session
// output image and writes filtered vectors straight into the MVec resource.
static bool BuildMVecComputePass(NeuralState& ns, uint32_t ow, uint32_t oh) {
    VkCtx& c = ns.vk;
    OpticalFlowState& f = ns.flow;
    DestroyMVecComputePass(c, f);
    if (!c.mvComputeSupported || !ns.mv.image || !f.out.image || !EnsureMVecComputeObjects(c)) return false;
    if (f.flowFormat != VK_FORMAT_R16G16_SFIXED5_NV) return false;  // shader decodes SFIXED5 bits only
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
    struct SpecData {
        uint32_t grid, srcW, srcH, dstW, dstH, fixed5;
        float deadzone;
        uint32_t bilinear;
    } data{};
    data.grid = f.grid ? f.grid : 1u;
    data.srcW = ow; data.srcH = oh;
    data.dstW = ns.mv.width; data.dstH = ns.mv.height;
    data.fixed5 = f.flowFormat == VK_FORMAT_R16G16_SFIXED5_NV ? 1u : 0u;
    data.deadzone = MVecDeadzone();
    data.bilinear = 1u;
    VkSpecializationMapEntry entries[8] = {
        {0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4},
        {4, 16, 4}, {5, 20, 4}, {6, 24, 4}, {7, 28, 4},
    };
    VkSpecializationInfo sp{};
    sp.mapEntryCount = 8; sp.pMapEntries = entries;
    sp.dataSize = sizeof(data); sp.pData = &data;
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = c.mvShaderModule;
    cpi.stage.pName = "main";
    cpi.stage.pSpecializationInfo = &sp;
    cpi.layout = c.mvPipeLayout;
    if (vkCreateComputePipelines(c.device, nullptr, 1, &cpi, nullptr, &f.mvPipeline) != VK_SUCCESS) {
        Log("[mvec] deadzone compute pipeline creation failed");
        DestroyMVecComputePass(c, f);
        return false;
    }
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(c.device, &dpci, nullptr, &f.mvPool) != VK_SUCCESS) {
        DestroyMVecComputePass(c, f);
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = f.mvPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &c.mvDescLayout;
    if (vkAllocateDescriptorSets(c.device, &dsai, &f.mvSet) != VK_SUCCESS) {
        DestroyMVecComputePass(c, f);
        return false;
    }
    VkDescriptorImageInfo srcInfo{ nullptr, f.outBitsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo dstInfo{ nullptr, ns.mv.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = f.mvSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = i == 0 ? &srcInfo : &dstInfo;
    }
    vkUpdateDescriptorSets(c.device, 2, writes, 0, nullptr);
    f.gpuCompute = true;
    Log("[mvec] GPU deadzone pass ready grid=%u flow=%ux%u mvec=%ux%u fixed5=%u deadzone=%.3f",
        data.grid, ow, oh, data.dstW, data.dstH, data.fixed5, data.deadzone);
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
    DestroyImage2D(c, f.flowFloat);
    f.enabled = false;
    f.inputFormat = f.flowFormat = VK_FORMAT_UNDEFINED;
    f.grid = 1;
    f.quality = kMVecBalanced;
    f.attemptedQuality = 0;
    f.userDisabled = false;
    f.hasPrev = false;
    f.gpuBlit = false;
    f.gpuBlitLinear = false;
    f.cpuOnly = false;
    f.hybrid = false;
    f.currentToPrevious = true;
    f.flowTransferSrc = false;
    f.gpuConvertChecked = false;
    f.loggedFirstFlow = false;
}

static bool SetupOpticalFlow(VkCtx& c, NeuralState& ns, uint32_t w, uint32_t h, uint32_t quality) {
    OpticalFlowState& f = ns.flow;
    DestroyOpticalFlow(c, f);
    f.attemptedQuality = quality;
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
        if (w < props.minWidth || h < props.minHeight || w > props.maxWidth || h > props.maxHeight) {
            Log("[mvec] size %ux%u outside NVOF limits %ux%u..%ux%u",
                w, h, props.minWidth, props.minHeight, props.maxWidth, props.maxHeight);
            return false;
        }
    }

    VkOpticalFlowGridSizeFlagsNV gridBit = ChooseFlowGrid(supported, w, h, quality);
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

    f.gpuBlit = false;
    f.gpuBlitLinear = false;
    f.flowTransferSrc = false;
    if (vkGetPhysicalDeviceFormatProperties) {
        VkFormatProperties src{};
        vkGetPhysicalDeviceFormatProperties(c.physical, f.flowFormat, &src);
        f.flowTransferSrc = (src.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
        if (vkCmdBlitImage && f.flowFormat != VK_FORMAT_R16G16_SFLOAT) {
            VkFormatProperties dst{};
            vkGetPhysicalDeviceFormatProperties(c.physical, VK_FORMAT_R16G16_SFLOAT, &dst);
            const bool srcBlit = (src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
            const bool dstBlit = (dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
            f.gpuBlit = srcBlit && dstBlit;
            f.gpuBlitLinear = (src.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        }
    }
    const char* gpuEnv = getenv("DLSSNR_MVEC_GPU");
    f.cpuOnly = gpuEnv && gpuEnv[0] == '0';
    if (f.cpuOnly) {
        f.gpuBlit = false;
        f.gpuBlitLinear = false;
    }
    const char* dirEnv = getenv("DLSSNR_MVEC_DIRECTION");
    f.currentToPrevious = !(dirEnv && dirEnv[0] == '0');
    const char* filterEnv = getenv("DLSSNR_MVEC_FILTER");
    if (filterEnv && filterEnv[0] == '1') f.gpuBlitLinear = true;

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

    VkOpticalFlowSessionCreateInfoNV sci{};
    sci.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV;
    sci.width = w;
    sci.height = h;
    sci.imageFormat = f.inputFormat;
    sci.flowVectorFormat = f.flowFormat;
    sci.costFormat = VK_FORMAT_UNDEFINED;
    sci.outputGridSize = gridBit;
    sci.hintGridSize = VK_OPTICAL_FLOW_GRID_SIZE_UNKNOWN_NV;
    sci.performanceLevel = quality == kMVecFast ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV
        : quality == kMVecQuality ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV
                                          : VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV;
    sci.flags = 0;
    if (vkCreateOpticalFlowSessionNV(c.device, &sci, nullptr, &f.session) != VK_SUCCESS) {
        Log("[mvec] vkCreateOpticalFlowSessionNV failed");
        DestroyOpticalFlow(c, f);
        return false;
    }

    GpuImage& refImg = f.currentToPrevious ? f.prev : f.curr;
    GpuImage& inImg = f.currentToPrevious ? f.curr : f.prev;
    VkResult bindRef = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, refImg.view, VK_IMAGE_LAYOUT_GENERAL);
    VkResult bindIn = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, inImg.view, VK_IMAGE_LAYOUT_GENERAL);
    VkResult bindOut = vkBindOpticalFlowSessionImageNV(c.device, f.session,
        VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, f.out.view, VK_IMAGE_LAYOUT_GENERAL);
    if (bindRef != VK_SUCCESS || bindIn != VK_SUCCESS || bindOut != VK_SUCCESS) {
        Log("[mvec] failed to bind NVOF session images ref=%d input=%d out=%d",
            (int)bindRef, (int)bindIn, (int)bindOut);
        DestroyOpticalFlow(c, f);
        return false;
    }

    if (!f.cpuOnly) {
        const char* compEnv = getenv("DLSSNR_MVEC_COMPUTE");
        if (compEnv && compEnv[0] == '0') {
            Log("[mvec] GPU deadzone pass disabled by env");
        } else if (!BuildMVecComputePass(ns, ow, oh)) {
            Log("[mvec] GPU deadzone pass unavailable, using legacy flow conversion");
        }
    }

    f.enabled = true;
    const bool gpuConvert = (f.flowFormat == VK_FORMAT_R16G16_SFLOAT && !f.cpuOnly) || f.gpuBlit;
    Log("[mvec] NV optical flow enabled size=%ux%u grid=%u quality=%u input=%d flow=%d gpu_convert=%d linear=%d dir=%d xfer=%d",
        w, h, f.grid, quality, (int)f.inputFormat, (int)f.flowFormat, int(gpuConvert), int(f.gpuBlitLinear),
        int(f.currentToPrevious), int(f.flowTransferSrc));
    Log("[mvec] session grid=%u perf=%u cost=off hints=off flags=%u",
        f.grid, (unsigned)sci.performanceLevel, (unsigned)sci.flags);
    return true;
}

static bool ReadbackFlowToStaging(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    const size_t flowBytes = ImageSizeBytes(ns.vk, f.out);
    const size_t mvBytes = ImageSizeBytes(ns.vk, ns.mv);
    const size_t need = flowBytes > mvBytes ? flowBytes : mvBytes;
    if (need > ns.vk.stagingSize && !CreateStaging(ns.vk, need)) {
        Log("[mvec] failed to allocate flow readback staging");
        return false;
    }
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    TransitionImage2(ns.vk, ns.vk.cmdScratch, f.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     0, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = { f.out.aspect(), 0, 0, 1 };
    region.imageExtent = { f.out.width, f.out.height, 1 };
    vkCmdCopyImageToBuffer(ns.vk.cmdScratch, f.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           ns.vk.readStaging, 1, &region);
    return SubmitAndWait(ns.vk, ns.vk.cmdScratch);
}

static bool FinishFlowConversionCPU(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    const size_t px = size_t(f.out.width) * f.out.height;
    const size_t mvBytes = ImageSizeBytes(ns.vk, ns.mv);
    const uint16_t* src = (const uint16_t*)ns.vk.readMap;
    uint16_t* dst = (uint16_t*)ns.vk.uploadMap;
    auto decode = [&](uint32_t x, uint32_t y, uint32_t comp) -> float {
        size_t idx = (size_t(y) * f.out.width + x) * 2 + comp;
        return float(int16_t(src[idx])) / 32.0f;
    };
    if (f.out.width == ns.mv.width && f.out.height == ns.mv.height) {
        for (size_t i = 0; i < px * 2; ++i) dst[i] = FloatToHalf(float(int16_t(src[i])) / 32.0f);
    } else {
        for (uint32_t y = 0; y < ns.mv.height; ++y) {
            float fy = (float(y) + 0.5f) * float(f.out.height) / float(ns.mv.height) - 0.5f;
            if (fy < 0) fy = 0;
            if (fy > float(f.out.height - 1)) fy = float(f.out.height - 1);
            uint32_t y0 = uint32_t(fy);
            uint32_t y1 = y0 + 1 < f.out.height ? y0 + 1 : y0;
            float wy = fy - float(y0);
            for (uint32_t x = 0; x < ns.mv.width; ++x) {
                float fx = (float(x) + 0.5f) * float(f.out.width) / float(ns.mv.width) - 0.5f;
                if (fx < 0) fx = 0;
                if (fx > float(f.out.width - 1)) fx = float(f.out.width - 1);
                uint32_t x0 = uint32_t(fx);
                uint32_t x1 = x0 + 1 < f.out.width ? x0 + 1 : x0;
                float wx = fx - float(x0);
                uint16_t* out = dst + (size_t(y) * ns.mv.width + x) * 2;
                for (uint32_t c = 0; c < 2; ++c) {
                    float v00 = decode(x0, y0, c), v10 = decode(x1, y0, c);
                    float v01 = decode(x0, y1, c), v11 = decode(x1, y1, c);
                    float vx0 = v00 + (v10 - v00) * wx;
                    float vx1 = v01 + (v11 - v01) * wx;
                    out[c] = FloatToHalf(vx0 + (vx1 - vx0) * wy);
                }
            }
        }
    }
    if (!UploadMappedPixels(ns.vk, ns.mv, mvBytes)) {
        Log("[mvec] MVec upload failed");
        return false;
    }
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    TransitionImage(ns.vk, ns.vk.cmdScratch, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (!SubmitAndWait(ns.vk, ns.vk.cmdScratch)) {
        Log("[mvec] MVec layout transition failed");
        return false;
    }
    return true;
}

static bool DebugMVecEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_MVEC_DEBUG");
        return p && p[0] == '1';
    }();
    return v;
}

static void LogFlowStats(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    if (!f.enabled || !f.flowTransferSrc || !ns.vk.readMap || !ns.vk.readStaging) return;
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

static bool FinishFlowConversionHybrid(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    if (!f.flowFloat.image) {
        if (!CreateImage2D(ns.vk, VK_FORMAT_R16G16_SFLOAT, f.out.width, f.out.height, f.flowFloat)) {
            Log("[mvec] failed to create low-res float flow image");
            return false;
        }
    }
    const size_t px = size_t(f.out.width) * f.out.height;
    const size_t flowBytes = ImageSizeBytes(ns.vk, f.out);
    const uint16_t* src = (const uint16_t*)ns.vk.readMap;
    uint16_t* dst = (uint16_t*)ns.vk.uploadMap;
    if (f.flowFormat == VK_FORMAT_R16G16_SFIXED5_NV) {
        for (size_t i = 0; i < px * 2; ++i) dst[i] = FloatToHalf(float(int16_t(src[i])) / 32.0f);
    } else if (f.flowFormat == VK_FORMAT_R16G16_SFLOAT) {
        std::memcpy(dst, src, flowBytes);
    } else {
        return false;
    }
    if (!UploadMappedPixels(ns.vk, f.flowFloat, flowBytes)) {
        Log("[mvec] low-res float flow upload failed");
        return false;
    }
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    TransitionImage(ns.vk, cb, f.flowFloat, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    const bool sameSize = f.flowFloat.width == ns.mv.width && f.flowFloat.height == ns.mv.height;
    if (sameSize) {
        VkImageCopy copy{};
        copy.srcSubresource = { f.flowFloat.aspect(), 0, 0, 1 };
        copy.dstSubresource = { ns.mv.aspect(), 0, 0, 1 };
        copy.extent = { f.flowFloat.width, f.flowFloat.height, 1 };
        vkCmdCopyImage(cb, f.flowFloat.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        if (!vkCmdBlitImage) {
            vkEndCommandBuffer(cb);
            Log("[mvec] vkCmdBlitImage missing for float flow upscale");
            return false;
        }
        VkImageBlit blit{};
        blit.srcSubresource = { f.flowFloat.aspect(), 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)f.flowFloat.width, (int32_t)f.flowFloat.height, 1 };
        blit.dstSubresource = { ns.mv.aspect(), 0, 0, 1 };
        blit.dstOffsets[1] = { (int32_t)ns.mv.width, (int32_t)ns.mv.height, 1 };
        vkCmdBlitImage(cb, f.flowFloat.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       f.gpuBlitLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return SubmitAndWait(ns.vk, cb);
}

static bool ConvertFlowToMVecHybrid(NeuralState& ns) {
    if (!ReadbackFlowToStaging(ns)) return false;
    return FinishFlowConversionHybrid(ns);
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
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, c.flowQuery, 0);
    }
}

static void WriteFlowTimestampEnd(VkCtx& c, VkCommandBuffer cb) {
    if (!c.flowQueryAvailable) return;
    if (vkCmdWriteTimestamp) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, c.flowQuery, 1);
    } else if (c.sync2 && vkCmdWriteTimestamp2) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, c.flowQuery, 1);
    }
}

static void CopyFlowTimestampResults(VkCtx& c, VkCommandBuffer cb) {
    if (!c.flowQueryAvailable) return;
    vkCmdCopyQueryPoolResults(cb, c.flowQuery, 0, 2, c.queryStaging, 0, 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}

static double ReadFlowTimestampMs(VkCtx& c) {
    if (!c.flowQueryAvailable || !c.queryMap) return -1.0;
    const uint64_t* q = (const uint64_t*)c.queryMap;
    if (q[0] == 0 || q[1] == 0 || q[1] < q[0]) return -1.0;
    return double(q[1] - q[0]) * double(c.timestampPeriod) / 1000000.0;
}

static bool ForceMvShaderRead(NeuralState& ns);

static bool RunOpticalFlow(NeuralState& ns) {
    OpticalFlowState& f = ns.flow;
    if (!f.enabled) return true;

    const bool async = ns.vk.semPrep && ns.vk.semFlow;
    const bool gpuCompute = f.gpuCompute && f.outBitsView && f.mvPipeline && async;

    // ---- cmdPrep (graphics): upload colorIn, feed NVOF inputs ----
    // The CPU swizzle already filled uploadMap; this submit also carries the
    // scene-cut MVec clear so no extra host round-trip is needed.
    if (!BeginCmd(ns.vk.cmdScratch)) return false;
    VkCommandBuffer cb = ns.vk.cmdScratch;
    TransitionImage(ns.vk, cb, ns.colorIn, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy upRegion{};
    upRegion.imageSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
    upRegion.imageExtent = { ns.colorIn.width, ns.colorIn.height, 1 };
    vkCmdCopyBufferToImage(cb, ns.vk.uploadStaging, ns.colorIn.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upRegion);
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
        VkImageCopy seed{};
        seed.srcSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
        seed.dstSubresource = { f.prev.aspect(), 0, 0, 1 };
        seed.extent = { ns.colorIn.width, ns.colorIn.height, 1 };
        vkCmdCopyImage(cb, ns.colorIn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       f.prev.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &seed);
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
        VkImageCopy copy{};
        copy.srcSubresource = { ns.colorIn.aspect(), 0, 0, 1 };
        copy.dstSubresource = { f.curr.aspect(), 0, 0, 1 };
        copy.extent = { ns.colorIn.width, ns.colorIn.height, 1 };
        vkCmdCopyImage(cb, ns.colorIn.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       f.curr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    ResetFlowTimestampQueries(ns.vk, cb);

    {
        VkAccessFlags prevSrcA; VkPipelineStageFlags prevSrcS;
        SrcAccessForLayout(f.prev.layout, &prevSrcA, &prevSrcS);
        TransitionImage2(ns.vk, cb, f.prev, VK_IMAGE_LAYOUT_GENERAL,
                         prevSrcA, VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV,
                         prevSrcS, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV);
    }
    TransitionImage2(ns.vk, cb, f.curr, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV);
    TransitionImage2(ns.vk, cb, f.out, VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV);

    WriteFlowTimestampBegin(ns.vk, cb);
    const int prepFence = SubmitAsync(ns.vk, cb, ns.vk.queue, 0, nullptr, nullptr,
                                      async ? ns.vk.semPrep : nullptr);
    if (prepFence < 0) return false;
    if (!async && !WaitFence(ns.vk, prepFence)) return false;

    // ---- NVOF execute on the dedicated optical-flow queue (no CPU wait) ----
    if (!BeginCmd(ns.vk.cmdFlow)) { WaitFence(ns.vk, prepFence); return false; }
    VkCommandBuffer fcb = ns.vk.cmdFlow;
    VkOpticalFlowExecuteInfoNV exec{};
    exec.sType = VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV;
    vkCmdOpticalFlowExecuteNV(fcb, f.session, &exec);
    // TOP_OF_PIPE: the wait must cover every command in this buffer (the NVOF
    // session reads its inputs through driver-private stages). ALL_COMMANDS is
    // invalid in pWaitDstStageMask on the optical-flow-only queue (VUID-00066)
    // and the driver silently drops it; TOP_OF_PIPE releases only after the
    // signaling submit's full first sync scope, ordering everything.
    const VkPipelineStageFlags flowWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const int flowFence = SubmitAsync(ns.vk, fcb, ns.vk.opticalQueue,
                                      async ? 1u : 0u, async ? &ns.vk.semPrep : nullptr,
                                      async ? &flowWaitStage : nullptr,
                                      async ? ns.vk.semFlow : nullptr);
    if (flowFence < 0) { WaitFence(ns.vk, prepFence); return false; }
    if (!async && !WaitFence(ns.vk, flowFence)) { WaitFence(ns.vk, prepFence); return false; }

    // ---- cmdPost (graphics): harvest flow + GPU deadzone pass ----
    const bool sameSize = f.out.width == ns.mv.width && f.out.height == ns.mv.height;
    const bool canCopy = sameSize && f.flowFormat == VK_FORMAT_R16G16_SFLOAT;
    const bool canBlit = vkCmdBlitImage && f.gpuBlit;
    const bool directConvert = !gpuCompute && !f.cpuOnly && !f.hybrid && (canCopy || canBlit);
    if (!gpuCompute && !directConvert) {
        const size_t flowBytes = ImageSizeBytes(ns.vk, f.out);
        const size_t mvBytes = ImageSizeBytes(ns.vk, ns.mv);
        const size_t need = flowBytes > mvBytes ? flowBytes : mvBytes;
        if (need > ns.vk.stagingSize && !CreateStaging(ns.vk, need)) {
            WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence);
            Log("[mvec] failed to allocate flow readback staging");
            return false;
        }
    }
    if (!BeginCmd(ns.vk.cmdFlowPost)) {
        WaitFence(ns.vk, prepFence); WaitFence(ns.vk, flowFence);
        return false;
    }
    cb = ns.vk.cmdFlowPost;
    WriteFlowTimestampEnd(ns.vk, cb);
    CopyFlowTimestampResults(ns.vk, cb);

    if (gpuCompute) {
        // The compute pass reads the raw NVOF texels in-place through the
        // R16G16_UINT view, decodes, deadzone-clamps and upscales straight into
        // the MVec resource. No host readback, no image->image bit copy.
        TransitionImage2(ns.vk, cb, f.out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        TransitionImage2(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f.mvPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ns.vk.mvPipeLayout,
                                0, 1, &f.mvSet, 0, nullptr);
        vkCmdDispatch(cb, (ns.mv.width + 7) / 8, (ns.mv.height + 7) / 8, 1);
        TransitionImage2(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_2_SHADER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    } else {
        TransitionImage2(ns.vk, cb, f.out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        if (directConvert) {
            VkAccessFlags mvSrcA; VkPipelineStageFlags mvSrcS;
            SrcAccessForLayout(ns.mv.layout, &mvSrcA, &mvSrcS);
            TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mvSrcA,
                            VK_ACCESS_TRANSFER_WRITE_BIT, mvSrcS, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (canCopy) {
                VkImageCopy copy{};
                copy.srcSubresource = { f.out.aspect(), 0, 0, 1 };
                copy.dstSubresource = { ns.mv.aspect(), 0, 0, 1 };
                copy.extent = { f.out.width, f.out.height, 1 };
                vkCmdCopyImage(cb, f.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else {
                VkImageBlit blit{};
                blit.srcSubresource = { f.out.aspect(), 0, 0, 1 };
                blit.srcOffsets[1] = { (int32_t)f.out.width, (int32_t)f.out.height, 1 };
                blit.dstSubresource = { ns.mv.aspect(), 0, 0, 1 };
                blit.dstOffsets[1] = { (int32_t)ns.mv.width, (int32_t)ns.mv.height, 1 };
                const VkFilter filter = sameSize ? VK_FILTER_NEAREST
                    : (f.gpuBlitLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
                vkCmdBlitImage(cb, f.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ns.mv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
            }
            TransitionImage(ns.vk, cb, ns.mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        } else {
            VkBufferImageCopy region{};
            region.imageSubresource = { f.out.aspect(), 0, 0, 1 };
            region.imageExtent = { f.out.width, f.out.height, 1 };
            vkCmdCopyImageToBuffer(cb, f.out.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   ns.vk.readStaging, 1, &region);
        }
    }

    TransitionImage2(ns.vk, cb, f.curr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    TransitionImage2(ns.vk, cb, f.prev, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    {
        VkImageCopy copy{};
        copy.srcSubresource = { f.curr.aspect(), 0, 0, 1 };
        copy.dstSubresource = { f.prev.aspect(), 0, 0, 1 };
        copy.extent = { f.curr.width, f.curr.height, 1 };
        vkCmdCopyImage(cb, f.curr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       f.prev.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    const VkPipelineStageFlags postWaitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const int postFence = SubmitAsync(ns.vk, cb, ns.vk.queue,
                                      async ? 1u : 0u, async ? &ns.vk.semFlow : nullptr,
                                      async ? &postWaitStage : nullptr, nullptr);
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

    if (gpuCompute) {
        if (!f.gpuConvertChecked) {
            f.gpuConvertChecked = true;
            if (DebugMVecEnabled() && !MVecFiniteCheck(ns)) {
                Log("[mvec] GPU deadzone pass produced invalid MVec, disabling compute path");
                f.gpuCompute = false;
                if (!ForceMvShaderRead(ns)) return false;
                ns.mvecResetPending = true;
            }
        }
    } else if (directConvert) {
        if (!f.gpuConvertChecked) {
            f.gpuConvertChecked = true;
            if (!MVecFiniteCheck(ns)) {
                Log("[mvec] GPU flow conversion produced invalid MVec, trying hybrid CPU+GPU conversion");
                if (!ConvertFlowToMVecHybrid(ns)) {
                    Log("[mvec] hybrid flow conversion failed, falling back to CPU");
                    f.hybrid = false;
                    f.cpuOnly = true;
                    f.gpuBlit = false;
                    if (!ReadbackFlowToStaging(ns) || !FinishFlowConversionCPU(ns)) {
                        Log("[mvec] CPU flow conversion failed");
                        return false;
                    }
                } else {
                    f.hybrid = true;
                }
            }
        }
    } else if (f.hybrid) {
        if (!FinishFlowConversionHybrid(ns)) {
            Log("[mvec] hybrid flow conversion failed, falling back to CPU");
            f.hybrid = false;
            f.cpuOnly = true;
            if (!ReadbackFlowToStaging(ns) || !FinishFlowConversionCPU(ns)) {
                Log("[mvec] CPU flow conversion failed");
                return false;
            }
        }
    } else {
        if (!FinishFlowConversionCPU(ns)) {
            Log("[mvec] CPU flow conversion failed");
            return false;
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
    ns.lastResetLogged = 0xFFFFFFFFu;
    if (!SetupOpticalFlow(ns.vk, ns, w, h, quality)) {
        ns.flow.attemptedQuality = quality;
        ns.flow.userDisabled = false;
        Log("[mvec] reactivation setup failed, using zero MVec");
        return 0;
    }
    Log("[mvec] reactivated quality=%u grid=%u", quality, ns.flow.grid);
    return 1;
}

static bool DetectSceneCut(NeuralState& ns, const uint8_t* in, uint32_t w, uint32_t h, uint32_t fmt) {
    static const bool enabled = [] {
        const char* p = getenv("DLSSNR_SCENE_CUT");
        return !p || p[0] != '0';
    }();
    static const int threshold = [] {
        const char* p = getenv("DLSSNR_SCENE_CUT_THRESHOLD");
        int v = p && *p ? atoi(p) : 55;
        return v > 0 ? v : 55;
    }();
    if (!enabled || !in || !w || !h) return false;

    const uint32_t gw = w < 64 ? w : 64;
    const uint32_t gh = h < 36 ? h : 36;
    const size_t n = size_t(gw) * gh;
    bool sizeChanged = ns.prevLuma.size() != n || ns.lumaW != gw || ns.lumaH != gh;
    if (sizeChanged) {
        ns.prevLuma.assign(n, 0);
        ns.lumaW = gw; ns.lumaH = gh;
        ns.sceneCutStreak = 0;
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
    if (mean >= threshold) ++ns.sceneCutStreak;
    else ns.sceneCutStreak = 0;
    bool cut = ns.sceneCutStreak >= 2;
    if (cut) Log("[mvec] scene cut detected mean=%d threshold=%d streak=%u", mean, threshold, ns.sceneCutStreak);
    return cut;
}

static NgxTuning TuningFor(const ShmHeader* h, uint32_t pass) {
    const PassTuning p = ShmResolvePass(h, pass);
    NgxTuning t;
    t.intensity = ClampF(p.intensity, 0.0f, 4.0f);
    t.localTone = ClampF(p.localTone, 0.0f, 4.0f);
    t.localStructure = ClampF(p.localStructure, 0.0f, 4.0f);
    t.skinStructure = ClampF(p.skinStructure, -1.0f, 4.0f);
    t.style = p.style;
    t.preset = p.preset;
    t.autoMask = p.autoMask ? 1u : 0u;
    return t;
}

static void PublishStatus(ShmMap& shm, NeuralState& ns, uint32_t state) {
    if (!shm.hdr) return;
    shm.hdr->helperState.store(state);
    shm.hdr->modelUp.store(ns.ngx.ready && !ns.ngx.disabled ? 1u : 0u);
    shm.hdr->helperFeatures.store(ns.livePasses);
    ShmStore64(shm.hdr->helperFramesLo, shm.hdr->helperFramesHi, ns.evaluates);
}

static uint16_t FloatToHalf(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) return uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0u));
    if (exp > 142u) return uint16_t(sign | 0x7C00u);
    if (exp < 103u) return uint16_t(sign);
    exp -= 112u;
    uint32_t h = sign | (exp << 10) | (mant >> 13);
    if ((mant >> 12) & 1u) ++h;
    return uint16_t(h);
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
static void ApplyMotionScale(NgxSnippet& ngx, uint32_t mode, uint32_t w, uint32_t h) {
    float sx = 2.0f / float(w), sy = 2.0f / float(h);
    if (mode == kMVecPixels) { sx = 1.0f; sy = 1.0f; }
    else if (mode == kMVecUv01) { sx = 1.0f / float(w); sy = 1.0f / float(h); }
    NgxSetMotionScale(ngx, sx, sy);
}

static bool EnsureNeural(NeuralState& ns, ShmMap& shm, uint32_t w, uint32_t h) {
    if (ns.ready && ns.w == w && ns.h == h) return true;
    if (ns.ngx.disabled) return false;

    if (ns.ngx.snippet) NgxReleaseAllPasses(ns.ngx, ns.vk.device);
    DestroyOpticalFlow(ns.vk, ns.flow);
    DestroyImage2D(ns.vk, ns.colorIn);
    DestroyImage2D(ns.vk, ns.workA);
    DestroyImage2D(ns.vk, ns.workB);
    DestroyImage2D(ns.vk, ns.mv);
    DestroyImage2D(ns.vk, ns.depth);
    ns.livePasses = 0;
    std::memset(ns.passDirty, 0, sizeof(ns.passDirty));

    if (!CreateImage2D(ns.vk, VK_FORMAT_R8G8B8A8_UNORM, w, h, ns.colorIn) ||
        !CreateImage2D(ns.vk, VK_FORMAT_R8G8B8A8_UNORM, w, h, ns.workA) ||
        !CreateImage2D(ns.vk, VK_FORMAT_R8G8B8A8_UNORM, w, h, ns.workB) ||
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
    // Motion is a different matter. It used to be zero for the same reason, which is what made the
    // pass weaker in motion than OptiScaler's -- the model judged every frame on its own with no
    // idea what had moved. It is now estimated here instead, with the optical-flow engine, from the
    // two frames the helper has anyway. bmitch87's work; DLSSNR_MVEC=0 turns it off.
    const char* mvecMode = getenv("DLSSNR_MVEC");
    const bool wantFlow = ns.mvecEnabled != 0 && !(mvecMode && !_stricmp(mvecMode, "0"));
    if (wantFlow && !SetupOpticalFlow(ns.vk, ns, w, h, ns.mvecQuality))
        Log("[helper] estimated motion vectors unavailable; falling back to a zero field");

    std::vector<uint8_t> zeros(size_t(w) * h * 4, 0);
    if (!UploadPixels(ns.vk, ns.mv, zeros.data(), zeros.size()) ||
        !UploadPixels(ns.vk, ns.depth, zeros.data(), zeros.size())) return false;

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
    if (!SubmitAndWait(ns.vk, ns.vk.cmdCreate) || !ok) {
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
    auto scanDirty = [&]() -> int {
        int first = -1;
        for (uint32_t i = 0; i < ns.livePasses; ++i) {
            const NgxTuning t = TuningFor(shm.hdr, i);
            if (!(t == ns.lastSeenTuning[i])) { ns.lastSeenTuning[i] = t; ns.tuningChangedMs = now; }
            ns.passDirty[i] = !(t == ns.tuning[i]);
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
            const NgxTuning t = TuningFor(shm.hdr, pass);
            NgxSetCreateTuning(ns.ngx, t);
            if (!BeginCmd(ns.vk.cmdCreate)) return;
            const bool built = NgxCreatePass(ns.ngx, pass, ns.w, ns.h, ns.vk.cmdCreate);
            SubmitAndWait(ns.vk, ns.vk.cmdCreate);
            if (built) {
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

static bool ProcessFrame(NeuralState& ns, ShmMap& shm) {
    uint32_t w = shm.hdr->width.load(), h = shm.hdr->height.load();
    if (!w || !h || w > kMaxW || h > kMaxH) return false;

    const size_t px = size_t(w) * h;
    const size_t bytes = px * 4;

    if (!ShmNeuralEnabled(shm.hdr)) {
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
    const bool mvecJustDisabled = prevMvecEnabled && !ns.mvecEnabled;
    const bool mvecJustEnabled = !prevMvecEnabled && ns.mvecEnabled;

    if (!EnsureNeural(ns, shm, w, h)) return false;
    MaintainPasses(ns, shm, wanted);
    if (!ns.ready || ns.livePasses == 0) return false;

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
    } else if (ns.mvecEnabled && !ns.flow.enabled &&
               (ns.flow.userDisabled || ns.flow.attemptedQuality != ns.mvecQuality)) {
        ns.flow.userDisabled = false;
        if (!SetupOpticalFlow(ns.vk, ns, w, h, ns.mvecQuality)) {
            Log("[helper] estimated motion vectors unavailable");
        } else {
            ns.firstFrame = true;
            ns.mvecResetPending = true;
            ns.lastResetLogged = 0xFFFFFFFFu;
        }
    }
    if (ns.flow.enabled && ns.flow.quality != ns.mvecQuality) {
        const int rc = ReactivateMotionVectors(ns, w, h, ns.mvecQuality);
        if (rc < 0) return false;
        if (rc == 0) Log("[helper] estimated motion vectors disabled after a quality change");
    }

    // The proxy the layer encoded. It is already R8G8B8A8_UNORM and display-referred, so there is
    // nothing to swizzle and nothing to convert.
    //
    // The flow engine wants the staging buffer too, and wants it larger than the frame when the
    // conversion runs partly on the host, so the size is settled before anything is copied in.
    size_t needed = bytes;
    if (ns.flow.enabled && (ns.flow.cpuOnly || ns.flow.hybrid)) {
        const size_t mvBytes = ImageSizeBytes(ns.vk, ns.mv);
        const size_t flowBytes = ImageSizeBytes(ns.vk, ns.flow.out);
        if (mvBytes > needed) needed = mvBytes;
        if (flowBytes > needed) needed = flowBytes;
    }
    if (needed > ns.vk.stagingSize && !CreateStaging(ns.vk, needed)) return false;
    std::memcpy(ns.vk.uploadMap, shm.inPixels, bytes);

    // A cut is not motion. Carrying a flow field across one hands the model a field describing a
    // scene that is no longer on screen, which is worse than handing it nothing.
    const bool sceneCut = DetectSceneCut(ns, shm.inPixels, w, h, 1);
    if (sceneCut && !ns.firstFrame && ns.flow.enabled) {
        ns.flow.hasPrev = false;
        ns.pendingMvClear = true;  // zeroed inside the flow prep submit, GPU-side
    }

    const double tUpload = time ? NowMs() : 0.0;
    if (ns.flow.enabled) {
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
    } else if (!UploadMappedPixels(ns.vk, ns.colorIn, bytes)) {
        return false;
    }

    const uint32_t passes = std::min(wanted, ns.livePasses);
    GpuImage* last = nullptr;

    for (uint32_t pass = 0; pass < passes; ++pass) {
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
        NgxSetResources(ns.ngx, rc, ro, rm, rd, w, h);

        // Sharpness is the one strength the model reads at evaluate, so it follows the setting
        // without a rebuild; everything else was latched when this pass's feature was built.
        const PassTuning ps = ShmResolvePass(shm.hdr, pass);
        NgxSetSharpness(ns.ngx, ClampF(ps.sharpness, 0.0f, 1.0f));

        if (!BeginCmd(ns.vk.cmdEval)) return false;
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

        if (!NgxEvaluatePass(ns.ngx, pass, ns.vk.cmdEval)) {
            vkEndCommandBuffer(ns.vk.cmdEval);
            return false;
        }
        if (!SubmitAndWait(ns.vk, ns.vk.cmdEval)) return false;
        last = out;
    }
    ns.mvecResetPending = false;
    ns.firstFrame = false;
    const double tEval = time ? NowMs() : 0.0;

    if (!last || !ReadbackPixels(ns.vk, *last, bytes)) return false;
    std::memcpy(shm.outPixels, ns.vk.readMap, bytes);
    const double tDone = time ? NowMs() : 0.0;

    ++ns.evaluates;
    if (shm.hdr) {
        ShmStore64(shm.hdr->helperFramesLo, shm.hdr->helperFramesHi, ns.evaluates);
        shm.hdr->helperEvalMsBits.store(FloatToBits(float(tEval - tUpload)));
        shm.hdr->helperFeatures.store(ns.livePasses);
        shm.hdr->modelUp.store(1);
    }

    if (time) {
        static int frameNo = 0;
        if (++frameNo % TimeInterval() == 0) {
            Log("[time] passes=%u/%u flow=%s upload=%.2f eval=%.2f readback=%.2f total=%.2f ms",
                passes, wanted, ns.flow.enabled ? "on" : "off",
                tUpload - t0, tEval - tUpload, tDone - tEval, tDone - t0);
        }
    }
    return true;
}

int main() {
    Log("=== dlssnr_helper starting ===");
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string dir(exePath);
        auto slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) {
            dir.resize(slash);
            SetCurrentDirectoryA(dir.c_str());
        }
    }
    InstallGuard();
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
                       "no NVIDIA device with the NVX extensions");
        return 3;
    }
    shm.hdr->helperState.store(kHelperRunning);
    Log("[helper] context ready, waiting for frames");

    uint32_t lastReq = shm.hdr->seq_resp.load();
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
        bool ok = ProcessFrame(ns, shm);
        if (!ok) Log("[helper] frame %u failed (w=%u h=%u)", req, shm.hdr->width.load(), shm.hdr->height.load());
        shm.hdr->seq_ok.store(ok ? req : 0);
        shm.hdr->seq_resp.store(req);
        lastReq = req;
        if (ns.ngx.disabled) {
            shm.hdr->quit.store(1);
            break;
        }
    }

    if (shm.hdr->quit.load()) Log("[helper] quit requested");
    else if (ns.ngx.disabled) Log("[helper] neural disabled");
    shm.hdr->helperState.store(kHelperStopped);
    Log("[helper] shutting down");
    if (ns.ngx.snippet) NgxTeardown(ns.ngx, ns.vk.device);
    vkDeviceWaitIdle(ns.vk.device);
    DestroyOpticalFlow(ns.vk, ns.flow);
    if (ns.vk.mvPipeLayout && vkDestroyPipelineLayout) vkDestroyPipelineLayout(ns.vk.device, ns.vk.mvPipeLayout, nullptr);
    if (ns.vk.mvDescLayout && vkDestroyDescriptorSetLayout) vkDestroyDescriptorSetLayout(ns.vk.device, ns.vk.mvDescLayout, nullptr);
    if (ns.vk.mvShaderModule && vkDestroyShaderModule) vkDestroyShaderModule(ns.vk.device, ns.vk.mvShaderModule, nullptr);
    if (ns.vk.semPrep && vkDestroySemaphore) vkDestroySemaphore(ns.vk.device, ns.vk.semPrep, nullptr);
    if (ns.vk.semFlow && vkDestroySemaphore) vkDestroySemaphore(ns.vk.device, ns.vk.semFlow, nullptr);
    if (ns.vk.cmdPoolFlow) vkDestroyCommandPool(ns.vk.device, ns.vk.cmdPoolFlow, nullptr);
    if (ns.vk.flowQuery) vkDestroyQueryPool(ns.vk.device, ns.vk.flowQuery, nullptr);
    if (ns.vk.queryStaging) vkDestroyBuffer(ns.vk.device, ns.vk.queryStaging, nullptr);
    if (ns.vk.queryMem) vkFreeMemory(ns.vk.device, ns.vk.queryMem, nullptr);
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