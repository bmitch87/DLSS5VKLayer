#pragma once
// The DLSS-NR pass, arranged for a present-time layer.
//
// OptiScaler runs this immediately after the game's upscaler, on surfaces it already holds. Here the
// only thing available is a finished swapchain image, and the model lives in another process behind a
// shared-memory round trip, so the same pass has to be split in two around that round trip:
//
//   leg 1   swapchain -> frame -> ENCODE -> proxy (+ the untouched keep)
//                              -> DOWNSAMPLE -> work, when the model runs below the frame
//                              -> host buffer
//     ...   the helper runs the model on those pixels and answers
//   leg 2   host buffer -> model image -> RESOLVE -> composed -> swapchain
//
// What crosses the shared memory is the proxy, not the frame: it is display-referred and 8-bit by
// construction, so the helper never has to know what format the game presents in, and the working
// scale reduces it quadratically.
//
// The composition itself -- what the resolve does with the model's answer -- is entirely the vendored
// shader's. Everything in this file is plumbing: which image is bound where, in what layout, and what
// goes in the constant block.
#include "capture.h"
#include "dlssnr_pass.h"
#include "scaler_vk.h"
#include "vk_table.h"

#include "../../common/shm_protocol.h"

#include <memory>
#include <string>

namespace dlssnr {

// One frame's worth of settings, read from the shared header once so that a control changed
// mid-frame cannot make the encode and the resolve disagree about what they are doing.
struct FrameSettings {
    float transferStrength = 1.0f;
    float colourStrength = 1.0f;
    float maxRatio = 2.0f;
    float debugScale = 1.0f;
    // The white point is three numbers, not one: where it comes from, the multiplier that says what
    // the model should consider white, and the trim that belongs to a measured reading rather than to
    // the slider. Keeping them apart is upstream's fix for a real bug -- sharing one stored value
    // meant touching the slider in one mode silently destroyed the number found in the other.
    float whitePointManual = 1.0f;
    float whitePointScale = 1.0f;
    float whitePointTrim = 1.0f;
    uint32_t whitePointSource = kWhitePointManual;
    float compareSplit = 0.5f;
    float compareZoom = 1.0f;
    float workingScale = 1.0f;
    uint32_t transfer = 1;
    uint32_t debugView = 0;
    uint32_t compareMode = 0;
    uint32_t compareSwap = 0;
    uint32_t reversibleMode = kReversibleKnee;
    uint32_t applyModel = 1;
    uint32_t holdFrame = 0;
    uint32_t downscaler = kScalerLanczos3;
    // 1: present the model's raw answer as the frame -- no blend, no guard, no compare.
    uint32_t compositionBypass = 0;

    // How much of the way toward a newly arrived answer the running pair moves each frame, 0..1.

    float ghostSlack = 0.5f;

    // Radius splitting the stale edit's safe half from the half that can ghost, in uv.
    float editBlur = 0.04f;

    float motionSmooth = 1.0f;

    // How much of the chroma-agreement gate to apply. See colourTrustPercent.
    float colourTrust = 1.0f;

    // How much of the relighting ratio comes from the neighbourhood. See ratioSmoothPercent.
    float ratioSmooth = 0.0f;


    static FrameSettings Read(const ShmHeader* h);
};

// Whether a swapchain format can be composed at all, and what this pass works in when it can.
//
// Every internal surface uses the format's UNORM twin rather than the swapchain's own: sampling a
// _SRGB view would decode to linear on the way in and re-encode on the way out, and the composition
// wants exactly the display-referred numbers the game already wrote. Copies between the two are
// byte-for-byte, which is why the final result goes back with vkCmdCopyImage and not a blit -- a blit
// into an _SRGB image would apply the encode a second time.
VkFormat CompositionFormat(VkFormat swapchainFormat);

// Whether the frame the game presents holds light or a picture. 8-bit and 10-bit formats are already
// display-referred -- the game tone mapped before it got here -- and only a float swapchain is linear.
bool ColourIsLinearHdr(VkFormat swapchainFormat, uint32_t colourMode);

class Composition {
  public:
    Composition(const DeviceTable* vk, const InstanceTable* instance, VkDevice device,
                VkPhysicalDevice physicalDevice);
    ~Composition();

    Composition(const Composition&) = delete;
    Composition& operator=(const Composition&) = delete;

    bool Usable() const { return _usable; }

    // Why not, when not. Empty while it is working.
    const char* Reason() const { return _reason.c_str(); }

    // Build or rebuild everything sized to this frame and this model resolution. Cheap and a no-op
    // when nothing has changed, so it is safe to call every present.
    // hdrProxy asks for the float16 HDR proxy: the crossing surfaces become R16G16B16A16_SFLOAT and
    // the encode writes normalised linear light instead of an sRGB picture. hdrTransfer = 1 says the
    // swapchain itself carries PQ. Both are honoured only while the device can hold a float16 proxy;
    // Prepare reports the decision it actually made through HdrProxyActive().
    bool Prepare(uint32_t width, uint32_t height, VkFormat swapchainFormat, const FrameSettings& s,
                 bool linearHdr, bool hdrProxy = false, uint32_t hdrTransfer = 0);

