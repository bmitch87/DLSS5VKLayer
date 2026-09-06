#pragma once
// OptiScaler's output-scaling pass, on the layer's dispatch table.
//
// Used here for one thing: supersampling. When the model works above the frame's resolution the proxy
// has to be enlarged on the way in and the model's answer averaged back on the way out, and doing
// either with a plain bilinear sampler aliases -- which is the whole reason upstream gave the pass its
// own downscaler rather than reusing the resolve's sampler.
//
// Two differences from upstream, both deliberate.
//
// The filter is chosen per instance at construction rather than read from a global on every dispatch,
// because the enlarge and the average want different ones and a global cannot hold two answers.
//
// The dispatch is sized from the images it is given. Upstream's Vulkan copy reads the sizes off
// State::currentFeature instead, which is the same fault its own notes record fixing in the Direct3D 12
// copy: "sized its dispatch from the global current feature rather than from the resources passed in.
// Those coincide for the conventional Output Scaling chain, so the bug stayed invisible until something
// else called it." This is something else calling it.
#include "shader_vk.h"

namespace dlssnr {

// Upstream's Scaler numbering, kept identical so a value copied from an OptiScaler profile means the
// same thing here. FSR1 is absent: it needs a different constant block and its own shader, and it is
// an upscaler rather than the averaging filter this pass wants.
enum ScalerFilter : uint32_t {
    kScalerFsr1 = 0,  // not supported here; falls back to Lanczos3
    kScalerBicubic = 1,
    kScalerCatmullRom = 2,
    kScalerLanczos2 = 3,
    kScalerLanczos3 = 4,
    kScalerKaiser2 = 5,
    kScalerKaiser3 = 6,
    kScalerMagic = 7,
    kScalerCount = 8,
};

// The protocol carries this number, so the two enumerations have to agree. They are separate types
// because the layer should not have to include the shared header to name a filter -- but a mismatch
// between them was a real bug: the default came out as Catmull-Rom because the protocol's older,
// narrower numbering put Lanczos3 at 2.
static_assert(kScalerLanczos3 == 4, "the protocol's Downscaler numbering must match this one");

const char* ScalerFilterName(uint32_t filter);

class ScalerVk : public Shader_Vk {
    static constexpr uint32_t kSlots = 6;

    bool _upsample = false;
    uint32_t _filter = kScalerLanczos3;
    VkDeviceSize _slotStride = 0;
    uint32_t _slot = 0;

  public:
    ScalerVk(const DeviceTable* InVk, const InstanceTable* InInstance, VkDevice InDevice,
             VkPhysicalDevice InPhysicalDevice, bool InUpsample, uint32_t InFilter);
    ~ScalerVk() override = default;

    bool Upsample() const { return _upsample; }
    uint32_t Filter() const { return _filter; }

    // Source must be in SHADER_READ_ONLY_OPTIMAL and destination in GENERAL; the caller states both,
    // as everywhere else here. Records the dispatch and the barrier after it.
    bool Dispatch(VkCommandBuffer cb, VkImageView source, VkImageView dest, uint32_t srcWidth,
                  uint32_t srcHeight, uint32_t destWidth, uint32_t destHeight);
};

}  // namespace dlssnr
