#include "composition.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dlssnr {

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
    if (s.debugView > 3) s.debugView = 0;
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
    DropHostBuffer(_meterBuf);
    DropHostBuffer(_download);
    DropHostBuffer(_upload);
    _superUp.reset();
    _superDown.reset();
    _superSample = false;
    _width = _height = _modelW = _modelH = 0;
    _haveModel = false;
    _frameCaptured = false;
    _captureRecorded = false;
    _meterRecorded = false;
    _meterCount = 0;
    _measuredWhitePoint = 0.0f;
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
    if (buf.mapped) _vk->vkUnmapMemory(_device, buf.memory);
    if (buf.buffer) _vk->vkDestroyBuffer(_device, buf.buffer, nullptr);
    if (buf.memory) _vk->vkFreeMemory(_device, buf.memory, nullptr);
    buf = HostBuffer{};
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------
bool Composition::Prepare(uint32_t width, uint32_t height, VkFormat swapchainFormat, const FrameSettings& s,
                          bool linearHdr) {
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
    //
    // The model works at this fraction of the frame.
    //
    // No rounding to a workgroup multiple: every dispatch here covers a partial group and the shader
    // bounds-checks against gWidth/gHeight, so alignment buys nothing -- and rounding *up* was worse
    // than nothing, because at a scale of exactly 1.0 it pushed a 500-pixel frame to 504 and quietly
    // engaged supersampling on a setting that means "leave it alone". A floor of 64 only stops a
    // pathologically small window from producing a degenerate raster.
    const auto scaled = [&](uint32_t v) {
        if (s.workingScale == 1.0f) return v;
        return std::max<uint32_t>(64, uint32_t(std::lround(double(v) * double(s.workingScale))));
    };
    const uint32_t modelW = scaled(width);
    const uint32_t modelH = scaled(height);
    const bool superSample = modelW > width || modelH > height;

    if (_width == width && _height == height && _swapchainFormat == swapchainFormat &&
        _modelW == modelW && _modelH == modelH && _linearHdr == linearHdr &&
        _scalerFilter == s.downscaler && _frame.image)
        return true;

    Log("[comp] building %ux%u, model %ux%u, %s%s", width, height, modelW, modelH,
        linearHdr ? "linear HDR" : "display-referred",
        superSample ? " (supersampling)" : "");

    if (superSample) {
        // Said out loud because it is the transport, not the GPU, that decides whether this is
        // usable: the proxy and the answer both cross shared memory at the model's raster, so the
        // per-frame copy grows with the square of the scale.
        const double mb = double(modelW) * modelH * 4.0 / (1024.0 * 1024.0);
        Log("[comp] supersampling to %ux%u means %.0f MB across shared memory each way, every frame",
            modelW, modelH, mb);
    }

    DropAll();

    _swapchainFormat = swapchainFormat;
    _workFormat = work;
    _blitSwapchain = blit;
    _linearHdr = linearHdr;

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
        MakeImage(_proxy, width, height, VK_FORMAT_R8G8B8A8_UNORM, sampled | storage | src) &&
        MakeImage(_model, modelW, modelH, VK_FORMAT_R8G8B8A8_UNORM, sampled | dst) &&
        MakeImage(_composed, width, height, work, storage | src) &&
        MakeHostBuffer(_download, size_t(modelW) * modelH * 4, dst) &&
        MakeHostBuffer(_upload, size_t(modelW) * modelH * 4, src);

    // The meter is a fixed 64x64 grid whatever the frame is, and is only built when there is
    // something to measure: on a frame the game already tone mapped there is no white point to find,
    // so the dispatch and its readback are skipped entirely rather than run and ignored.
    const bool okMeter =
        !linearHdr ||
        (MakeImage(_meter, kDlssNrMeterGrid, kDlssNrMeterGrid, VK_FORMAT_R32_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
         MakeHostBuffer(_meterBuf, size_t(kDlssNrMeterGrid) * kDlssNrMeterGrid * sizeof(float),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT));

    const bool needWork = (modelW != width || modelH != height);
    const bool okWork = !needWork || MakeImage(_work, modelW, modelH, VK_FORMAT_R8G8B8A8_UNORM,
                                               sampled | storage | src);

    // The averaged answer, and the two filters that get there. Built only when supersampling.
    bool okSuper = true;
    if (superSample) {
        okSuper = MakeImage(_modelNative, width, height, VK_FORMAT_R8G8B8A8_UNORM, sampled | storage);
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

    if (!ok || !okWork || !okMeter || !okSuper) {
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
// Barriers
// ---------------------------------------------------------------------------
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
    if (s.compositionBypass) {
        c.DebugView = 2;
        c.DebugScale = _linearHdr ? ResolvedWhitePoint(s) : 1.0f;
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
    return c;
}

// ---------------------------------------------------------------------------
// Leg 1: the frame the model is shown
// ---------------------------------------------------------------------------
bool Composition::RecordCapture(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s) {
    if (!_usable || !_frame.image) return false;

    // Frame hold, on the edge rather than the level, so the white point is snapshotted once at the
    // moment it comes on rather than re-read every frame it stays on.
    if (s.holdFrame && !_holding) {
        _holding = true;
        _heldWhitePoint = ResolvedWhitePoint(s);
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
    _meterRecorded = false;
    if (_meter.image) {
        Transition(cb, _keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cb, _meter, VK_IMAGE_LAYOUT_GENERAL);

        DlssNrConstants meter = BaseConstants(s);
        meter.Mode = DlssNrMode_Calibrate;
        meter.Width = kDlssNrMeterGrid;
        meter.Height = kDlssNrMeterGrid;
        if (_pass->Dispatch(cb, meter, kDlssNrMeterGrid, kDlssNrMeterGrid, _keep.view, VK_NULL_HANDLE,
                            VK_NULL_HANDLE, VK_NULL_HANDLE, _meter.view, VK_NULL_HANDLE)) {
            Transition(cb, _meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy mr{};
            mr.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            mr.imageExtent = { kDlssNrMeterGrid, kDlssNrMeterGrid, 1 };
            _vk->vkCmdCopyImageToBuffer(cb, _meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        _meterBuf.buffer, 1, &mr);
            _meterRecorded = true;
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
    _vk->vkCmdCopyImageToBuffer(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _download.buffer, 1,
                                &region);
    return true;
}

// ---------------------------------------------------------------------------
// Leg 2: the model's answer, composed back
// ---------------------------------------------------------------------------
bool Composition::RecordCompose(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s) {
    if (!_usable || !_composed.image) return false;

    Transition(cb, _model, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { _modelW, _modelH, 1 };
    _vk->vkCmdCopyBufferToImage(cb, _upload.buffer, _model.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Transition(cb, _model, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // When the model worked above the frame its answer is averaged back to native first, and the
    // composition then sees a native proxy against a native answer -- which is what it should see,
    // because from its point of view the model effectively ran at the frame's own resolution.
    Image* answer = &_model;
    Image* source = _work.image ? &_work : &_proxy;
    if (_superSample) {
        Transition(cb, _modelNative, VK_IMAGE_LAYOUT_GENERAL);
        if (!_superDown->Dispatch(cb, _model.view, _modelNative.view, _modelW, _modelH, _width, _height))
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
    if (!_pass->Dispatch(cb, res, _width, _height, source->view, answer->view, _keep.view, VK_NULL_HANDLE,
                         _composed.view, VK_NULL_HANDLE))
        return false;

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


// A high percentile of tile peaks, not the maximum.
//
// The maximum is a sun or a specular hit and would normalise the whole picture into the dark; the
// mean is scene brightness and says nothing about what scale the buffer is on. The 90th percentile of
// per-tile peaks is high enough to sit at the top of the real range and common enough that no single
// highlight decides it.
//
// The reading is only offered when enough of the frame carries light, measured against its own
// brightest tile rather than an absolute threshold -- the units here are the game's and there is no
// absolute scale. That is what separates "this buffer is scaled by 240" from "I am standing in a dark
// cave": a percentile of tile peaks describes the buffer only when enough of the picture is lit for
// the top of the range to appear in it.
void Composition::ConsumeMeter() {
    if (!_meterRecorded || !_meterBuf.mapped) return;
    _meterRecorded = false;

    const float* src = (const float*) _meterBuf.mapped;
    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);
    for (uint32_t i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i) {
        if (std::isfinite(src[i]) && src[i] > 1e-6f) tiles.push_back(src[i]);
    }
    if (tiles.size() < 16) return;

    const size_t nth = size_t(float(tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());
    const float candidate = tiles[nth];

    float brightest = 0.0f;
    for (float v : tiles) brightest = std::max(brightest, v);
    uint32_t lit = 0;
    for (float v : tiles) {
        if (v > brightest * 0.10f) ++lit;
    }
    const float litFraction = float(lit) / float(tiles.size());

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which is a value the
    // slider can hold -- so a garbage frame could be offered as a real answer. Reject rather than clamp.
    if (!(candidate > 0.0f) || candidate >= 1999.0f) return;
    if (litFraction <= 0.20f) return;

    _meterHistory[_meterCount % kMeterHistory] = candidate;
    ++_meterCount;
    _measuredWhitePoint = std::min(std::max(candidate, 0.25f), 1990.0f);

    // Steadiness is the spread of recent answers, not their size. A number that has held still is one
    // worth taking; one that is swinging means the scene is moving under the measurement and no single
    // value would serve anyway.
    const uint32_t have = std::min(_meterCount, kMeterHistory);
    if (have >= 8) {
        float lo = _meterHistory[0], hi = _meterHistory[0];
        for (uint32_t i = 0; i < have; ++i) {
            lo = std::min(lo, _meterHistory[i]);
            hi = std::max(hi, _meterHistory[i]);
        }
        const float spread = lo > 0.0f ? hi / lo : 2.0f;
        _meterSteadiness = std::min(std::max(1.0f - (spread - 1.0f), 0.0f), 1.0f);
    }
}

}  // namespace dlssnr
