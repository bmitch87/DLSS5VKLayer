#include "shader_vk.h"
#include "log.h"

namespace dlssnr {

Shader_Vk::Shader_Vk(std::string InName, const DeviceTable* InVk, const InstanceTable* InInstance, VkDevice InDevice,
                     VkPhysicalDevice InPhysicalDevice)
    : _name(std::move(InName)), _vk(InVk), _instance(InInstance), _device(InDevice), _physicalDevice(InPhysicalDevice) {}

Shader_Vk::~Shader_Vk() {
    if (!_vk || _device == VK_NULL_HANDLE) return;

    if (_pipeline != VK_NULL_HANDLE) _vk->vkDestroyPipeline(_device, _pipeline, nullptr);
    if (_descriptorPool != VK_NULL_HANDLE) _vk->vkDestroyDescriptorPool(_device, _descriptorPool, nullptr);
    if (_descriptorSetLayout != VK_NULL_HANDLE)
        _vk->vkDestroyDescriptorSetLayout(_device, _descriptorSetLayout, nullptr);
    if (_pipelineLayout != VK_NULL_HANDLE) _vk->vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if (_mappedConstantBuffer && _vk->vkUnmapMemory) _vk->vkUnmapMemory(_device, _constantBufferMemory);
    if (_constantBuffer != VK_NULL_HANDLE) _vk->vkDestroyBuffer(_device, _constantBuffer, nullptr);
    if (_constantBufferMemory != VK_NULL_HANDLE) _vk->vkFreeMemory(_device, _constantBufferMemory, nullptr);
    if (_textureSampler != VK_NULL_HANDLE) _vk->vkDestroySampler(_device, _textureSampler, nullptr);

    _pipeline = VK_NULL_HANDLE;
    _descriptorPool = VK_NULL_HANDLE;
    _descriptorSetLayout = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _constantBuffer = VK_NULL_HANDLE;
    _constantBufferMemory = VK_NULL_HANDLE;
    _textureSampler = VK_NULL_HANDLE;
    _mappedConstantBuffer = nullptr;
}

uint32_t Shader_Vk::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memProperties {};
    _instance->vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    Log("[%s] no memory type with %#x", _name.c_str(), (unsigned) properties);
    return UINT32_MAX;
}

bool Shader_Vk::CreateComputePipeline(VkPipelineLayout pipelineLayout, VkPipeline* pipeline,
                                      const std::vector<char>& shaderCode, const char* entryPoint) {
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo createInfo {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = shaderCode.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());

    if (_vk->vkCreateShaderModule(_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        Log("[%s] vkCreateShaderModule failed", _name.c_str());
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStageInfo {};
    shaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStageInfo.module = shaderModule;
    shaderStageInfo.pName = entryPoint;

    VkComputePipelineCreateInfo pipelineInfo {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStageInfo;
    pipelineInfo.layout = pipelineLayout;

    const VkResult r = _vk->vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, pipeline);
    _vk->vkDestroyShaderModule(_device, shaderModule, nullptr);

    if (r != VK_SUCCESS) {
        Log("[%s] vkCreateComputePipelines -> %d", _name.c_str(), (int) r);
        return false;
    }
    return true;
}

bool Shader_Vk::CreateBufferResource(VkBuffer* buffer, VkDeviceMemory* memory, VkDeviceSize size,
                                     VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    if (*buffer != VK_NULL_HANDLE) {
        _vk->vkDestroyBuffer(_device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
    }
    if (*memory != VK_NULL_HANDLE) {
        _vk->vkFreeMemory(_device, *memory, nullptr);
        *memory = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bufferInfo {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (_vk->vkCreateBuffer(_device, &bufferInfo, nullptr, buffer) != VK_SUCCESS) {
        Log("[%s] vkCreateBuffer failed", _name.c_str());
        return false;
    }

    VkMemoryRequirements memRequirements {};
    _vk->vkGetBufferMemoryRequirements(_device, *buffer, &memRequirements);

    const uint32_t type = FindMemoryType(memRequirements.memoryTypeBits, properties);
    if (type == UINT32_MAX) return false;

    VkMemoryAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = type;

    if (_vk->vkAllocateMemory(_device, &allocInfo, nullptr, memory) != VK_SUCCESS) {
        Log("[%s] vkAllocateMemory failed (%llu bytes)", _name.c_str(), (unsigned long long) memRequirements.size);
        return false;
    }

    return _vk->vkBindBufferMemory(_device, *buffer, *memory, 0) == VK_SUCCESS;
}

VkDescriptorSetLayoutBinding Shader_Vk::CreateBinding(uint32_t binding, VkDescriptorType descriptorType,
                                                      uint32_t descriptorCount, VkShaderStageFlags stageFlags) {
    VkDescriptorSetLayoutBinding layoutBinding {};
    layoutBinding.binding = binding;
    layoutBinding.descriptorType = descriptorType;
    layoutBinding.descriptorCount = descriptorCount;
    layoutBinding.stageFlags = stageFlags;
    layoutBinding.pImmutableSamplers = nullptr;
    return layoutBinding;
}

void Shader_Vk::CreateLayouts(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    VkDescriptorSetLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (_vk->vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &_descriptorSetLayout) != VK_SUCCESS) {
        Log("[%s] vkCreateDescriptorSetLayout failed", _name.c_str());
        return;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;

    if (_vk->vkCreatePipelineLayout(_device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS)
        Log("[%s] vkCreatePipelineLayout failed", _name.c_str());
}

void Shader_Vk::CreateDescriptorPool(const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets) {
    VkDescriptorPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = maxSets;

    if (_vk->vkCreateDescriptorPool(_device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS)
        Log("[%s] vkCreateDescriptorPool failed", _name.c_str());
}

void Shader_Vk::CreateDescriptorSets(uint32_t count) {
    if (_descriptorSetLayout == VK_NULL_HANDLE || _descriptorPool == VK_NULL_HANDLE) return;

    std::vector<VkDescriptorSetLayout> layouts(count, _descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = _descriptorPool;
    allocInfo.descriptorSetCount = count;
    allocInfo.pSetLayouts = layouts.data();

    _descriptorSets.resize(count);
    if (_vk->vkAllocateDescriptorSets(_device, &allocInfo, _descriptorSets.data()) != VK_SUCCESS) {
        Log("[%s] vkAllocateDescriptorSets failed (%u sets)", _name.c_str(), count);
        _descriptorSets.clear();
    }
}

void Shader_Vk::CreateSampler(VkFilter filter, VkSamplerAddressMode addressMode) {
    VkSamplerCreateInfo samplerInfo {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = filter;
    samplerInfo.minFilter = filter;
    samplerInfo.addressModeU = addressMode;
    samplerInfo.addressModeV = addressMode;
    samplerInfo.addressModeW = addressMode;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (_vk->vkCreateSampler(_device, &samplerInfo, nullptr, &_textureSampler) != VK_SUCCESS)
        Log("[%s] vkCreateSampler failed", _name.c_str());
}

void Shader_Vk::SetImageLayout(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout,
                               VkImageLayout newLayout, VkImageSubresourceRange subresourceRange) const {
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = subresourceRange;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }

    if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        destinationStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }

    _vk->vkCmdPipelineBarrier(cmdBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace dlssnr
