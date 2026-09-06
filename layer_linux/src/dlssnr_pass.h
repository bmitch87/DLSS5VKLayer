#pragma once
// The composition pass, on the layer's dispatch table.
//
// This is OptiScaler's DlssNr_Vk with the loader-exported Vulkan calls replaced by the layer's next-
// chain table; the SPIR-V is byte-for-byte the same module, so the picture this produces and the
// picture OptiScaler produces are the same picture by construction rather than by agreement.
//
// Three things are worth knowing before reading the implementation.
//
// Every binding is written every dispatch. The shader declares all seven resources at file scope and
// branches on gMode, so all of them are statically reachable from the entry point and Vulkan requires
// a valid descriptor for each one whether a given mode reads it or not. Slots a mode has no use for
// get a 1x1 dummy rather than a null handle.
//
// Constants are slotted rather than overwritten. A single mapped uniform buffer would be wrong here:
// encode and resolve run in the same frame with different constants, and the second write would land
// before the first dispatch had read it. The buffer holds a ring of slots and each dispatch takes the
// next, at an offset the device's own alignment rule allows.
//
// Layouts are the caller's to declare and this pass's to respect. It never guesses what state an
// image arrived in.
#include "shader_vk.h"
#include "dlssnr/DlssNr_Common.h"

namespace dlssnr {

class DlssNrPass : public Shader_Vk {
    // Enough slots for several dispatches per frame across the frames that can be in flight. Encode
    // and resolve are two; the debug views, the calibration grid and the downsample are the others.
    static constexpr uint32_t kSlotsPerFrame = 6;
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    VkDeviceSize _slotStride = 0;  // sizeof(DlssNrConstants), rounded up to the device's alignment
    uint32_t _slot = 0;            // next slot to hand out, wrapping

    // Stands in for a resource a given mode does not read. One pixel, never sampled for its content,
    // present only because Vulkan will not accept an unwritten binding.
    VkImage _dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory _dummyMemory = VK_NULL_HANDLE;
    VkImageView _dummyView = VK_NULL_HANDLE;
    bool _dummyReady = false;

    bool CreateDummy(VkCommandBuffer cmdList);

    void WriteDescriptors(VkDescriptorSet set, VkDeviceSize constantOffset, VkImageView source, VkImageView model,
                          VkImageView original, VkImageView motion, VkImageView target, VkImageView keep,
                          VkImageLayout sourceLayout, VkImageLayout motionLayout);

  public:
    DlssNrPass(const DeviceTable* InVk, const InstanceTable* InInstance, VkDevice InDevice,
               VkPhysicalDevice InPhysicalDevice);
    ~DlssNrPass() override;

    // One dispatch of the composition shader.
    //
    // Any of the four read views may be VK_NULL_HANDLE, in which case the dummy is bound; the two
    // written views may not, because a mode that writes nothing has no reason to run. This records
    // the dispatch and the barrier that follows it, not the transitions that got them there.
    bool Dispatch(VkCommandBuffer InCmdList, const DlssNrConstants& InConstants, uint32_t InThreadsX,
                  uint32_t InThreadsY, VkImageView InSource, VkImageView InModel, VkImageView InOriginal,
                  VkImageView InMotion, VkImageView InTarget, VkImageView InKeep,
                  VkImageLayout InSourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VkImageLayout InMotionLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
};

}  // namespace dlssnr
