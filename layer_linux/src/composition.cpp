#include "composition.h"
#include "log.h"
#include "shaders/meter_reduce_spv.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include <unistd.h>
#include <fcntl.h>

namespace dlssnr {

// The meter state buffer and its host mirror share one layout, pinned to match
// the MeterState block in shaders/meter_reduce.comp.
namespace {
constexpr size_t kMeterStateBytes = 128;
constexpr VkDeviceSize kMeterMeasuredOffset = 4;
constexpr VkDeviceSize kMeterResolvedOffset = 8;
constexpr VkDeviceSize kMeterSteadinessOffset = 12;

struct MeterPush {
    float manual;
    float scale;
    float trim;
    float holdValue;
    uint32_t source;
    uint32_t hold;
};
static_assert(sizeof(MeterPush) == 24, "must match the push_constant block in meter_reduce.comp");
}  // namespace

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------
VkFormat CompositionFormat(VkFormat swapchainFormat) {
    switch (swapchainFormat) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

bool ColourIsLinearHdr(VkFormat swapchainFormat, uint32_t colourMode) {
    if (colourMode == kColourDisplay) return false;
    if (colourMode == kColourLinearHdr) return true;
    // Auto. An 8-bit frame has been tone mapped or there would be nothing to see, and HDR10's
    // ten-bit formats carry PQ, which is display-referred as well. Only a float swapchain is light.
    return swapchainFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
FrameSettings FrameSettings::Read(const ShmHeader* h) {
    FrameSettings s;
    if (!h) return s;
    s.transferStrength = BitsToFloat(h->transferStrengthBits.load());
    s.colourStrength = BitsToFloat(h->colourStrengthBits.load());
    s.maxRatio = BitsToFloat(h->maxRatioBits.load());
    s.debugScale = BitsToFloat(h->debugScaleBits.load());
    s.compareSplit = BitsToFloat(h->compareSplitBits.load());
    s.compareZoom = BitsToFloat(h->compareZoomBits.load());
    s.workingScale = BitsToFloat(h->workingScaleBits.load());
    s.transfer = h->transfer.load();
    s.debugView = h->debugView.load();
    s.compareMode = h->compareMode.load();
    s.compareSwap = h->compareSwap.load();
    s.reversibleMode = h->reversibleMode.load();
    s.applyModel = h->applyModel.load();
    s.holdFrame = h->holdFrame.load();
    s.downscaler = h->scalingDownscaler.load();
    s.compositionBypass = h->compositionBypass.load();
    {
        // DLSSNR_SETTLE, in the same hundredths, so the ramp can be swept from a launch option --
        // including back to 100, which is the behaviour before it existed.
        static const int forced = [] {
            const char* v = getenv("DLSSNR_SETTLE");
            return v && *v ? atoi(v) : -1;
        }();
    }
    {
        static const int forced = [] {
            const char* v = getenv("DLSSNR_GHOST_SLACK");
            return v && *v ? atoi(v) : -1;
        }();
        if (forced >= 0) s.ghostSlack = float(forced) / 100.0f;
        if (!std::isfinite(s.ghostSlack) || s.ghostSlack < 0.0f) s.ghostSlack = 0.5f;
    }
    {
        s.ratioSmooth = float(h->ratioSmoothPercent.load()) / 100.0f;
        static const int forcedRs = [] {
            const char* v = getenv("DLSSNR_RATIO_SMOOTH");
            return v && *v ? atoi(v) : -1;
        }();
        if (forcedRs >= 0) s.ratioSmooth = float(forcedRs) / 100.0f;
        if (!std::isfinite(s.ratioSmooth) || s.ratioSmooth < 0.0f) s.ratioSmooth = 0.0f;
        if (s.ratioSmooth > 1.0f) s.ratioSmooth = 1.0f;

        s.colourTrust = float(h->colourTrustPercent.load()) / 100.0f;
        static const int forcedCt = [] {
            const char* v = getenv("DLSSNR_COLOUR_TRUST");
            return v && *v ? atoi(v) : -1;
        }();
        if (forcedCt >= 0) s.colourTrust = float(forcedCt) / 100.0f;
        if (!std::isfinite(s.colourTrust) || s.colourTrust < 0.0f) s.colourTrust = 1.0f;
        if (s.colourTrust > 8.0f) s.colourTrust = 8.0f;

        static const int forced = [] {
            const char* v = getenv("DLSSNR_MOTION_SMOOTH");
            return v && *v ? atoi(v) : -1;
        }();
        if (forced >= 0) s.motionSmooth = float(forced) / 100.0f;
        if (!std::isfinite(s.motionSmooth) || s.motionSmooth < 0.0f) s.motionSmooth = 0.0f;
        if (s.motionSmooth > 1.0f) s.motionSmooth = 1.0f;
    }
    {
        static const int forced = [] {
            const char* v = getenv("DLSSNR_EDIT_BLUR");
            return v && *v ? atoi(v) : -1;
        }();
        if (forced >= 0) s.editBlur = float(forced) / 1000.0f;
        if (!std::isfinite(s.editBlur) || s.editBlur < 0.0f) s.editBlur = 0.0f;
        if (s.editBlur > 0.25f) s.editBlur = 0.25f;
    }


    s.whitePointManual = BitsToFloat(h->whitePointBits.load());
    s.whitePointScale = BitsToFloat(h->whitePointScaleBits.load());
    s.whitePointTrim = BitsToFloat(h->whitePointTrimBits.load());
    s.whitePointSource = h->whitePointSource.load();

    // Clamped here rather than trusted, because these come from a file any process can write.
    const auto clamp = [](float v, float lo, float hi, float fallback) {
        if (!std::isfinite(v)) return fallback;
        return std::min(std::max(v, lo), hi);
    };
    s.transferStrength = clamp(s.transferStrength, 0.0f, 4.0f, 1.0f);
    s.colourStrength = clamp(s.colourStrength, 0.0f, 4.0f, 1.0f);
    s.maxRatio = clamp(s.maxRatio, 1.0f, float(kMaxPasses), 2.0f);
    s.debugScale = clamp(s.debugScale, 0.01f, 100.0f, 1.0f);
    s.whitePointManual = clamp(s.whitePointManual, 1e-4f, 2000.0f, 1.0f);
    s.whitePointScale = clamp(s.whitePointScale, 0.01f, 100.0f, 1.0f);
    s.whitePointTrim = clamp(s.whitePointTrim, 0.01f, 100.0f, 1.0f);
    if (s.whitePointSource > kWhitePointMeasured) s.whitePointSource = kWhitePointManual;
    s.compareSplit = clamp(s.compareSplit, 0.0f, 1.0f, 0.5f);
    s.compareZoom = clamp(s.compareZoom, 1.0f, 2.0f, 1.0f);

    // Above 1.0 the model supersamples, up to upstream's 2x ceiling.
    s.workingScale = clamp(s.workingScale, 0.25f, 2.0f, 1.0f);
    if (s.downscaler >= kScalerCount || s.downscaler == kScalerFsr1) s.downscaler = kScalerLanczos3;

    // Native + edit is mode 2; the clamp used to stop at 1 and silently killed it.
    if (s.transfer > 2) s.transfer = 2;
    // 4 and 5 are the two views of the colour bound. This clamp is why they did nothing when they
    // were added: the shader grew the cases and the validation did not, so the GUI offered them, the
    // header carried them, and the layer quietly rewrote them to 0 on the way past.
    if (s.debugView > 5) s.debugView = 0;
    if (s.compareMode > 2) s.compareMode = 0;
    if (s.reversibleMode >= kReversibleModeCount) s.reversibleMode = kReversibleKnee;
    return s;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
Composition::Composition(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
                         VkPhysicalDevice physicalDevice)
    : _vk(vk), _instance(instance), _device(device), _physicalDevice(physicalDevice) {
    if (!DeviceTableComplete(*vk)) {
        _reason = "the device does not expose everything a compute pass needs";
        Log("[comp] %s", _reason.c_str());
        return;
    }

    _pass = std::make_unique<DlssNrPass>(vk, instance, device, physicalDevice);
    if (!_pass->CanRender()) {
        _reason = "the composition pipeline could not be built";
        _pass.reset();
        return;
    }

    _usable = true;
}

Composition::~Composition() {
    DropAll();
    DropMeterObjects();
    _pass.reset();
}

void Composition::DropAll() {
    DropImage(_frame);
    DropImage(_keep);
    DropImage(_proxy);
    DropImage(_work);
    DropImage(_model);
    DropImage(_composed);
    DropImage(_modelNative);
    DropImage(_meter);
    DropXfer();
    DropMeterState();
    DropHostBuffer(_download);
    DropHostBuffer(_upload);
    _superUp.reset();
    _superDown.reset();
    _superSample = false;
    _width = _height = _modelW = _modelH = 0;
    _haveModel = false;
    _frameCaptured = false;
    _captureRecorded = false;
    _measuredWhitePoint = 0.0f;
    _meterSteadiness = 0.0f;
}

bool Composition::FormatSupportsStorage(VkFormat format) const {
    VkFormatProperties props{};
    _instance->vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

// Both directions, because the swapchain is the source of the capture and the destination of the
// composition, and a blit needs the format to allow each end it is used at.
bool Composition::FormatSupportsBlit(VkFormat format) const {
    VkFormatProperties props{};
    _instance->vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &props);
    const VkFormatFeatureFlags both = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (props.optimalTilingFeatures & both) == both;
}

bool Composition::MakeImage(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    DropImage(img);

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (_vk->vkCreateImage(_device, &ci, nullptr, &img.image) != VK_SUCCESS) {
        Log("[comp] vkCreateImage %ux%u fmt=%d failed", w, h, (int) format);
        return false;
    }

    VkMemoryRequirements req{};
    _vk->vkGetImageMemoryRequirements(_device, img.image, &req);

    VkPhysicalDeviceMemoryProperties mp{};
    _instance->vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (_vk->vkAllocateMemory(_device, &mai, nullptr, &img.memory) != VK_SUCCESS) {
        Log("[comp] out of device memory for a %ux%u surface", w, h);
        return false;
    }
    if (_vk->vkBindImageMemory(_device, img.image, img.memory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (_vk->vkCreateImageView(_device, &vi, nullptr, &img.view) != VK_SUCCESS) return false;

    img.format = format;
    img.width = w;
    img.height = h;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void Composition::DropImage(Image& img) {
    if (img.view) _vk->vkDestroyImageView(_device, img.view, nullptr);
    if (img.image) _vk->vkDestroyImage(_device, img.image, nullptr);
    if (img.memory) _vk->vkFreeMemory(_device, img.memory, nullptr);
    img = Image{};
}

bool Composition::MakeHostBuffer(HostBuffer& buf, size_t bytes, VkBufferUsageFlags usage) {
    DropHostBuffer(buf);

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (_vk->vkCreateBuffer(_device, &bci, nullptr, &buf.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    _vk->vkGetBufferMemoryRequirements(_device, buf.buffer, &req);

    VkPhysicalDeviceMemoryProperties mp{};
    _instance->vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &mp);

    // Host visible and coherent is the requirement; cached is a large win on the readback and
    // harmless on the upload, so it is preferred rather than demanded.
    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        int score = 0;
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) score += 100;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) score -= 50;
        if (score > bestScore) { bestScore = score; best = (int) i; }
    }
    if (best < 0) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = uint32_t(best);
    if (_vk->vkAllocateMemory(_device, &mai, nullptr, &buf.memory) != VK_SUCCESS) return false;
    if (_vk->vkBindBufferMemory(_device, buf.buffer, buf.memory, 0) != VK_SUCCESS) return false;
    if (_vk->vkMapMemory(_device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped) != VK_SUCCESS) return false;

    buf.size = bytes;
    return true;
}

void Composition::DropHostBuffer(HostBuffer& buf) {
    if (buf.mapped && !buf.hostPtr) _vk->vkUnmapMemory(_device, buf.memory);
    if (buf.buffer) _vk->vkDestroyBuffer(_device, buf.buffer, nullptr);
    if (buf.memory) _vk->vkFreeMemory(_device, buf.memory, nullptr);
    buf = HostBuffer{};
}

// A buffer whose device memory IS the shared-memory region, so the GPU writes the proxy straight
// into the bytes the helper reads and reads the answer straight out of the bytes the helper wrote.
// The host pointer is the mapping both processes share; the driver tells us which memory type may
// back it. Anything the driver declines -- no extension, an alignment it will not take -- falls
// back to private staging in EnsureTransport, which is the arrangement that shipped before.
bool Composition::MakeTransportBuffer(HostBuffer& buf, size_t bytes, VkBufferUsageFlags usage,
                                      void* hostPtr) {
    DropHostBuffer(buf);
    if (!hostPtr || !_vk->vkGetMemoryHostPointerPropertiesEXT) return false;

    // The driver both names the memory type that may back this pointer and says nothing about its
    // alignment; the alignment the extension demands is a physical-device property (64 KiB on
    // NVIDIA). The mappings are made at offsets that satisfy it, but the check stays: a mapping
    // that arrived misaligned falls back to staging rather than failing the allocation.
    VkDeviceSize align = 0;
    if (_instance->vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{};
        hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &hostProps;
        _instance->vkGetPhysicalDeviceProperties2(_physicalDevice, &props2);
        align = hostProps.minImportedHostPointerAlignment;
    }
    if (!align) align = 1;
    if (reinterpret_cast<uintptr_t>(hostPtr) % align) return false;

    VkMemoryHostPointerPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (_vk->vkGetMemoryHostPointerPropertiesEXT(
            _device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostPtr, &props) != VK_SUCCESS)
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
    if (_vk->vkCreateBuffer(_device, &bci, nullptr, &buf.buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    _vk->vkGetBufferMemoryRequirements(_device, buf.buffer, &req);
    const uint32_t typeBits = req.memoryTypeBits & props.memoryTypeBits;
    if (!typeBits) {
        _vk->vkDestroyBuffer(_device, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
        return false;
    }
    uint32_t type = 0;
    while (!(typeBits & (1u << type))) ++type;

    VkImportMemoryHostPointerInfoEXT hpi{};
    hpi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
    hpi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    hpi.pHostPointer = hostPtr;
    // The allocation size must be a multiple of the import alignment -- a model raster of, say,
    // 1920x1080x4 is not. Rounding up is legal (the buffer only reads the bytes it was created for)
    // and the mapping is rounded to the same figure in ShmMapFrames, so the extra stays inside the
    // file's pages. Without this the import fails at almost every resolution and the staging path
    // silently carries every frame.
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &hpi;
    mai.allocationSize = (req.size + align - 1) & ~(align - 1);
    mai.memoryTypeIndex = type;
    if (_vk->vkAllocateMemory(_device, &mai, nullptr, &buf.memory) != VK_SUCCESS ||
        _vk->vkBindBufferMemory(_device, buf.buffer, buf.memory, 0) != VK_SUCCESS) {
        _vk->vkDestroyBuffer(_device, buf.buffer, nullptr);
        buf = HostBuffer{};
        return false;
    }

    buf.mapped = hostPtr;
    buf.hostPtr = hostPtr;
    buf.size = bytes;
    return true;
}

// Build the transport pair against whatever mapping is currently set: imported when the shared
// memory covers the frame, private host-visible staging otherwise. Called from Prepare (which
// knows the model size) and from SetTransport (which knows the mapping), so a resize and a remap
// each land on the right rebuild.
void Composition::EnsureTransport() {
    if (!_frame.image) return;
    const size_t bytes = ModelBytes();
    if (_download.buffer && _download.hostPtr == _transportIn && _download.size >= bytes &&
        _upload.buffer && _upload.hostPtr == _transportOut && _upload.size >= bytes)
        return;

    bool imported = false;
    if (_transportIn && _transportOut && _transportBytes >= bytes) {
        imported =
            MakeTransportBuffer(_download, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, _transportIn) &&
            MakeTransportBuffer(_upload, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, _transportOut);
        if (!imported) {
            DropHostBuffer(_download);
            DropHostBuffer(_upload);
        }
    }
    if (!imported) {
        MakeHostBuffer(_download, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        MakeHostBuffer(_upload, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
}

void Composition::SetTransport(void* inRegion, void* outRegion, size_t bytes) {
    if (inRegion == _transportIn && outRegion == _transportOut && bytes == _transportBytes) return;
    _transportIn = inRegion;
    _transportOut = outRegion;
    _transportBytes = bytes;
    EnsureTransport();
}

// ---------------------------------------------------------------------------
// The white-point meter, on the GPU
// ---------------------------------------------------------------------------
// The reduce pipeline is size-independent and survives rebuilds; only the state buffer, the mirror
// and the descriptor's image binding follow the meter image.
bool Composition::BuildMeterPipeline() {
    if (_meterPipeline) return true;
    if (!_vk->vkCreateShaderModule || !_vk->vkCmdPushConstants || !_vk->vkCmdFillBuffer) return false;

    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = kMeterReduceSpvLen * sizeof(uint32_t);
    smci.pCode = kMeterReduceSpv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (_vk->vkCreateShaderModule(_device, &smci, nullptr, &module) != VK_SUCCESS)
        return false;

    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 2;
    dli.pBindings = bindings;
    if (_vk->vkCreateDescriptorSetLayout(_device, &dli, nullptr, &_meterDescriptorLayout) != VK_SUCCESS) {
        _vk->vkDestroyShaderModule(_device, module, nullptr);
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(MeterPush);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &_meterDescriptorLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (_vk->vkCreatePipelineLayout(_device, &pli, nullptr, &_meterPipelineLayout) != VK_SUCCESS) {
        _vk->vkDestroyShaderModule(_device, module, nullptr);
        return false;
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (_vk->vkCreateSampler(_device, &sci, nullptr, &_meterSampler) != VK_SUCCESS) {
        _vk->vkDestroyShaderModule(_device, module, nullptr);
        return false;
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (_vk->vkCreateDescriptorPool(_device, &dpci, nullptr, &_meterDescriptorPool) != VK_SUCCESS) {
        _vk->vkDestroyShaderModule(_device, module, nullptr);
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = _meterDescriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_meterDescriptorLayout;
    if (_vk->vkAllocateDescriptorSets(_device, &dsai, &_meterDescriptorSet) != VK_SUCCESS) {
        _vk->vkDestroyShaderModule(_device, module, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = module;
    cpi.stage.pName = "main";
    cpi.layout = _meterPipelineLayout;
    const VkResult built = _vk->vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                                         &_meterPipeline);
    _vk->vkDestroyShaderModule(_device, module, nullptr);
    return built == VK_SUCCESS && _meterPipeline != VK_NULL_HANDLE;
}

void Composition::DropMeterObjects() {
    if (_meterPipeline && _vk->vkDestroyPipeline) _vk->vkDestroyPipeline(_device, _meterPipeline, nullptr);
    if (_meterPipelineLayout) _vk->vkDestroyPipelineLayout(_device, _meterPipelineLayout, nullptr);
    if (_meterDescriptorLayout) _vk->vkDestroyDescriptorSetLayout(_device, _meterDescriptorLayout, nullptr);
    if (_meterDescriptorPool) _vk->vkDestroyDescriptorPool(_device, _meterDescriptorPool, nullptr);
    if (_meterSampler) _vk->vkDestroySampler(_device, _meterSampler, nullptr);
    _meterPipeline = VK_NULL_HANDLE;
    _meterPipelineLayout = VK_NULL_HANDLE;
    _meterDescriptorLayout = VK_NULL_HANDLE;
    _meterDescriptorPool = VK_NULL_HANDLE;
    _meterDescriptorSet = VK_NULL_HANDLE;
    _meterSampler = VK_NULL_HANDLE;
}

bool Composition::BuildMeterDescriptors() {
    if (!_meterDescriptorSet || !_meter.view || !_meterState) return false;
    VkDescriptorImageInfo grid{ _meterSampler, _meter.view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo state{ _meterState, 0, VK_WHOLE_SIZE };
    const VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _meterDescriptorSet, 0, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &grid, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _meterDescriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &state, nullptr },
    };
    _vk->vkUpdateDescriptorSets(_device, 2, writes, 0, nullptr);
    return true;
}

// The meter's device-local state (percentile, history, resolved value) plus the 128-byte host
// mirror the CPU reads after leg 1's fence -- for the frame-hold snapshot and the status field.
bool Composition::MakeMeterState() {
    DropMeterState();
    if (!BuildMeterPipeline()) return false;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = kMeterStateBytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (_vk->vkCreateBuffer(_device, &bci, nullptr, &_meterState) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    _vk->vkGetBufferMemoryRequirements(_device, _meterState, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    _instance->vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        _vk->vkDestroyBuffer(_device, _meterState, nullptr);
        _meterState = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (_vk->vkAllocateMemory(_device, &mai, nullptr, &_meterStateMemory) != VK_SUCCESS ||
        _vk->vkBindBufferMemory(_device, _meterState, _meterStateMemory, 0) != VK_SUCCESS) {
        DropMeterState();
        return false;
    }
    if (!MakeHostBuffer(_meterMirror, kMeterStateBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        DropMeterState();
        return false;
    }
    if (!BuildMeterDescriptors()) {
        DropMeterState();
        return false;
    }
    _meterStateCleared = false;
    _meterGpu = true;
    return true;
}

void Composition::DropMeterState() {
    _meterGpu = false;
    DropHostBuffer(_meterMirror);
    if (_meterState) _vk->vkDestroyBuffer(_device, _meterState, nullptr);
    if (_meterStateMemory) _vk->vkFreeMemory(_device, _meterStateMemory, nullptr);
    _meterState = VK_NULL_HANDLE;
    _meterStateMemory = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------
// The model works at this fraction of the frame.
//
// No rounding to a workgroup multiple: every dispatch here covers a partial group and the shader
// bounds-checks against gWidth/gHeight, so alignment buys nothing -- and rounding *up* was worse
// than nothing, because at a scale of exactly 1.0 it pushed a 500-pixel frame to 504 and quietly
// engaged supersampling on a setting that means "leave it alone". A floor of 64 only stops a
// pathologically small window from producing a degenerate raster.
void Composition::ModelExtent(uint32_t width, uint32_t height, const FrameSettings& s,
                              uint32_t& modelW, uint32_t& modelH) {
    const auto scaled = [&](uint32_t v) {
        if (s.workingScale == 1.0f) return v;
        return std::max<uint32_t>(64, uint32_t(std::lround(double(v) * double(s.workingScale))));
    };
    modelW = scaled(width);
    modelH = scaled(height);
}

bool Composition::Prepare(uint32_t width, uint32_t height, VkFormat swapchainFormat, const FrameSettings& s,
                          bool linearHdr, bool hdrProxy, uint32_t hdrTransfer) {
    if (!_usable) return false;

    VkFormat work = CompositionFormat(swapchainFormat);
    if (work == VK_FORMAT_UNDEFINED) {
        _reason = "unsupported swapchain format";
        return false;
    }

    // The composed surface is written as a storage image and then handed back to the swapchain. When
    // the swapchain's own UNORM twin can be written that way, that is what everything internal uses:
    // it shares the swapchain's bit layout, so both ends are a byte-for-byte copy and nothing is
    // reinterpreted.
    //
    // Not every presentable format can be written as a storage image, though. NVIDIA does not expose
    // A2R10G10B10_UNORM_PACK32 that way, and that is exactly what a 10-bit desktop hands most games
    // -- so the pass used to switch itself off, for the whole run, on the machines it was written
    // for. Compose in half float in that case and blit at both ends instead: the blit converts, and
    // sixteen bits a channel hold more than the ten the swapchain can show, so nothing is lost that
    // the display could have displayed.
    bool blit = false;
    if (!FormatSupportsStorage(work)) {
        const VkFormat wide = VK_FORMAT_R16G16B16A16_SFLOAT;
        if (FormatSupportsStorage(wide) && FormatSupportsBlit(wide) && FormatSupportsBlit(swapchainFormat)) {
            // Prepare runs every frame; this is only news when the swapchain changed under it.
            if (swapchainFormat != _swapchainFormat)
                Log("[comp] format %d cannot be written as a storage image here; composing in half float",
                    (int) work);
            work = wide;
            blit = true;
        } else {
            _reason = "this device cannot write the swapchain's format as a storage image";
            Log("[comp] %s (format %d)", _reason.c_str(), (int) work);
            _usable = false;
            return false;
        }
    }

    // A known divergence from the spec, carried deliberately and inherited from upstream.
    //
    // The vendored module declares its two storage images with an explicit format operand, Rgba32f.
    // Vulkan wants the bound image view's format to match that, and none of the views here do: the
    // proxy is R8G8B8A8_UNORM because eight bits is all that crosses to the model, and the composed
    // surface has to be the swapchain's own UNORM twin or the copy back would not be byte-exact.
    // Validation reports it as Undefined-Value-StorageImage-FormatMismatch-ImageView.
    //
    // Upstream has the same mismatch -- DlssNrFeature_Vk binds R16G16B16A16_SFLOAT to the same
    // Rgba32f declaration -- so this is the arrangement the shader has always run in. Measured on an
    // RTX 5090 by reading the composed surface back and comparing it against the frame it was made
    // from: means within a tenth of a level per channel, centre pixel within one level, alpha exact.
    // The writes land correctly on this driver.
    //
    // It is still undefined behaviour by the letter of the spec, and the two real fixes are known:
    // recompile the shader with [[vk::image_format]] matching what is bound, which cannot work while
    // one binding serves surfaces of three different formats; or recompile it with an Unknown format
    // and enable shaderStorageImageWriteWithoutFormat on the device, which is the arrangement this
    // pass actually needs and which vkBasalt reaches by a similar route.
    uint32_t modelW = 0, modelH = 0;
    ModelExtent(width, height, s, modelW, modelH);
    const bool superSample = modelW > width || modelH > height;

    // The float16 proxy needs a surface the shader can write and sample as float. Where the device
    // says it cannot, the request quietly becomes the 8-bit arrangement that shipped before -- the
    // same shape as every other capability step in this file.
    if (hdrProxy) {
        VkFormatProperties fp16{};
        _instance->vkGetPhysicalDeviceFormatProperties(_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &fp16);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp16.optimalTilingFeatures & need) != need) {
            if (!_hdrProxy) Log("[comp] float16 proxy requested but not supported here; staying 8-bit");
            hdrProxy = false;
            hdrTransfer = 0;
        }
    }

    if (_width == width && _height == height && _swapchainFormat == swapchainFormat &&
        _modelW == modelW && _modelH == modelH && _linearHdr == linearHdr && _hdrProxy == hdrProxy &&
        _hdrTransfer == hdrTransfer && _scalerFilter == s.downscaler && _frame.image)
        return true;

    Log("[comp] building %ux%u, model %ux%u, %s%s%s", width, height, modelW, modelH,
        linearHdr ? "linear HDR" : "display-referred",
        hdrProxy ? (hdrTransfer ? ", float16 proxy (PQ in)" : ", float16 proxy") : "",
        superSample ? " (supersampling)" : "");

    if (superSample) {
        // Said out loud because it is the transport, not the GPU, that decides whether this is
        // usable: the proxy and the answer both cross shared memory at the model's raster, so the
        // per-frame copy grows with the square of the scale.
        const double mb = double(modelW) * modelH * (hdrProxy ? 8.0 : 4.0) / (1024.0 * 1024.0);
        Log("[comp] supersampling to %ux%u means %.0f MB across shared memory each way, every frame",
            modelW, modelH, mb);
    }

    DropAll();

    _swapchainFormat = swapchainFormat;
    _workFormat = work;
    _blitSwapchain = blit;
    _linearHdr = linearHdr;
    _hdrProxy = hdrProxy;
    _hdrTransfer = hdrProxy ? hdrTransfer : 0;
    const VkFormat proxyFormat = _hdrProxy ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;

    // The untouched copy is float only when the frame it holds is: on a display-referred frame the
    // swapchain's own format loses nothing and costs half the memory.
    _keepFormat = linearHdr ? VK_FORMAT_R16G16B16A16_SFLOAT : work;

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags src = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags dst = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    const bool ok =
        MakeImage(_frame, width, height, work, sampled | dst) &&
        MakeImage(_keep, width, height, _keepFormat, sampled | storage) &&
        MakeImage(_proxy, width, height, proxyFormat, sampled | storage | src) &&
        MakeImage(_model, modelW, modelH, proxyFormat, sampled | src | dst) &&
        MakeImage(_composed, width, height, work, storage | src);

    // The transport pair is sized to the model raster and rebuilt against whatever mapping is
    // currently pointed at it -- the shared-memory regions when there are any, staging otherwise.
    _modelW = modelW;
    _modelH = modelH;
    EnsureTransport();
    const bool okTransport = _download.buffer && _upload.buffer;


    // The meter is a fixed 64x64 grid whatever the frame is, and is only built when there is
    // something to measure: on a frame the game already tone mapped there is no white point to find,
    // so the dispatch and its reduction are skipped entirely rather than run and ignored. The
    // reduction -- percentile, gates, history, resolve -- runs on the GPU; only a 128-byte mirror
    // of its answer ever reaches the CPU.
    const bool okMeter =
        !linearHdr ||
        (MakeImage(_meter, kDlssNrMeterGrid, kDlssNrMeterGrid, VK_FORMAT_R32_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
         MakeMeterState());

    const bool needWork = (modelW != width || modelH != height);
    const bool okWork = !needWork || MakeImage(_work, modelW, modelH, proxyFormat,
                                               sampled | storage | src);

    // The averaged answer, and the two filters that get there. Built only when supersampling.
    bool okSuper = true;
    if (superSample) {
        okSuper = MakeImage(_modelNative, width, height, proxyFormat, sampled | storage);
        _superUp = std::make_unique<ScalerVk>(_vk, _instance, _device, _physicalDevice, true, s.downscaler);
        _superDown = std::make_unique<ScalerVk>(_vk, _instance, _device, _physicalDevice, false, s.downscaler);
        if (!_superUp->CanRender() || !_superDown->CanRender()) {
            Log("[comp] the resampling filters could not be built; supersampling is unavailable");
            _superUp.reset();
            _superDown.reset();
            okSuper = false;
        }
    } else {
        _superUp.reset();
        _superDown.reset();
    }

    if (!ok || !okTransport || !okWork || !okMeter || !okSuper) {
        _reason = "could not allocate the composition surfaces";
        DropAll();
        return false;
    }

    _width = width;
    _height = height;
    _modelW = modelW;
    _modelH = modelH;
    _superSample = superSample;
    _scalerFilter = s.downscaler;
    _reason.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Phase 5: dma-buf transport
// ---------------------------------------------------------------------------
// The proxy and the answer as exportable device memory. The layer exports the proxy's memory as a
// file descriptor and the helper imports it, and the other way round for the answer, so the pixels
// cross the process boundary as VRAM rather than as host pages -- no GPU->host->GPU round trip at
// all. The descriptors travel over a unix socket (see fd_channel); the shared-memory transport
// above stays as the fallback for every step that can fail: no channel, no extension, a driver
// that refuses the export or the import.
//
// The images are CONCURRENT so they may be used by two devices, and each side hands the other one
// through VK_QUEUE_FAMILY_FOREIGN_EXT: the producer releases to FOREIGN after its last write, the
// consumer acquires from FOREIGN before its first read. That is the spec's shape for sharing with
// "another API", which from one driver's point of view the other process's driver is.

// The dma-buf must land in a memory type the driver allows for this handle type, and
// VkMemoryRequirements does not say which ones those are. Candidates are tried in order; each
// attempt gets its own duplicate of the descriptor, because a failed import may or may not have
// consumed the fd depending on the driver. The winner keeps its duplicate, the losers close
// theirs, and the caller closes the original either way.
bool Composition::ImportFdMemory(int fd, const VkMemoryRequirements& req, VkDeviceMemory* out) {
    for (uint32_t type = 0; type < 32; ++type) {
        if (!(req.memoryTypeBits & (1u << type))) continue;
        const int dup = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (dup < 0) return false;
        VkImportMemoryFdInfoKHR imp{};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        imp.fd = dup;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &imp;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (_vk->vkAllocateMemory(_device, &mai, nullptr, out) == VK_SUCCESS) return true;
        close(dup);
    }
    return false;
}

bool Composition::ImportProxy(int fd, uint32_t w, uint32_t h) {
    if (fd < 0 || !_vk->vkAllocateMemory || !w || !h) {
        if (fd >= 0) close(fd);
        return false;
    }
    // A new descriptor is a new image -- a restarted helper names a fresh buffer behind the same
    // raster -- so the old import goes away first. Nothing of ours may be in flight against it.
    if (_proxyXfer.image && _vk->vkDeviceWaitIdle) _vk->vkDeviceWaitIdle(_device);
    DropImage(_proxyXfer);

    // The proxy is the helper's memory; this device only ever writes it, and the helper reads it
    // as its own. CONCURRENT because two devices touch it, and the handle type says the fd names a
    // dma-buf rather than an opaque driver object.
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = _hdrProxy ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (_vk->vkCreateImage(_device, &ci, nullptr, &_proxyXfer.image) != VK_SUCCESS) {
        close(fd);
        return false;
    }
    _proxyXfer.format = ci.format;
    _proxyXfer.width = w;
    _proxyXfer.height = h;

    VkMemoryRequirements req{};
    _vk->vkGetImageMemoryRequirements(_device, _proxyXfer.image, &req);
    if (!ImportFdMemory(fd, req, &_proxyXfer.memory)) {
        DropImage(_proxyXfer);
        close(fd);
        return false;
    }
    close(fd);  // the winning duplicate holds its own reference
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = _proxyXfer.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = _proxyXfer.format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (_vk->vkCreateImageView(_device, &vi, nullptr, &_proxyXfer.view) != VK_SUCCESS) {
        DropImage(_proxyXfer);
        return false;
    }
    _proxyXfer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Log("[comp] proxy imported at %ux%u (dma-buf)", w, h);
    return true;
}
bool Composition::ImportAnswerFd(int fd, uint32_t w, uint32_t h) {
    if (fd < 0 || !_vk->vkAllocateMemory || !w || !h) {
        if (fd >= 0) close(fd);
        return false;
    }
    // As with the proxy: a new descriptor names a new image, and the old import must not outlive
    // it. The wait covers the resolve that may still be sampling the old surface.
    if (_answerXfer.image && _vk->vkDeviceWaitIdle) _vk->vkDeviceWaitIdle(_device);
    DropImage(_answerXfer);

    // The image is created first so its memory requirements say what the import needs; the fd is
    // then consumed by the allocation that backs it. CONCURRENT because two devices touch it, and
    // the handle type says the fd is a dma-buf rather than an opaque driver object.
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = _hdrProxy ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (_vk->vkCreateImage(_device, &ci, nullptr, &_answerXfer.image) != VK_SUCCESS) {
        close(fd);
        return false;
    }
    _answerXfer.format = ci.format;
    _answerXfer.width = w;
    _answerXfer.height = h;

    VkMemoryRequirements req{};
    _vk->vkGetImageMemoryRequirements(_device, _answerXfer.image, &req);
    if (!ImportFdMemory(fd, req, &_answerXfer.memory)) {
        DropImage(_answerXfer);
        close(fd);
        return false;
    }
    close(fd);  // the winning duplicate holds its own reference
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = _answerXfer.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = _answerXfer.format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (_vk->vkCreateImageView(_device, &vi, nullptr, &_answerXfer.view) != VK_SUCCESS) {
        DropImage(_answerXfer);
        return false;
    }
    _answerXfer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Log("[comp] answer imported at %ux%u (dma-buf)", w, h);
    return true;
}

void Composition::DropXfer() {
    // The answer surface is sampled by submitted resolves, so nothing may be in flight when it
    // goes away. This runs on channel loss and on teardown, not per frame -- the wait is free.
    if ((_proxyXfer.image || _answerXfer.image) && _vk && _vk->vkDeviceWaitIdle)
        _vk->vkDeviceWaitIdle(_device);
    DropImage(_proxyXfer);
    DropImage(_answerXfer);
}
void Composition::Transition(VkCommandBuffer cb, Image& img, VkImageLayout to) {
    if (img.layout == to) return;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    _pass->SetImageLayout(cb, img.image, img.layout, to, range);
    img.layout = to;
}

void Composition::TransitionSwapchain(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &b);
}

// One step in or out of the swapchain. A copy when the two formats share a bit layout, which is the
// usual case and moves the bytes untouched; a blit when the working format had to differ, which
// converts between them. Same extent either way -- this never resamples.
void Composition::CopyWholeImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
                                 VkImageLayout dstLayout, uint32_t w, uint32_t h) {
    if (_blitSwapchain) {
        VkImageBlit b{};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.srcOffsets[1] = { (int32_t) w, (int32_t) h, 1 };
        b.dstOffsets[1] = { (int32_t) w, (int32_t) h, 1 };
        _vk->vkCmdBlitImage(cb, src, srcLayout, dst, dstLayout, 1, &b, VK_FILTER_NEAREST);
        return;
    }
    VkImageCopy copy{};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { w, h, 1 };
    _vk->vkCmdCopyImage(cb, src, srcLayout, dst, dstLayout, 1, &copy);
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
// Where the white point comes from, in one place.
//
// The measured reading is only taken when there is one: the meter needs a lit scene and a linear
// frame to say anything, and until it has spoken the slider is the answer rather than zero. The scale
// applies to whichever was chosen, because it is the user saying what the model should treat as white
// rather than a property of the measurement.
float Composition::ResolvedWhitePoint(const FrameSettings& s) const {
    float base = s.whitePointManual;
    if (s.whitePointSource == kWhitePointMeasured && _measuredWhitePoint > 0.0f)
        base = _measuredWhitePoint * s.whitePointTrim;
    const float wp = base * s.whitePointScale;
    return std::min(std::max(wp, 1e-4f), 2000.0f);
}

DlssNrConstants Composition::BaseConstants(const FrameSettings& s) const {
    DlssNrConstants c{};

    c.WhitePoint = (_holding && _frameCaptured) ? _heldWhitePoint : ResolvedWhitePoint(s);
    c.TransferStrength = s.transferStrength;
    c.ColourStrength = s.colourStrength;
    c.MaxRatio = s.maxRatio;
    c.DebugView = s.debugView;
    c.DebugScale = s.debugScale;
    c.Transfer = s.transfer;
    c.CompareMode = s.compareMode;
    c.CompareSplit = s.compareSplit;
    c.CompareZoom = s.compareZoom;
    c.CompareSwap = s.compareSwap;
    c.ReversibleMode = s.reversibleMode;
    c.ApplyModel = s.applyModel;

    // The model's answer IS the frame. The raw-answer debug path returns the model's picture ahead of
    // every step of the composition -- no ratio, no guard, no blend, no compare -- and it returns
    // before the normalisation step, so the scale that step would have applied has to come from here.
    //
    // That early return is also why compare did nothing under a bypass: the overlay lives at the tail
    // of the resolve, past every return, so the raw path showed one picture with a divider across it
    // and nothing to compare. The replace modes are the only route that presents the model's answer
    // without returning early -- pure inverse of the encode, none of the composition -- so comparing
    // under a bypass borrows them: soft knee and Neutwo pair with NeutwoDecode, Hybrid with
    // HybridDecode. The encode reads these same constants, so the pair stays an exact inverse. The
    // price is that the proxy shown to the model while comparing is the replace mode's own rather
    // than the user's, which is acceptable for a diagnostic view and why this is not done when the
    // composition is on, where the overlay already runs on the composed picture.
    if (s.compositionBypass) {
        if (s.compareMode != 0) {
            c.ReversibleMode = s.reversibleMode >= 3 ? 4u : 2u;
        } else {
            c.DebugView = 2;
            c.DebugScale = _linearHdr ? ResolvedWhitePoint(s) : 1.0f;
        }
    }

    // A frame the game already tone mapped goes through the encode untouched, and the composition
    // works in its units rather than normalising by a white point that means nothing here.
    c.Passthrough = _linearHdr ? 0u : 1u;

    // No motion vectors and no exposure reach a present-time layer. The guides are declared at the
    // frame's own size so nothing downstream scales by a ratio that does not exist.
    c.MvScaleX = 1.0f;
    c.MvScaleY = 1.0f;
    c.GuideWidth = _width;
    c.GuideHeight = _height;
    c.UseGameExposure = 0;
    c.ExposurePreMul = 1.0f;
    c.HdrProxy = _hdrProxy ? 1u : 0u;
    c.HdrTransfer = _hdrProxy ? _hdrTransfer : 0u;
    c.ColourTrust = s.colourTrust;
    c.RatioSmooth = s.ratioSmooth;
    return c;
}

// ---------------------------------------------------------------------------
// Leg 1: the frame the model is shown
// ---------------------------------------------------------------------------
bool Composition::RecordCapture(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s) {
    if (!_usable || !_frame.image) return false;

    // Frame hold, on the edge rather than the level, so the white point is snapshotted once at the
    // moment it comes on rather than re-read every frame it stays on. The snapshot comes from the
    // meter's mirror -- the resolved value the GPU settled on for the frame just captured -- so a
    // hold freezes the same number the resolve has been using, not a host-side approximation.
    if (s.holdFrame && !_holding) {
        _holding = true;
        const float* mirror = _meterGpu && _meterMirror.mapped ? (const float*) _meterMirror.mapped : nullptr;
        _heldWhitePoint = mirror && mirror[2] > 0.0f ? mirror[2] : ResolvedWhitePoint(s);
        Log("[comp] frame held (white point %.3f)", double(_heldWhitePoint));
    } else if (!s.holdFrame && _holding) {
        _holding = false;
        Log("[comp] frame released");
    }

    // While held the frame is not re-read, but everything downstream of it still runs: the encode
    // re-encodes, the model re-evaluates and the resolve re-composes, so a setting changed now is
    // answered on the same picture. Freezing the proxy instead would be wrong -- settings must still
    // re-encode -- and freezing it would also desynchronise it from the untouched keep.
    const bool freeze = _holding && _frameCaptured;

    if (!freeze) {
        TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyWholeImage(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _frame.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, _width, _height);

        // Straight back, so that every path out of this leg -- including the ones that give up --
        // leaves the image in the layout the present engine requires.
        TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        _frameCaptured = true;
    }

    Transition(cb, _frame, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cb, _proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cb, _keep, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrConstants enc = BaseConstants(s);
    enc.Mode = DlssNrMode_Encode;
    enc.Width = _width;
    enc.Height = _height;
    if (!_pass->Dispatch(cb, enc, _width, _height, _frame.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                         _proxy.view, _keep.view))
        return false;

    // The meter, measured off the untouched copy and never off anything this pass writes. That
    // distinction is the whole reason it is safe: an earlier white point meter upstream read its own
    // output and chased it, walking one session from 0.010 to 97.910. There is no path from what this
    // pass writes back into what this reads.
    //
    // The grid lands in a device-local state buffer and the reduce pass turns it into the resolved
    // white point without the bytes ever crossing to the host; the resolve reads the answer through
    // a four-byte copy into its own constant block (RecordCompose). The 128-byte mirror the transfer
    // copies at the end is for the CPU's frame-hold snapshot and status field only.
    if (_meter.image && _meterGpu) {
        Transition(cb, _keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cb, _meter, VK_IMAGE_LAYOUT_GENERAL);

        DlssNrConstants meter = BaseConstants(s);
        meter.Mode = DlssNrMode_Calibrate;
        meter.Width = kDlssNrMeterGrid;
        meter.Height = kDlssNrMeterGrid;
        if (_pass->Dispatch(cb, meter, kDlssNrMeterGrid, kDlssNrMeterGrid, _keep.view, VK_NULL_HANDLE,
                            VK_NULL_HANDLE, VK_NULL_HANDLE, _meter.view, VK_NULL_HANDLE)) {
            if (!_meterStateCleared) {
                _vk->vkCmdFillBuffer(cb, _meterState, 0, VK_WHOLE_SIZE, 0);
                _meterStateCleared = true;
            }
            MeterPush pc{};
            pc.manual = s.whitePointManual;
            pc.scale = s.whitePointScale;
            pc.trim = s.whitePointTrim;
            pc.holdValue = _holding ? _heldWhitePoint : 0.0f;
            pc.source = s.whitePointSource;
            pc.hold = _holding && _frameCaptured ? 1u : 0u;
            _vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _meterPipeline);
            _vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, _meterPipelineLayout, 0, 1,
                                         &_meterDescriptorSet, 0, nullptr);
            _vk->vkCmdPushConstants(cb, _meterPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(pc), &pc);
            _vk->vkCmdDispatch(cb, 1, 1, 1);

            // The mirror must show this frame's answer, so the transfer read waits on the reduce's
            // write. The state buffer stays in its default layout; buffers need no transition.
            VkBufferMemoryBarrier toMirror{};
            toMirror.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toMirror.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toMirror.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            toMirror.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toMirror.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toMirror.buffer = _meterState;
            toMirror.offset = 0;
            toMirror.size = VK_WHOLE_SIZE;
            _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0, 0, nullptr, 1, &toMirror, 0, nullptr);
            const VkBufferCopy mirror{ 0, 0, kMeterStateBytes };
            _vk->vkCmdCopyBuffer(cb, _meterState, _meterMirror.buffer, 1, &mirror);
        }
        Transition(cb, _keep, VK_IMAGE_LAYOUT_GENERAL);
    }

    // What the model is actually handed: the full-resolution proxy, or a reduction of it.
    Image* source = &_proxy;
    if (_work.image) {
        Transition(cb, _proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cb, _work, VK_IMAGE_LAYOUT_GENERAL);

        if (_superSample) {
            // Enlarge, so the model has more pixels to synthesise into than the frame has.
            if (!_superUp->Dispatch(cb, _proxy.view, _work.view, _width, _height, _modelW, _modelH))
                return false;
        } else {
            // Reduce, with the module's own area filter -- the model then works on fewer pixels and
            // less crosses the shared memory.
            DlssNrConstants down = BaseConstants(s);
            down.Mode = DlssNrMode_Downsample;
            down.Width = _modelW;
            down.Height = _modelH;
            if (!_pass->Dispatch(cb, down, _modelW, _modelH, _proxy.view, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                 VK_NULL_HANDLE, _work.view, VK_NULL_HANDLE))
                return false;
        }
        source = &_work;
    }

    Transition(cb, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { _modelW, _modelH, 1 };

    // dma-buf: the proxy goes to the exportable image and is released to the other process's
    // driver, which sees it as a foreign consumer. The host transport is skipped entirely -- the
    // bytes never leave VRAM.
    if (ProxyActive()) {
        // Acquire from FOREIGN: the helper was the last user of this memory and released it back
        // before processing on, so the layout it reports is UNDEFINED and its caches are flushed.
        VkImageMemoryBarrier pacq{};
        pacq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        pacq.srcAccessMask = 0;
        pacq.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pacq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pacq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pacq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        pacq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pacq.image = _proxyXfer.image;
        pacq.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &pacq);
        _proxyXfer.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageCopy copy{};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.extent = { _modelW, _modelH, 1 };
        _vk->vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _proxyXfer.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Release to FOREIGN: the helper's driver acquires from the same domain before it reads.
        Transition(cb, _proxyXfer, VK_IMAGE_LAYOUT_GENERAL);
        VkImageMemoryBarrier rel{};
        rel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rel.dstAccessMask = 0;
        rel.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        rel.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        rel.image = _proxyXfer.image;
        rel.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &rel);
        _proxyXfer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The helper reads this image through the fd on exactly the frame the flag was set for,
        // and the flag is set when and only when this branch runs, so the host transport stays
        // untouched. One decision, one surface, one frame.
        return true;
    }

    _vk->vkCmdCopyImageToBuffer(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _download.buffer, 1,
                                &region);
    return true;
}

// ---------------------------------------------------------------------------
// Leg 2: the model's answer, composed back
// ---------------------------------------------------------------------------
bool Composition::RecordCompose(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s) {
    if (!_usable || !_composed.image) return false;

    // dma-buf: the answer is already device memory this process can read -- the helper released it
    // before it answered, and the sequence handshake ordered that before this submit. No upload.
    // The flag is the layer's own decision from before the request went out, so it and the helper
    // agree on which surface holds this frame's answer.
    const bool dmaAnswer = AnswerViaFd();
    const bool rawCopy = s.compositionBypass != 0 && s.compareMode == 0 && !_capture.Active() &&
                         !_superSample && !_hdrProxy && !_linearHdr &&
                         _modelW == _width && _modelH == _height &&
                         (dmaAnswer ? _answerXfer.format : _model.format) == _workFormat;
    if (dmaAnswer) {
        // Acquire from FOREIGN, then into the layout the resolve samples in. The first barrier's
        // old layout is the one the exporter left it in; the image's own tracking says UNDEFINED
        // because this device has never written it, which is exactly the state an import starts in.
        VkImageMemoryBarrier acq{};
        acq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acq.srcAccessMask = 0;
        acq.dstAccessMask = rawCopy ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT;
        acq.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acq.newLayout = rawCopy ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acq.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acq.image = _answerXfer.image;
        acq.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  rawCopy ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &acq);
        _answerXfer.layout = rawCopy ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        Transition(cb, _model, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { _modelW, _modelH, 1 };
        _vk->vkCmdCopyBufferToImage(cb, _upload.buffer, _model.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);
        Transition(cb, _model, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // When the model worked above the frame its answer is averaged back to native first, and the
    // composition then sees a native proxy against a native answer -- which is what it should see,
    // because from its point of view the model effectively ran at the frame's own resolution.
    Image* answer = dmaAnswer ? &_answerXfer : &_model;
    Image* source = _work.image ? &_work : &_proxy;
    if (rawCopy) {
        Transition(cb, *answer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CopyWholeImage(cb, answer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, _width, _height);
        TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        if (dmaAnswer) {
            VkImageMemoryBarrier rel{};
            rel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            rel.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            rel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            rel.image = _answerXfer.image;
            rel.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &rel);
            _answerXfer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        return true;
    }
    if (_superSample) {
        Transition(cb, _modelNative, VK_IMAGE_LAYOUT_GENERAL);
        if (!_superDown->Dispatch(cb, answer->view, _modelNative.view, _modelW, _modelH, _width, _height))
            return false;
        Transition(cb, _modelNative, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        answer = &_modelNative;
        source = &_proxy;
    }

    Transition(cb, *source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cb, _keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cb, _composed, VK_IMAGE_LAYOUT_GENERAL);

    DlssNrConstants res = BaseConstants(s);
    res.Mode = DlssNrMode_Resolve;
    res.Width = _width;
    res.Height = _height;

    // When the meter feeds the white point, the resolve must see the GPU's resolved value, not the
    // host's one-frame-stale mirror of it: copy the four bytes straight from the meter state into
    // the constant slot this dispatch is about to take, over the placeholder the host wrote. The
    // host memcpy ran before submit, so this transfer write is the last writer ahead of the
    // dispatch's uniform read, and the barrier states that.
    if (_meterGpu && s.whitePointSource == kWhitePointMeasured) {
        const VkDeviceSize slotOff = _pass->ConstantSlotStride() * _pass->NextConstantSlot() +
                                     offsetof(DlssNrConstants, WhitePoint);
        const VkBufferCopy patch{ kMeterResolvedOffset, slotOff, sizeof(float) };
        _vk->vkCmdCopyBuffer(cb, _meterState, _pass->ConstantBuffer(), 1, &patch);
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = _pass->ConstantBuffer();
        b.offset = slotOff;
        b.size = sizeof(float);
        _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  0, 0, nullptr, 1, &b, 0, nullptr);
    }

    if (!_pass->Dispatch(cb, res, _width, _height, source->view, answer->view, _keep.view, VK_NULL_HANDLE,
                         _composed.view, VK_NULL_HANDLE))
        return false;

    // The answer's memory belongs to the helper's device between frames: hand it back before the
    // helper's next write, so neither side's caches hold a version the other has moved past.
    if (dmaAnswer) {
        VkImageMemoryBarrier rel{};
        rel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rel.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rel.dstAccessMask = 0;
        rel.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rel.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        rel.image = _answerXfer.image;
        rel.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        _vk->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &rel);
        _answerXfer.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    Transition(cb, _composed, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyWholeImage(cb, _composed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, _width, _height);
    TransitionSwapchain(cb, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // The pair, taken here because this is the one place that holds both the frame as the game
    // presented it and the frame the model edited, for the same frame.
    _captureRecorded = false;
    if (_capture.Active()) {
        const size_t bytes = size_t(_width) * _height * (_workFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4);
        if (_captureBuf.buffer || MakeHostBuffer(_captureBuf, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            Transition(cb, _frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(cb, _composed, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r{};
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.imageExtent = { _width, _height, 1 };
            _vk->vkCmdCopyImageToBuffer(cb, _frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        _captureBuf.buffer, 1, &r);
            r.bufferOffset = bytes;
            _vk->vkCmdCopyImageToBuffer(cb, _composed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        _captureBuf.buffer, 1, &r);
            _captureRecorded = true;
        }
    }

    // The keep is written by the next encode, so it goes back to where that dispatch expects it.
    Transition(cb, _keep, VK_IMAGE_LAYOUT_GENERAL);
    return true;
}

void Composition::WriteCapturedFrame() {
    if (!_captureRecorded || !_captureBuf.mapped) return;
    _captureRecorded = false;
    const size_t bytes = size_t(_width) * _height * (_workFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4);
    const uint8_t* base = (const uint8_t*) _captureBuf.mapped;
    _capture.WriteFrame(base, base + bytes, _width, _height, uint32_t(_workFormat));
}


// The percentile, the gates and the history now run on the GPU (shaders/meter_reduce.comp); this
// reads the 128-byte mirror of that state, recorded in leg 1 and landed by leg 1's fence. The
// reasoning the shader reproduces: the 90th percentile of tile peaks, not the maximum (a sun or a
// specular hit would normalise the whole picture into the dark) and not the mean (scene brightness
// says nothing about the buffer's scale); offered only when enough of the frame carries light
// against its own brightest tile, since the units are the game's and there is no absolute scale.
// A rejected reading carries the previous answer forward on the GPU, so the value here never needs
// to be zeroed by a dark or torn frame.
void Composition::ConsumeMeter() {
    if (!_meterGpu || !_meterMirror.mapped) return;
    const float* mirror = (const float*) _meterMirror.mapped;
    _measuredWhitePoint = mirror[1];
    _meterSteadiness = mirror[3];
}

}  // namespace dlssnr