    // The raster the model will work at, for a caller that has to size a transport before Prepare
    // runs. Same arithmetic Prepare uses, in one place so the two cannot disagree.
    static void ModelExtent(uint32_t width, uint32_t height, const FrameSettings& s,
                            uint32_t& modelW, uint32_t& modelH);

    uint32_t ModelWidth() const { return _modelW; }
    uint32_t ModelHeight() const { return _modelH; }
    // Eight bytes a pixel while the float16 proxy is on, four when it is not -- every transport size
    // in the layer derives from this one number so the two sides cannot disagree.
    size_t ModelBytes() const { return size_t(_modelW) * _modelH * (_hdrProxy ? 8 : 4); }
    bool HdrProxyActive() const { return _hdrProxy; }
    uint32_t HdrTransfer() const { return _hdrProxy ? _hdrTransfer : 0; }

    // Point the transport buffers at the shared-memory pixel regions so the GPU reads and writes
    // them directly, with no host copy in between. Null pointers fall back to private host-visible
    // staging. Safe to call every present; buffers are rebuilt only when the mapping changes, and
    // only between frames (the caller must have waited for the previous frame's work).
    void SetTransport(void* inRegion, void* outRegion, size_t bytes);

    // Leg 1. Leaves the proxy the model should see in the download buffer, and the swapchain image
    // back in PRESENT_SRC_KHR so a caller that gives up after this still presents something valid.
    bool RecordCapture(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s);

    // The pixels leg 1 produced, and where the model's answer goes before leg 2.
    const void* ProxyPixels() const { return _download.mapped; }
    void* ModelPixels() { return _upload.mapped; }

    // Leg 2. Composes and leaves the swapchain image holding the result, in PRESENT_SRC_KHR.
    bool RecordCompose(VkCommandBuffer cb, VkImage swapchainImage, const FrameSettings& s);

    // ---------------------------------------------------------------------
    // Phase 5: dma-buf transport. When both sides can, the proxy and the answer are device memory
    // exported as dma-bufs and adopted through /proc -- the pixels never touch host
    // pages at all. Every step is optional: with no channel, no extension, or a failed import, the
    // shared-memory transport above carries the frame exactly as before.
    // ---------------------------------------------------------------------

    // Adopt one of the helper's exported images. The fd is consumed (or closed). Returns false if
    // it cannot be imported at this raster; the shared-memory path then carries that direction.
    bool ImportProxy(int fd, uint32_t w, uint32_t h);
    bool ImportAnswerFd(int fd, uint32_t w, uint32_t h);
    // True when the imported answer covers the current model raster, so leg 2 can read it directly.
    bool AnswerImported() const {
        return _answerXfer.image != VK_NULL_HANDLE && _answerXfer.width == _modelW &&
               _answerXfer.height == _modelH;
    }
    void DropXfer();

    // The channel's readiness, restated every present: the exportable proxy is only built while it
    // is on, and leg 1 only writes it while it is on. Turning it off mid-run falls back to the
    // shared-memory transport on the next frame.
    void SetDmaBuf(bool on) { _dmaBuf = on; }
    bool DmaBuf() const { return _dmaBuf; }

    // True when leg 1 writes the proxy into the imported image this frame, so the caller must not
    // also copy it into the shared-memory region -- and the helper reads it through the fd.
    bool ProxyActive() const {
        return _dmaBuf && _proxyXfer.image != VK_NULL_HANDLE && _proxyXfer.width == _modelW &&
               _proxyXfer.height == _modelH;
    }
    void SetAnswerViaFd(bool on) { _answerViaFd = on; }
    bool AnswerViaFd() const { return _dmaBuf && _answerViaFd && AnswerImported(); }
    // The channel went away: forget both imported surfaces so the next frame falls back whole.
    void InvalidateXfer() { DropXfer(); _answerViaFd = false; }

    bool HasModelFrame() const { return _haveModel; }
    void MarkModelFrame() { _haveModel = true; }

    // Write this many matched before/after pairs, starting with the next composed frame.
    void RequestCapture(uint32_t frames) { _capture.Begin(frames); }
    bool CaptureActive() const { return _capture.Active(); }

    // Called after leg 2's fence, when the readback the compose recorded has landed.
    void WriteCapturedFrame();

    // Called after leg 1's fence: turns the tile grid the meter wrote into a white point.
    void ConsumeMeter();

    // What the meter settled on, or 0 when it has not taken a usable reading. For the interface, so
    // the number in use is visible rather than inferred.
    float MeasuredWhitePoint() const { return _measuredWhitePoint; }

