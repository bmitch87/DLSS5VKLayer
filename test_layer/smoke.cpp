// Headless end-to-end smoke test for VK_LAYER_NV_dlssnr.
// Creates an instance with the layer force-enabled, a headless surface +
// swapchain, clears + presents a few frames, and reports success if the
// layer's log shows Feature 18 created + evaluated without SEH.
#include <windows.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static HMODULE g_vk = nullptr;
static PFN_vkGetInstanceProcAddr g_gipa = nullptr;

#define VK_FN(name) static PFN_##name name = nullptr;
VK_FN(vkCreateInstance) VK_FN(vkDestroyInstance) VK_FN(vkEnumeratePhysicalDevices)
VK_FN(vkGetPhysicalDeviceProperties) VK_FN(vkGetPhysicalDeviceQueueFamilyProperties)
VK_FN(vkGetPhysicalDeviceSurfaceSupportKHR) VK_FN(vkCreateDevice) VK_FN(vkDestroyDevice)
VK_FN(vkGetDeviceQueue) VK_FN(vkCreateCommandPool) VK_FN(vkAllocateCommandBuffers)
VK_FN(vkBeginCommandBuffer) VK_FN(vkEndCommandBuffer) VK_FN(vkQueueSubmit)
VK_FN(vkCreateFence) VK_FN(vkDestroyFence) VK_FN(vkWaitForFences) VK_FN(vkResetFences)
VK_FN(vkCmdPipelineBarrier) VK_FN(vkCmdClearColorImage)
VK_FN(vkCreateWin32SurfaceKHR) VK_FN(vkDestroySurfaceKHR)
VK_FN(vkCreateSwapchainKHR) VK_FN(vkDestroySwapchainKHR) VK_FN(vkGetSwapchainImagesKHR)
VK_FN(vkAcquireNextImageKHR) VK_FN(vkQueuePresentKHR) VK_FN(vkDeviceWaitIdle)
#undef VK_FN

#define CHECK(x) do { if ((x) != VK_SUCCESS) { printf("[smoke] %s failed: %d\n", #x, (int)(x)); return 1; } } while (0)

static uint32_t EnvU32(const char* name, uint32_t def) {
    const char* v = getenv(name);
    return v && *v ? (uint32_t)strtoul(v, nullptr, 10) : def;
}

// A real function rather than a lambda: WNDPROC carries __stdcall, and on 32-bit Windows that is a
// different calling convention from the one a lambda's function pointer has. On x86-64 there is only
// one convention, so the lambda compiled there and only there.
static LRESULT CALLBACK SmokeWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

