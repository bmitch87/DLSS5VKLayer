#pragma once
// The next layer's entry points, resolved once per instance and per device.
//
// A layer must never call a Vulkan function through the loader's own exported symbol: that re-enters
// the top of the chain and, for anything this layer hooks, recurses. Everything below therefore goes
// through pfnNextGetInstanceProcAddr / pfnNextGetDeviceProcAddr, which is what these two tables hold.
//
// The lists are long because the composition runs a real compute pipeline on the game's device --
// images, views, descriptors, a pipeline and the dispatch -- rather than the transfer copies the
// first version of this layer needed.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace dlssnr {

#define DLSSNR_INSTANCE_FN_LIST(X)                                                                 \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkGetPhysicalDeviceMemoryProperties2)

#define DLSSNR_DEVICE_FN_LIST(X)                                                                   \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkGetDeviceQueue2)                                                                           \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkQueuePresentKHR)                                                                           \
    X(vkQueueSubmit)                                                                               \
    X(vkQueueWaitIdle)                                                                             \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkResetCommandPool)                                                                          \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkFreeCommandBuffers)                                                                        \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)                                                                               \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkBindBufferMemory)                                                                          \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCmdCopyImage)                                                                              \
    X(vkCmdBlitImage)                                                                              \
    X(vkCmdPipelineBarrier)                                                                        \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreateComputePipelines)                                                                    \
    X(vkDestroyPipeline)                                                                           \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdDispatch)                                                                               \
    X(vkCreateQueryPool)                                                                           \
    X(vkDestroyQueryPool)                                                                          \
    X(vkCmdResetQueryPool)                                                                         \
    X(vkCmdWriteTimestamp)                                                                         \
    X(vkGetQueryPoolResults)

struct InstanceTable {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;
#define X(name) PFN_##name name = nullptr;
    DLSSNR_INSTANCE_FN_LIST(X)
#undef X

    void Load(VkInstance instance) {
#define X(name) name = (PFN_##name) next_gipa(instance, #name);
        DLSSNR_INSTANCE_FN_LIST(X)
#undef X
    }
};

struct DeviceTable {
    PFN_vkGetDeviceProcAddr next_dpa = nullptr;
#define X(name) PFN_##name name = nullptr;
    DLSSNR_DEVICE_FN_LIST(X)
#undef X

    void Load(VkDevice device) {
#define X(name) name = (PFN_##name) next_dpa(device, #name);
        DLSSNR_DEVICE_FN_LIST(X)
#undef X
    }
};

// What the composition needs to exist at all. Anything absent from this list is either optional
// (timestamps, queue2) or already checked by the caller.
inline bool DeviceTableComplete(const DeviceTable& t) {
    return t.vkCreateImage && t.vkCreateImageView && t.vkAllocateMemory && t.vkBindImageMemory &&
           t.vkCreateBuffer && t.vkBindBufferMemory && t.vkMapMemory && t.vkCreateSampler &&
           t.vkCreateShaderModule && t.vkCreateDescriptorSetLayout && t.vkCreatePipelineLayout &&
           t.vkCreateDescriptorPool && t.vkAllocateDescriptorSets && t.vkUpdateDescriptorSets &&
           t.vkCreateComputePipelines && t.vkCmdBindPipeline && t.vkCmdBindDescriptorSets &&
           t.vkCmdDispatch && t.vkCmdPipelineBarrier && t.vkCmdCopyImage && t.vkCmdCopyImageToBuffer &&
           t.vkCmdCopyBufferToImage;
}

}  // namespace dlssnr
