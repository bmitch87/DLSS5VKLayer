#pragma once
// OptiScaler's Shader_Vk, on the layer's dispatch table.
//
// The original resolves Vulkan through the loader's exported symbols, which a layer must not do: the
// hooked entry points would recurse and the unhooked ones would re-enter the top of the chain. Every
// call here goes through the DeviceTable the layer built from pfnNextGetDeviceProcAddr instead. That
// is the only difference; the shape of the class, what it creates and in what order, is unchanged, so
// a fix on either side still reads as the same code.
#include "vk_table.h"

#include <string>
#include <vector>

namespace dlssnr {

// Matches NVSDK_NGX_ImageViewInfo_VK, as it does upstream, so an image described here can be handed
// straight to NGX by anything that later wants to.
struct VkImageInfo {
    VkImageView ImageView = VK_NULL_HANDLE;
    VkImage Image = VK_NULL_HANDLE;
    VkImageSubresourceRange SubresourceRange {};
    VkFormat Format = VK_FORMAT_UNDEFINED;
    unsigned int Width = 0;
    unsigned int Height = 0;
};

class Shader_Vk {
  protected:
    std::string _name;
    bool _init = false;

    const DeviceTable* _vk = nullptr;
    const InstanceTable* _instance = nullptr;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;

    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;

    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSets;
    uint32_t _maxFramesInFlight = 3;

    VkBuffer _constantBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _constantBufferMemory = VK_NULL_HANDLE;
    void* _mappedConstantBuffer = nullptr;
    VkSampler _textureSampler = VK_NULL_HANDLE;

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    bool CreateComputePipeline(VkPipelineLayout pipelineLayout, VkPipeline* pipeline,
                               const std::vector<char>& shaderCode, const char* entryPoint = "CSMain");
    bool CreateBufferResource(VkBuffer* buffer, VkDeviceMemory* memory, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);

    VkDescriptorSetLayoutBinding CreateBinding(uint32_t binding, VkDescriptorType descriptorType,
                                               uint32_t descriptorCount = 1,
                                               VkShaderStageFlags stageFlags = VK_SHADER_STAGE_COMPUTE_BIT);
    void CreateLayouts(const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    void CreateDescriptorPool(const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets);
    void CreateDescriptorSets(uint32_t count);
    void CreateSampler(VkFilter filter = VK_FILTER_LINEAR,
                       VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

  public:
    bool IsInit() const { return _init; }
    bool CanRender() const { return _init && _pipeline != VK_NULL_HANDLE; }

    Shader_Vk(std::string InName, const DeviceTable* InVk, const InstanceTable* InInstance, VkDevice InDevice,
              VkPhysicalDevice InPhysicalDevice);
    virtual ~Shader_Vk();

    Shader_Vk(const Shader_Vk&) = delete;
    Shader_Vk& operator=(const Shader_Vk&) = delete;

    // Moves an image between layouts with the conservative masks the upstream helper uses. Callers
    // that know better state their own barrier; this is for the ones that do not.
    void SetImageLayout(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkImageSubresourceRange subresourceRange) const;
};

}  // namespace dlssnr
