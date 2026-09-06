#include "scaler_vk.h"
#include "log.h"

#include "scaling/bcus_Shader_Vk.h"
#include "scaling/bcds_bicubic_Shader_Vk.h"
#include "scaling/bcds_catmull_Shader_Vk.h"
#include "scaling/bcds_lanczos2_Shader_Vk.h"
#include "scaling/bcds_lanczos3_Shader_Vk.h"
#include "scaling/bcds_kaiser2_Shader_Vk.h"
#include "scaling/bcds_kaiser3_Shader_Vk.h"
#include "scaling/bcds_magc_Shader_Vk.h"

#include <algorithm>
#include <cstring>

namespace dlssnr {
namespace {

// What the scaling shaders read. Upstream's Constants, spelled here so the layer does not have to
// carry the 33 KB of HLSL source strings that sit beside it in OS_Common.h for the D3D11 path.
struct alignas(256) ScalerConstants {
    int32_t srcWidth;
    int32_t srcHeight;
    int32_t destWidth;
    int32_t destHeight;
};

struct Blob {
    const unsigned char* data;
    size_t size;
};

Blob DownsampleBlob(uint32_t filter) {
    switch (filter) {
        case kScalerBicubic: return { bcds_bicubic_spv, sizeof(bcds_bicubic_spv) };
        case kScalerCatmullRom: return { bcds_catmull_spv, sizeof(bcds_catmull_spv) };
        case kScalerLanczos2: return { bcds_lanczos2_spv, sizeof(bcds_lanczos2_spv) };
        case kScalerLanczos3: return { bcds_lanczos3_spv, sizeof(bcds_lanczos3_spv) };
        case kScalerKaiser2: return { bcds_kaiser2_spv, sizeof(bcds_kaiser2_spv) };
        case kScalerKaiser3: return { bcds_kaiser3_spv, sizeof(bcds_kaiser3_spv) };
        case kScalerMagic: return { bcds_magc_spv, sizeof(bcds_magc_spv) };
        default: return { bcds_lanczos3_spv, sizeof(bcds_lanczos3_spv) };
    }
}

}  // namespace

const char* ScalerFilterName(uint32_t filter) {
    switch (filter) {
        case kScalerBicubic: return "bicubic";
        case kScalerCatmullRom: return "catmull-rom";
        case kScalerLanczos2: return "lanczos2";
        case kScalerLanczos3: return "lanczos3";
        case kScalerKaiser2: return "kaiser2";
        case kScalerKaiser3: return "kaiser3";
        case kScalerMagic: return "magic";
        default: return "lanczos3";
    }
}

ScalerVk::ScalerVk(const DeviceTable* InVk, const InstanceTable* InInstance, VkDevice InDevice,
                   VkPhysicalDevice InPhysicalDevice, bool InUpsample, uint32_t InFilter)
    : Shader_Vk(InUpsample ? "dlssnr-enlarge" : "dlssnr-average", InVk, InInstance, InDevice, InPhysicalDevice),
      _upsample(InUpsample),
      _filter(InFilter < kScalerCount && InFilter != kScalerFsr1 ? InFilter : kScalerLanczos3) {
    if (InDevice == VK_NULL_HANDLE) return;

    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (_textureSampler == VK_NULL_HANDLE) return;

    VkPhysicalDeviceProperties props{};
    _instance->vkGetPhysicalDeviceProperties(_physicalDevice, &props);
    const VkDeviceSize alignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    _slotStride = ((sizeof(ScalerConstants) + alignment - 1) / alignment) * alignment;

    if (!CreateBufferResource(&_constantBuffer, &_constantBufferMemory, _slotStride * kSlots,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return;
    if (_vk->vkMapMemory(_device, _constantBufferMemory, 0, _slotStride * kSlots, 0, &_mappedConstantBuffer) !=
        VK_SUCCESS)
        return;

    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        CreateBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLER),
    };
    CreateLayouts(bindings);
    if (_descriptorSetLayout == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE) return;

    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kSlots },
    };
    CreateDescriptorPool(poolSizes, kSlots);
    CreateDescriptorSets(kSlots);
    if (_descriptorSets.size() < kSlots) return;

    const Blob blob = _upsample ? Blob{ bcus_spv, sizeof(bcus_spv) } : DownsampleBlob(_filter);
    std::vector<char> code(blob.data, blob.data + blob.size);
    if (!CreateComputePipeline(_pipelineLayout, &_pipeline, code)) return;

    _init = true;
    Log("[scaler] %s ready, %s", _name.c_str(), _upsample ? "bicubic enlarge" : ScalerFilterName(_filter));
}

bool ScalerVk::Dispatch(VkCommandBuffer cb, VkImageView source, VkImageView dest, uint32_t srcWidth,
                        uint32_t srcHeight, uint32_t destWidth, uint32_t destHeight) {
    if (!CanRender() || cb == VK_NULL_HANDLE || source == VK_NULL_HANDLE || dest == VK_NULL_HANDLE) return false;

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kSlots;
    const VkDeviceSize offset = _slotStride * slot;

    // From the images this call was handed, not from a global that happens to agree most of the time.
    ScalerConstants c{};
    c.srcWidth = int32_t(srcWidth);
    c.srcHeight = int32_t(srcHeight);
    c.destWidth = int32_t(destWidth);
    c.destHeight = int32_t(destHeight);
    std::memcpy((char*) _mappedConstantBuffer + offset, &c, sizeof(c));

    VkDescriptorSet set = _descriptorSets[slot];
    VkDescriptorBufferInfo bufferInfo{ _constantBuffer, offset, sizeof(ScalerConstants) };
    VkDescriptorImageInfo sourceInfo{ VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo destInfo{ VK_NULL_HANDLE, dest, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo samplerInfo{ _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    const VkWriteDescriptorSet writes[] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr,
          &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          &sourceInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &destInfo,
          nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo,
          nullptr, nullptr },
    };
    _vk->vkUpdateDescriptorSets(_device, uint32_t(sizeof(writes) / sizeof(writes[0])), writes, 0, nullptr);

    _vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    _vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);

    // The enlarge shader's group is 16x16 and every averaging shader's is 8x8, which is what the
    // vendored modules declare and what upstream dispatches them at.
    const uint32_t tile = _upsample ? 16u : 8u;
    _vk->vkCmdDispatch(cb, (destWidth + tile - 1) / tile, (destHeight + tile - 1) / tile, 1);

    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                              &barrier, 0, nullptr, 0, nullptr);
    return true;
}

}  // namespace dlssnr