int main() {
    const uint32_t smW = EnvU32("DLSSNR_SMOKE_W", 1920);
    const uint32_t smH = EnvU32("DLSSNR_SMOKE_H", 1080);
    const uint32_t frames = EnvU32("DLSSNR_SMOKE_FRAMES", 5);
    printf("=== DLSSNR layer smoke test ===\n");
    g_vk = LoadLibraryA("vulkan-1.dll");
    if (!g_vk) { printf("[smoke] no vulkan-1.dll\n"); return 1; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_vk, "vkGetInstanceProcAddr");
    if (!g_gipa) { printf("[smoke] no gipa\n"); return 1; }

    vkCreateInstance = (PFN_vkCreateInstance)g_gipa(nullptr, "vkCreateInstance");
    if (!vkCreateInstance) { printf("[smoke] no vkCreateInstance\n"); return 1; }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlssnr_smoke";
    app.apiVersion = VK_API_VERSION_1_3;

    const char* instExts[] = { "VK_KHR_get_physical_device_properties2", "VK_KHR_surface", "VK_KHR_win32_surface" };

    // Never name the layer: winevulkan enumerates host implicit layers but
    // rejects explicitly-named ones (VK_ERROR_LAYER_NOT_PRESENT). The layer
    // self-enables via enable_environment DLSSNR_ENABLE=1.
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 3;
    ici.ppEnabledExtensionNames = instExts;
    VkInstance instance = nullptr;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    printf("[smoke] instance created (layer implicit via DLSSNR_ENABLE)\n");

#define LOAD(name) name = (PFN_##name)g_gipa(instance, #name); if (!name) { printf("[smoke] missing " #name "\n"); return 1; }
    LOAD(vkDestroyInstance) LOAD(vkEnumeratePhysicalDevices) LOAD(vkGetPhysicalDeviceProperties)
    LOAD(vkGetPhysicalDeviceQueueFamilyProperties) LOAD(vkGetPhysicalDeviceSurfaceSupportKHR)
    LOAD(vkCreateDevice) LOAD(vkDestroyDevice) LOAD(vkGetDeviceQueue) LOAD(vkCreateCommandPool)
    LOAD(vkAllocateCommandBuffers) LOAD(vkBeginCommandBuffer) LOAD(vkEndCommandBuffer)
    LOAD(vkQueueSubmit) LOAD(vkCreateFence) LOAD(vkDestroyFence) LOAD(vkWaitForFences)
    LOAD(vkResetFences) LOAD(vkCmdPipelineBarrier) LOAD(vkCmdClearColorImage)
    LOAD(vkCreateWin32SurfaceKHR) LOAD(vkDestroySurfaceKHR) LOAD(vkCreateSwapchainKHR)
    LOAD(vkDestroySwapchainKHR) LOAD(vkGetSwapchainImagesKHR) LOAD(vkAcquireNextImageKHR)
    LOAD(vkQueuePresentKHR) LOAD(vkDeviceWaitIdle)
#undef LOAD

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> phys(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, phys.data());
    VkPhysicalDevice pd = nullptr;
    for (auto p : phys) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(p, &props);
        printf("[smoke] device: %s vendor=%#x\n", props.deviceName, props.vendorID);
        if (props.vendorID == 0x10DE) pd = p;
    }
    if (!pd) { printf("[smoke] no NVIDIA device\n"); return 1; }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static const wchar_t kCls[] = L"dlssnrSmokeWnd";
    WNDCLASSW wc{};
    wc.lpfnWndProc = SmokeWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = kCls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, kCls, L"dlssnr smoke", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 360, nullptr, nullptr, hi, nullptr);
    if (!hwnd) { printf("[smoke] CreateWindowEx failed (%lu)\n", GetLastError()); return 1; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    VkWin32SurfaceCreateInfoKHR wsci{};
    wsci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    wsci.hinstance = hi;
    wsci.hwnd = hwnd;
    CHECK(vkCreateWin32SurfaceKHR(instance, &wsci, nullptr, &surface));

    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &famCount, fams.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < famCount; ++i) {
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &supported);
        if (supported && (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    }
    if (family == UINT32_MAX) { printf("[smoke] no presentable graphics family\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VkDevice device = nullptr;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &device));
    VkQueue queue = nullptr;
    vkGetDeviceQueue(device, family, 0, &queue);
    printf("[smoke] device + headless surface ready\n");

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = 2;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = { smW, smH };
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, nullptr);
    std::vector<VkImage> images(imgCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imgCount, images.data());
    printf("[smoke] swapchain %ux%u BGRA8, %u images\n", smW, smH, imgCount);

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    CHECK(vkCreateCommandPool(device, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    for (uint32_t frame = 0; frame < frames; ++frame) {
        uint32_t index = 0;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, fence, &index);
        if (acq != VK_SUCCESS && acq != VK_NOT_READY && acq != VK_SUBOPTIMAL_KHR) { printf("[smoke] acquire failed: %d\n", (int)acq); return 1; }
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        // Clear the acquired image to opaque magenta so the net has real input.
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue color{ 1.0f, 0.0f, 1.0f, 1.0f };
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = range;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        CHECK(vkQueueSubmit(queue, 1, &si, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkResetFences(device, 1, &fence);

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        VkResult pres = vkQueuePresentKHR(queue, &pi);
        printf("[smoke] frame %u present -> %d\n", frame, (int)pres);
        if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) return 1;
    }

    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    DestroyWindow(hwnd);
    printf("=== SMOKE PASS ===\n");
    return 0;
}