  private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t size = 0;
        // Non-null when the allocation is the shared-memory region itself (an imported host
        // pointer), rather than private pinned memory the transport is copied through.
        void* hostPtr = nullptr;
    };

    bool MakeImage(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage);
    void DropImage(Image& img);
    bool ImportFdMemory(int fd, const VkMemoryRequirements& req, VkDeviceMemory* out);
    bool MakeHostBuffer(HostBuffer& buf, size_t bytes, VkBufferUsageFlags usage);
    bool MakeTransportBuffer(HostBuffer& buf, size_t bytes, VkBufferUsageFlags usage, void* hostPtr);
    void DropHostBuffer(HostBuffer& buf);
    void EnsureTransport();
    void DropAll();

    bool BuildMeterPipeline();
    void DropMeterObjects();
    bool BuildMeterDescriptors();
    bool MakeMeterState();
    void DropMeterState();

    void Transition(VkCommandBuffer cb, Image& img, VkImageLayout to);
    void TransitionSwapchain(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to);
    bool FormatSupportsBlit(VkFormat format) const;
    void CopyWholeImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
                        VkImageLayout dstLayout, uint32_t w, uint32_t h);

    bool FormatSupportsStorage(VkFormat format) const;
    DlssNrConstants BaseConstants(const FrameSettings& s) const;
    float ResolvedWhitePoint(const FrameSettings& s) const;

    const DeviceTable* _vk = nullptr;
    const InstanceTable* _instance = nullptr;
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;

    std::unique_ptr<DlssNrPass> _pass;
    bool _usable = false;
    std::string _reason;

    uint32_t _width = 0, _height = 0;
    uint32_t _modelW = 0, _modelH = 0;
    VkFormat _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkFormat _workFormat = VK_FORMAT_UNDEFINED;  // usually the swapchain's UNORM twin; see Prepare
    bool _blitSwapchain = false;                 // set when _workFormat is not the swapchain's twin
    VkFormat _keepFormat = VK_FORMAT_UNDEFINED;
    bool _linearHdr = false;
    bool _hdrProxy = false;
    uint32_t _hdrTransfer = 0;
    bool _haveModel = false;

    Image _frame{}, _keep{}, _proxy{}, _work{}, _model{}, _composed{};

    // Supersampling: the model works above the frame, so the proxy is enlarged on the way in and the
    // answer averaged back on the way out. _modelNative holds that average; without it the resolve
    // would read the larger answer through a bilinear sampler and alias, which is the reason upstream
    // gave this its own filter rather than reusing the resolve's.
    Image _modelNative{};
    std::unique_ptr<ScalerVk> _superUp, _superDown;
    bool _superSample = false;
    uint32_t _scalerFilter = kScalerLanczos3;

    // The white point meter: a grid of tile peak luminances measured off the untouched copy, and the
    // percentile taken across it -- on the GPU. The reduce pass keeps the percentile, the history
    // and the resolved value in a device-local state buffer; the resolve reads the resolved value
    // through a four-byte copy into its own constant block, so no tensor crosses to the host. The
    // mirror is a 128-byte copy of the state the CPU reads after leg 1's fence, for the frame-hold
    // snapshot and the status field only. See ConsumeMeter.
    Image _meter{};
    bool _meterGpu = false;
    bool _meterStateCleared = false;
    VkBuffer _meterState = VK_NULL_HANDLE;
    VkDeviceMemory _meterStateMemory = VK_NULL_HANDLE;
    HostBuffer _meterMirror{};
    VkPipeline _meterPipeline = VK_NULL_HANDLE;
    VkPipelineLayout _meterPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _meterDescriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool _meterDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _meterDescriptorSet = VK_NULL_HANDLE;
    VkSampler _meterSampler = VK_NULL_HANDLE;
    float _measuredWhitePoint = 0.0f;
    float _meterSteadiness = 0.0f;
    HostBuffer _download{}, _upload{}, _captureBuf{};

    // The shared-memory regions the transport buffers alias, when they alias them at all.
    void* _transportIn = nullptr;
    void* _transportOut = nullptr;
    size_t _transportBytes = 0;

    // Phase 5. _proxyXfer is the proxy at the model's raster in exportable device memory;
    // _answerXfer is the helper's answer, imported from its dma-buf. Both are null when the
    // channel is not up, and the host transport above carries the frame.
    Image _proxyXfer{}, _answerXfer{};
    bool _dmaBuf = false;
    bool _answerViaFd = false;

    CaptureWriter _capture;
    bool _captureRecorded = false;

    // Frame hold. The freeze point is the raw colour the encode reads, not the proxy: both the proxy
    // and the untouched keep are derived from it, and freezing further down would stop a setting
    // change from re-encoding, which is the whole point of holding.
    bool _holding = false;
    bool _frameCaptured = false;
    float _heldWhitePoint = 1.0f;
};

}  // namespace dlssnr
