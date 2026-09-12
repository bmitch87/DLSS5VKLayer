#pragma once
// Shared-memory contract between the three processes that make up DLSSNR:
//
//   the Linux Vulkan layer   captures the frame, runs the composition, presents the result
//   the Windows helper       owns nvngx_dlssnr.dll and runs the model
//   the Qt GUI               writes settings and reads status
//
// Everything here is plain atomics in a file mapping, so no side needs the others' toolchain and a
// process dying leaves the others reading a consistent -- if stale -- picture.
//
// Layout of the mapping:
//
//   [0, kHeaderBytes)                       ShmHeader
//   [kHeaderBytes, +kMaxFrame)              the proxy the layer encoded, for the model
//   [kHeaderBytes + kMaxFrame, +kMaxFrame)  the model's answer, for the composition
//
// The proxy is R8G8B8A8_UNORM, display-referred, unless the HDR path is on: then it is
// R16G16B16A16_SFLOAT carrying linear light normalised by the white point, and the model is shown the
// highlights the SDR encode throws away. `proxyFormat` says which one the helper actually built --
// the model has the last word, and the layer encodes to match it. `format` is kept for older helpers
// and is always 1.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

// 'GNR2'. Bumped from the v1 magic on purpose: a stale v1 mapping left in XDG_RUNTIME_DIR must be
// re-initialised rather than half-read, because the header grew and every offset moved.
static constexpr uint32_t kShmMagic = 0x32524E47;
// v14: this branch and upstream both grew the header, so neither side's number describes it.
// From upstream: the pixel regions are eight bytes a pixel for the float16 HDR proxy, the header is
// 64 KiB because VK_EXT_external_memory_host demands the imported pointer meet
// minImportedHostPointerAlignment and NVIDIA answers 64 KiB, and the dma-buf exchange and HDR
// and round-trip attribution. A stale mapping of either lineage must be re-created, not half-read.
static constexpr uint32_t kShmVersion = 19;


static constexpr uint32_t kMaxW = 7680, kMaxH = 4320;
// And a floor. A game that presents a 1x1 probe swapchain -- NWN:EE does, behind its real window --
// used to have the model built at that size, and the first submit hung the GPU channel outright
// (Xid 109, CTX SWITCH TIMEOUT), taking the game down with it. 300x300 is known good; nothing below
// this is a frame worth composing anyway.
static constexpr uint32_t kMinW = 64, kMinH = 64;
// Eight bytes a pixel: the float16 proxy needs them, and the 8-bit path simply uses the first half of
// each region. The mapping is file-backed and sparse, so an SDR session never commits the second half.
static constexpr size_t kMaxFrame = size_t(kMaxW) * kMaxH * 8;
static constexpr size_t kHeaderBytes = 65536;

// The ceiling on how many times the model runs over one frame. It is OptiScaler's
// DlssNr::kMaxPasses and sizes the per-pass arrays below.
static constexpr uint32_t kMaxPasses = 30;

static constexpr size_t kReasonBytes = 192;
static constexpr size_t kNameBytes = 128;

// Which fields a per-pass entry actually overrides. Sparse by design: a pass with no entry, or an
// entry that does not name a field, follows the global setting -- which is what makes "pass 3 is
// gentler" expressible without restating everything else about pass 3.
enum PassOverrideBit : uint32_t {
    kOverrideIntensity = 1u << 0,
    kOverrideLocalStructure = 1u << 1,
    kOverrideLocalTone = 1u << 2,
    kOverrideSkinStructure = 1u << 3,
    kOverrideStyle = 1u << 4,
    kOverridePreset = 1u << 5,
    kOverrideAutoMask = 1u << 6,
    kOverrideSharpness = 1u << 7,
};

// Where the white point comes from. The layer has no game exposure texture to read -- it sees a
// finished swapchain image and nothing else -- so OptiScaler's source 1 has no counterpart here and
// the choice is between the slider and the frame's own measurement.
enum WhitePointSource : uint32_t {
    kWhitePointManual = 0,   // the slider, and nothing else
    kWhitePointMeasured = 1, // the calibration grid, measured off the untouched copy
};

// What the swapchain holds. Getting this wrong encodes an encoded frame a second time, which looks
// washed out and banded -- so the default is to decide it from the format rather than to guess.
enum ColourMode : uint32_t {
    kColourAuto = 0,      // 8-bit: display-referred. 10-bit and float: linear HDR.
    kColourDisplay = 1,   // force display-referred; the encode becomes a pass-through
    kColourLinearHdr = 2, // force linear HDR; the encode scales by the white point and sRGB-encodes
};

// The RenoDX reversible proxy. Which encode the model is shown, and how its answer comes back.
//
//   0  soft knee + our composition            the shipped behaviour, and byte-identical to it
//   1  unclipped Neutwo proxy + composition
//   2  Neutwo proxy + pure-inverse replace    the model's answer straight back, no composition
//   3  hybrid proxy + composition             identity midtones, unclipped highlights
//   4  hybrid proxy + replace
//
// From Dagherbou/OptiScaler_DLSSNR; the proxy itself is RenoDX's (clshortfuse). Modes 2 and 4 are
// noted upstream as flashing on bright lights, which is why 0 is the default rather than a taste.
enum ReversibleMode : uint32_t {
    kReversibleKnee = 0,
    kReversibleNeutwo = 1,
    kReversibleNeutwoReplace = 2,
    kReversibleHybrid = 3,
    kReversibleHybridReplace = 4,
    kReversibleModeCount = 5,
};

// The filter that brings the model's answer back down when it ran above native resolution. Only
// consulted when the working scale is above 1.0.
//
// These are OptiScaler's own Scaler numbers, kept identical so a value copied from an OptiScaler
// profile means the same thing here. FSR1 keeps slot 0 for that reason even though this pass cannot
// use it -- it wants a different constant block, and it is an upscaler rather than the averaging
// filter the down-leg needs. A header asking for it falls back to Lanczos3.
enum Downscaler : uint32_t {
    kDownscaleFsr1 = 0,  // unsupported here; reserved so the numbering matches upstream
    kDownscaleBicubic = 1,
    kDownscaleCatmullRom = 2,
    kDownscaleLanczos2 = 3,
    kDownscaleLanczos3 = 4,  // upstream's default: the sharp one
    kDownscaleKaiser2 = 5,
    kDownscaleKaiser3 = 6,
    kDownscaleMagic = 7,
    kDownscalerCount = 8,
};

// What the helper has managed to do, for the GUI and for the layer's fail-open decision.
// The layer reads this to decide whether anything is listening, so "nobody" has to be the value a
// freshly initialised header holds -- not a state that also means "starting".
enum HelperState : uint32_t {
    kHelperStarting = 0,
    kHelperNoVulkan = 1,   // no NVIDIA device with the NVX extensions
    kHelperNoBinaries = 2, // nvngx_dlssnr.dll not found
    kHelperModelFailed = 3,
    kHelperRunning = 4,
    kHelperStopped = 5,
};

// How the motion field the helper hands the model is scaled. From bmitch87's motion-vector work.
// The HDR input path. The layer detects what the swapchain carries, the user decides whether to use
// it, and the helper's model has the final say on whether a float16 proxy is actually built.
enum HdrMode : uint32_t {
    kHdrAuto = 0,   // HDR when the swapchain is HDR
    kHdrOff = 1,    // always the SDR proxy, whatever the swapchain
    kHdrForce = 2,  // float16 proxy even for an SDR swapchain (an A/B tool, not a preference)
};

enum HdrKind : uint32_t {
    kHdrNone = 0,       // 8-bit swapchain: already tone mapped
    kHdrLinearFp16 = 1, // R16G16B16A16_SFLOAT: linear light, open range
    kHdrPq10 = 2,       // 10-bit with a PQ/BT.2020 colour space: ST 2084 code
};

// What the crossing images actually are, published by the helper: the model decides, and the layer
// encodes to match rather than to hope.
enum ProxyFormat : uint32_t {
    kProxyUnknown = 0,
    kProxyRgba8 = 1,
    kProxyRgba16F = 2,
};

enum MVecScaleMode : uint32_t {
    kMVecNormalized = 0,
    kMVecPixels = 1,
    kMVecUv01 = 2,
};

// What the optical-flow engine is asked for. Higher costs more of the frame's budget.
enum MVecQuality : uint32_t {
    kMVecFast = 0,
    kMVecBalanced = 1,
    kMVecQuality = 2,
};

// Where the mapping lives.
//
// It has to name the same file in every process that touches it, and a Steam game does not share a
// mount namespace with the helper: pressure-vessel gives the container a private tmpfs at
// $XDG_RUNTIME_DIR, so a mapping put there is simply absent inside the game. The layer then creates
// its own empty one at a path that reads identically in the log and waits forever for a helper that
// is answering on the other file -- the "attached ... seq_req=0 / helper not running" case. /tmp is
// bind-mounted from the host into the container, so both sides land on one file; it is also what a
// Wine prefix exposes as Z:\tmp\..., which is how the helper opens it.
inline std::string ShmRuntimeDir() {
    const char* uid = std::getenv("DLSSNR_UID");
    if (uid && *uid) return std::string("/tmp/dlssnr-") + uid;
#ifdef _WIN32
    // The helper is always handed DLSSNR_SHM by the launcher, so this is only ever a last resort.
    return "/tmp/dlssnr";
#else
    return "/tmp/dlssnr-" + std::to_string((unsigned) getuid());
#endif
}

inline std::string ShmDefaultPath() { return ShmRuntimeDir() + "/shm.bin"; }

inline size_t ShmTotalBytes() { return kHeaderBytes + kMaxFrame * 2; }

// One pass's overrides. Every field is present; `overrideMask` says which of them mean anything.
struct PassControl {
    std::atomic<uint32_t> overrideMask;
    std::atomic<uint32_t> intensityBits;
    std::atomic<uint32_t> localToneBits;
    std::atomic<uint32_t> localStructureBits;
    std::atomic<uint32_t> skinStructureBits;
    std::atomic<uint32_t> sharpnessBits;
    std::atomic<uint32_t> style;
    std::atomic<uint32_t> preset;
    std::atomic<uint32_t> autoMask;
};

// A pass's settings after the global values and its own overrides have been merged. Plain floats:
// this is the resolved answer, not shared state.
struct PassTuning {
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;  // -1 follows local structure; it is not a strength of zero
    float sharpness = 0.0f;
    uint32_t style = 0;
    uint32_t preset = 0;
    uint32_t autoMask = 1;

    bool SameCreateParams(const PassTuning& o) const {
        // Everything the model latches when its feature is built. Sharpness is absent because it is
        // read at evaluate, and so is the only one of these a running feature will actually follow.
        return intensity == o.intensity && localTone == o.localTone && localStructure == o.localStructure &&
               skinStructure == o.skinStructure && style == o.style && preset == o.preset && autoMask == o.autoMask;
    }
};

struct ShmHeader {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;

    // The frame handshake. The layer bumps seq_req after writing a proxy; the helper answers by
    // storing the same number into seq_resp once the model's answer is in the output region.
    std::atomic<uint32_t> seq_req;
    std::atomic<uint32_t> seq_resp;
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> format;  // always 1 (RGBA byte order); kept so a v1 helper is not silently wrong
    std::atomic<uint32_t> quit;
    std::atomic<uint32_t> heartbeat;

    // Bumped by whoever writes a setting. The layer and the helper watch it rather than re-reading
    // thirty values every frame.
    std::atomic<uint32_t> controlSeq;

    // Bumped only when something the model latches at feature creation changes. The helper rebuilds
    // its features on this and debounces the rebuild; bumping it every frame exhausts the driver's
    // latches and the model stops responding until the process restarts.
    std::atomic<uint32_t> tuningSeq;

    // --- the model ---------------------------------------------------------------------------
    std::atomic<uint32_t> enabled;
    std::atomic<uint32_t> passes;
    std::atomic<uint32_t> unlockPasses;
    std::atomic<uint32_t> preset;
    std::atomic<uint32_t> style;
    std::atomic<uint32_t> autoMask;
    std::atomic<uint32_t> intensityBits;
    std::atomic<uint32_t> localToneBits;
    std::atomic<uint32_t> localStructureBits;
    std::atomic<uint32_t> skinStructureBits;
    std::atomic<uint32_t> sharpnessBits;

    // --- the composition ---------------------------------------------------------------------
    // How much of the model's edit reaches the frame, and how much of it is allowed to be colour
    // rather than luminance. Separating the two is what keeps saturated highlights from shifting hue.
    std::atomic<uint32_t> transferStrengthBits;
    std::atomic<uint32_t> colourStrengthBits;
    // The most the pass may multiply or divide a pixel by. The transfer is a ratio, and a ratio
    // against a near-black proxy pixel is unbounded without one.
    std::atomic<uint32_t> maxRatioBits;
    // How a model that worked below the frame's size is brought back. 0 classic, 1 matched residual,
    // 2 native + edit -- the frame's own pixels with only the model's difference added, so what the
    // model left alone never passes through the enlargement.
    std::atomic<uint32_t> transfer;
    // 0 off, 1 the picture the model was shown, 2 its raw answer, 3 what it changed, amplified.
    std::atomic<uint32_t> debugView;
    std::atomic<uint32_t> debugScaleBits;
    std::atomic<uint32_t> whitePointBits;
    std::atomic<uint32_t> whitePointScaleBits;
    std::atomic<uint32_t> whitePointSource;
    std::atomic<uint32_t> whitePointTrimBits;
    // What fraction of the frame's resolution the model works at. The frame itself is never reduced:
    // only the model's contribution is computed at this scale and resized, so the picture underneath
    // is untouched whatever this is.
    //
    // Above 1.0 is supersampling -- the model runs above native and its answer is brought back down
    // by scalingDownscaler. Upstream allows up to 2.0. Below 1.0 it also cuts what crosses the shared
    // memory, quadratically, which on this transport matters more than it does upstream.
    std::atomic<uint32_t> workingScaleBits;
    // 0 off, 1 side by side, 2 a wipe.
    std::atomic<uint32_t> compareMode;
    std::atomic<uint32_t> compareSplitBits;
    std::atomic<uint32_t> compareZoomBits;
    std::atomic<uint32_t> compareSwap;
    std::atomic<uint32_t> colourMode;
    // Writes one set of matched before/after frames per session when the layer next presents.
    std::atomic<uint32_t> captureRequest;

    // A Linux KEY_ code the layer watches to toggle the pass, or 0 for none. Unbound by default,
    // because a key that does something unexpected is worse than a key that does nothing.
    //
    // Only useful where the layer can read the keyboard at all: an X11 or XWayland session, inside
    // gamescope, or anywhere the user is in the 'input' group. A game presenting through winewayland
    // is a Wayland client whose keys never reach this process, and keyboards get no uaccess ACL, so
    // there the answer is a desktop shortcut bound to 'dlssnr-shmctl toggle enabled' instead.
    std::atomic<uint32_t> toggleKey;

    // Which proxy the model is shown, and whether its answer is composed or substituted. See
    // ReversibleMode. Default 0 keeps the picture identical to the pre-import behaviour.
    std::atomic<uint32_t> reversibleMode;

    // Whether the model's edit is applied at all. Off keeps the whole pass running -- the capture,
    // the round trip, the encode -- and simply presents the clean frame, which is what makes an
    // honest A/B possible: the cost is unchanged, so only the picture differs.
    std::atomic<uint32_t> applyModel;

    // Freeze the frame the pass works on, so changing a setting re-runs the composition over the
    // SAME picture instead of over whatever the game has drawn since. The only clean way to compare
    // two settings, and a live testing control rather than a saved preference.
    //
    // In this architecture it is cheaper than upstream: the layer already holds the captured proxy
    // and the model's last answer, so holding means not re-capturing rather than keeping a frame
    // alive somewhere it would not otherwise be.
    std::atomic<uint32_t> holdFrame;

    // The filter for the supersampling down-leg. See Downscaler; only read when workingScale > 1.
    std::atomic<uint32_t> scalingDownscaler;

    // --- status, written by the helper --------------------------------------------------------
    std::atomic<uint32_t> helperState;
    std::atomic<uint32_t> modelUp;
    std::atomic<uint32_t> helperFramesLo;
    std::atomic<uint32_t> helperFramesHi;
    std::atomic<uint32_t> helperEvalMsBits;
    std::atomic<uint32_t> helperUploadMsBits;
    std::atomic<uint32_t> helperReadbackMsBits;
    std::atomic<uint32_t> helperVramMB;
    std::atomic<uint32_t> helperFeatures;   // how many NGX features are actually built
    std::atomic<uint32_t> helperPassCeiling;  // what the VRAM budget currently allows

    // --- status, written by the layer ---------------------------------------------------------
    std::atomic<uint32_t> layerAttached;
    std::atomic<uint32_t> layerFramesLo;
    std::atomic<uint32_t> layerFramesHi;
    std::atomic<uint32_t> layerWidth;
    std::atomic<uint32_t> layerHeight;
    std::atomic<uint32_t> layerFormat;
    std::atomic<uint32_t> layerCompositionUp;
    std::atomic<uint32_t> layerMsBits;
    std::atomic<uint32_t> layerMeasuredWhiteBits;
    std::atomic<uint32_t> layerHeartbeat;

    // Free text, each guarded by its own sequence number: bumped after the bytes are written, so a
    // reader that sees an unchanged number is looking at a whole string.
    std::atomic<uint32_t> helperReasonSeq;
    char helperReason[kReasonBytes];
    std::atomic<uint32_t> layerReasonSeq;
    char layerReason[kReasonBytes];
    std::atomic<uint32_t> gameNameSeq;
    char gameName[kNameBytes];

    PassControl pass[kMaxPasses];

    // Appended after the pass array on purpose: everything before it has a pinned offset, and a new
    // field inserted higher up would move all of them. Motion vectors, from bmitch87's work.
    std::atomic<uint32_t> mvecEnabled;
    std::atomic<uint32_t> mvecScaleMode;
    std::atomic<uint32_t> mvecQuality;
    // How far the helper has answered *successfully*. seq_resp says a frame came back; this says it
    // was worth using, so the layer can present the game's own frame when it was not.
    std::atomic<uint32_t> seq_ok;

    // 0: the composition blends the model's edit onto the frame under the strength and guard limits.
    // 1: no composition at all -- the model's raw answer IS the presented frame, and the limits,
    // enlargement and compare overlays are moot. Default 1: the composition is off until the user
    // turns it on.
    std::atomic<uint32_t> compositionBypass;

    // Wall-clock milliseconds the helper waits after the last tuning change before it rebuilds a
    // feature, and between one rebuild and the next. NGX creation is expensive and back-to-back
    // creation was seen to exhaust the driver's latches on some setups, so the default spaces
    // rebuilds rather than firing them at once; 0 means no spacing -- build the moment the change
    // settles and chain the remaining builds back to back. It is time rather than frames because a
    // frame-counted wait crawls on a 30 fps game and races on a 144 fps one.
    std::atomic<uint32_t> rebuildSettleMs;

    // The raster the helper actually answered, echoed before seq_resp. More than one swapchain can
    // share this channel -- a game and the Steam overlay, or a game mid-resize with its old and new
    // swapchains both presenting -- and seq_resp only says *a* frame came back. Without the echo a
    // swapchain waiting on its own request can be satisfied by another's answer and copy the wrong
    // number of bytes, which is the row-shifted colour garbage this field exists to refuse.
    std::atomic<uint32_t> answeredW;
    std::atomic<uint32_t> answeredH;

    // Phase 5: the dma-buf exchange, carried entirely through this header.
    //
    // The helper owns both images that cross the boundary -- the proxy it reads and the answer it
    // writes -- because it is the one process that can hand out a dma-buf descriptor without any
    // help: Wine's ws2_32 has no AF_UNIX and Wine traps direct syscalls, so SCM_RIGHTS is out, but
    // vkGetMemoryFdKHR returns a plain Linux fd number that the helper can simply name here. The
    // layer, native and unrestricted, opens /proc/<pid>/fd/<fd> to take its own reference on the
    // same buffer. That open needs ptrace_mask off (yama ptrace_scope 0) or a descendant
    // relationship; where it fails, the shared-memory transport carries the frame instead.
    //
    // Each export sequence increments when the fd or the image behind it changes; the importer
    // re-opens on a new sequence. The layer's flags say which surfaces it is reading and writing
    // through the fd path, set before seq_req and honoured on exactly that frame.
    std::atomic<uint32_t> proxyExportSeq;  // helper: proxyPid/proxyFd/proxyGen are current
    std::atomic<uint32_t> proxyPid;        // the helper's Linux pid
    std::atomic<uint32_t> proxyFd;         // its fd number for the proxy image
    std::atomic<uint32_t> proxyGen;        // rebuilt (size or channel) since
    std::atomic<uint32_t> answerExportSeq; // helper: answerPid/answerFd/answerGen are current
    std::atomic<uint32_t> answerPid;
    std::atomic<uint32_t> answerFd;
    std::atomic<uint32_t> answerGen;
    // The importer's echo: the export sequence each side has taken a reference at, restated every
    // frame, 0 for none. The helper reads and writes the fd path only while its own current
    // sequence is echoed back -- which also means a restarted helper is not read through a
    // descriptor the layer has not re-opened yet, and a restarted layer is not trusted by the
    // helper's stale memory.
    std::atomic<uint32_t> layerProxySeq;
    std::atomic<uint32_t> layerAnswerSeq;

    // The HDR input path. hdrMode is the user's choice (see HdrMode); hdrDetected and hdrKind are the
    // layer's reading of the primary swapchain's format and colour space; hdrActive is the decision
    // the layer actually encoded this frame under, and proxyFormat is what the helper built the
    // crossing images as. The layer uses the float16 path only while both agree it exists -- if the
    // model refused the float input, proxyFormat stays 8-bit and the encode tone maps as before.
    std::atomic<uint32_t> hdrMode;
    std::atomic<uint32_t> hdrDetected;
    std::atomic<uint32_t> hdrActive;
    std::atomic<uint32_t> proxyFormat;
    // What the proxy bytes in the shared region actually are for the request being made: the layer
    // writes this immediately before seq_req, so the helper reads the width from the same statement
    // that announced the pixels. It lags hdrActive by the frames it takes to switch the crossing
    // images over, and the helper refuses a frame whose width does not match what it built -- which
    // is one presented-as-is frame at a toggle, never a misread one.
    std::atomic<uint32_t> hdrEncode;

    // trip, which is what it has always been; N takes an answer up only on a frame whose count is a
    // multiple of N.
    //
    // round trip does not divide the frame time, so those changes fall at uneven intervals. Each one
    // is a step -- the edit jumps from an old answer warped a long way to a fresh one warped a short
    // way -- and a step at an uneven interval reads as judder where the same step at an even one does
    // not. This pins the interval.
    //
    // What is paced is the send, not the collect: the answer is still taken up the moment it lands,
    // so it is as fresh as the round trip allows and the interval is pinned because each update is
    // the same round trip after an evenly spaced send. Measured on a 4000-frame pan at three passes,
    // the average answer stayed 13 frames old at every stride from 0 to 32 -- the cadence costs no
    // freshness at all, which is not what was assumed when this was written.
    //
    // It is also cheaper. Sending less often is less work for the helper, and the frames it stops
    // doing come back as frame rate: 577 fps at 0, 596 at 16, 675 at 24, 729 at 32 on that same run.
    //
    // A stride shorter than the round trip cannot be honoured and is not faked -- the send waits for
    // the next multiple at which the helper is free, so the cadence stays a multiple of N rather than
    // drifting off it.

    // How hard the measured displacement is filtered over time, in hundredths. 0 applies the estimate
    // exactly as measured, which is what happened before this existed; 100 is the full filter.
    //
    // The estimate scatters by a few pixels from frame to frame however it is tuned -- the search can
    // only name a cell, and the gradient solve refines within one rather than removing the cell-to-
    // cell instability. The edit is warped by that number, so the scatter shows up as the whole
    // picture shaking a different way each frame. Neither of the two obvious culprits was it: halving
    // the staleness did not reduce the scatter, and pinning the cadence the answers arrive on did not
    // either.
    //
    // the standard answer. It is applied to the velocity rather than to the displacement, because the
    // displacement steps every time a new answer moves the reference and only the velocity is
    // continuous across that. See PASS_SMOOTH in globalmotion.comp.
    //
    // Measured on a 2 px/frame pan, the frame-to-frame scatter in the estimate falls from 1.70 px to
    // 0.37 with this at 100, and the estimate tracks the true speed instead of stepping 0, 1, 3, 4
    // pixels at a time to average it.

    // How much of the chroma-agreement gate to apply, in hundredths. 100 is the gate as written; 0
    // switches it off and takes the model's colour everywhere.
    //
    // The gate exists because the model's colour disagrees with the frame's most at edges -- an edge
    // being precisely what it was asked to re-decide -- and taking that hue whole put one colour on
    // one side of an edge and its complement on the other. Measured, the pass moved colour balance
    // three to five times more at edges than on flat pixels.
    //
    // The cost of it is that where the gate closes, the composed pixel falls back to the frame's own
    // colour scaled by one luminance ratio -- so on a detailed frame the model's chroma detail is
    // dropped exactly where the model had most to say. That is a real part of "composition only ever
    // removes detail", and it was never separable from the colour strength control, because the gate
    // multiplies that control rather than being one.
    std::atomic<uint32_t> colourTrustPercent;

    // How much of the relighting ratio is taken from the pixel's neighbourhood instead of the pixel,
    // in hundredths. 0 is the behaviour that shipped before it existed.
    //
    // The composition rebuilds the frame as its own pixel times one per-pixel number. On detailed
    // content that number varies sharply, because the model's answer differs sharply there, and the
    // highlight guard is all that holds it -- so raising the guard lets the variation through as
    // blown and black pixels wearing whatever colour the texture had. Carrying a ratio at full
    // spatial frequency is the mistake: what the model knows at this scale is how much light belongs
    // here, not which pixel is brighter than its neighbour, and the frame already knows that.
    std::atomic<uint32_t> ratioSmoothPercent;

    // SDR uses 8-bit ping-pong images by default. Enable 16-bit UNORM to avoid quantising between
    // passes at higher memory and bandwidth cost.
    std::atomic<uint32_t> sdr16Multipass;

};

static_assert(sizeof(ShmHeader) <= kHeaderBytes, "ShmHeader outgrew its region");

// The layout, pinned.
//
// Every process that maps this file agrees on where each field is only because they were compiled
// from the same header. A field inserted anywhere but the end silently moves everything after it, and
// a build that has not caught up then reads its neighbour's value -- which is not a crash, it is a
// status display quietly reporting 4861 for a flag that is 0 or 1, and it took a nonsensical number
// on screen to notice.
//
// The version check already existed to prevent exactly that; what was missing was anything to make
// someone remember to use it. If these fire, the layout changed: bump kShmVersion in the same commit,
// then update these numbers.
static_assert(sizeof(ShmHeader) == 1964, "the header layout changed -- bump kShmVersion");

static_assert(offsetof(ShmHeader, enabled) == 44, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, transferStrengthBits) == 88, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, helperState) == 176, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, pass) == 780, "layout changed -- bump kShmVersion");
static_assert(offsetof(ShmHeader, mvecEnabled) == 1860, "layout changed -- bump kShmVersion");

inline uint32_t FloatToBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline float BitsToFloat(uint32_t u) {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline void ShmStoreString(std::atomic<uint32_t>& seq, char* dst, size_t cap, const char* src) {
    std::memset(dst, 0, cap);
    if (src) std::strncpy(dst, src, cap - 1);
    seq.fetch_add(1);
}

inline std::string ShmLoadString(const std::atomic<uint32_t>& seq, const char* src, size_t cap) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = seq.load();
        char buf[kReasonBytes > kNameBytes ? kReasonBytes : kNameBytes];
        std::memset(buf, 0, sizeof(buf));
        std::memcpy(buf, src, cap < sizeof(buf) ? cap : sizeof(buf));
        buf[(cap < sizeof(buf) ? cap : sizeof(buf)) - 1] = '\0';
        if (seq.load() == before) return std::string(buf);
    }
    return std::string();
}

inline void ShmInitDefaults(ShmHeader* h) {
    std::memset(static_cast<void*>(h), 0, sizeof(ShmHeader));
    h->magic.store(kShmMagic);
    h->version.store(kShmVersion);
    h->helperState.store(kHelperStopped);
    h->format.store(1);
    h->passes.store(1);
    h->enabled.store(1);
    h->autoMask.store(1);
    h->intensityBits.store(FloatToBits(1.0f));
    h->localToneBits.store(FloatToBits(1.0f));
    h->localStructureBits.store(FloatToBits(1.0f));
    h->skinStructureBits.store(FloatToBits(-1.0f));
    h->sharpnessBits.store(FloatToBits(0.0f));

    h->transferStrengthBits.store(FloatToBits(1.0f));
    h->colourStrengthBits.store(FloatToBits(1.0f));
    h->maxRatioBits.store(FloatToBits(2.0f));
    h->transfer.store(1);
    h->debugScaleBits.store(FloatToBits(1.0f));
    h->whitePointBits.store(FloatToBits(1.0f));
    h->whitePointScaleBits.store(FloatToBits(1.0f));
    h->whitePointTrimBits.store(FloatToBits(1.0f));
    h->whitePointSource.store(kWhitePointManual);
    h->workingScaleBits.store(FloatToBits(1.0f));
    h->compareSplitBits.store(FloatToBits(0.5f));
    h->compareZoomBits.store(FloatToBits(1.0f));
    h->colourMode.store(kColourAuto);
    h->reversibleMode.store(kReversibleKnee);
    h->applyModel.store(1);
    h->holdFrame.store(0);
    h->scalingDownscaler.store(kDownscaleLanczos3);

    h->hdrMode.store(kHdrAuto);
    h->hdrDetected.store(kHdrNone);
    h->hdrActive.store(0);
    h->proxyFormat.store(kProxyRgba8);

    h->mvecEnabled.store(1);
    h->mvecScaleMode.store(kMVecPixels);
    h->mvecQuality.store(kMVecBalanced);
    h->seq_ok.store(0);
    h->compositionBypass.store(1);
    h->rebuildSettleMs.store(250);
    h->colourTrustPercent.store(200);

    h->ratioSmoothPercent.store(100);
    h->sdr16Multipass.store(0);


    for (uint32_t i = 0; i < kMaxPasses; ++i) {
        h->pass[i].overrideMask.store(0);
        h->pass[i].intensityBits.store(FloatToBits(1.0f));
        h->pass[i].localToneBits.store(FloatToBits(1.0f));
        h->pass[i].localStructureBits.store(FloatToBits(1.0f));
        h->pass[i].skinStructureBits.store(FloatToBits(-1.0f));
        h->pass[i].sharpnessBits.store(FloatToBits(0.0f));
        // Inert until overrideMask names them, but initialised to the global defaults so a pass that
        // is switched on later starts from what the rest of the frame is already doing.
        h->pass[i].style.store(0);
        h->pass[i].preset.store(0);
        h->pass[i].autoMask.store(1);
    }
}

inline uint32_t ShmPassCeiling(const ShmHeader* h) {
    (void)h;
    return kMaxPasses;
}

inline uint32_t ShmPasses(const ShmHeader* h) {
    uint32_t p = h->passes.load();
    const uint32_t ceiling = ShmPassCeiling(h);
    if (p == 0) return 1;
    return p > ceiling ? ceiling : p;
}

inline bool ShmNeuralEnabled(const ShmHeader* h) { return h->enabled.load() != 0; }

// The global settings with one pass's overrides applied. A field the pass does not name follows the
// global value, which is what keeps a sparse override sparse.
inline PassTuning ShmResolvePass(const ShmHeader* h, uint32_t pass) {
    PassTuning t;
    t.intensity = BitsToFloat(h->intensityBits.load());
    t.localTone = BitsToFloat(h->localToneBits.load());
    t.localStructure = BitsToFloat(h->localStructureBits.load());
    t.skinStructure = BitsToFloat(h->skinStructureBits.load());
    t.sharpness = BitsToFloat(h->sharpnessBits.load());
    t.style = h->style.load();
    t.preset = h->preset.load();
    t.autoMask = h->autoMask.load();

    if (pass >= kMaxPasses) return t;
    const uint32_t mask = h->pass[pass].overrideMask.load();
    if (mask == 0) return t;
    if (mask & kOverrideIntensity) t.intensity = BitsToFloat(h->pass[pass].intensityBits.load());
    if (mask & kOverrideLocalTone) t.localTone = BitsToFloat(h->pass[pass].localToneBits.load());
    if (mask & kOverrideLocalStructure) t.localStructure = BitsToFloat(h->pass[pass].localStructureBits.load());
    if (mask & kOverrideSkinStructure) t.skinStructure = BitsToFloat(h->pass[pass].skinStructureBits.load());
    if (mask & kOverrideSharpness) t.sharpness = BitsToFloat(h->pass[pass].sharpnessBits.load());
    if (mask & kOverrideStyle) t.style = h->pass[pass].style.load();
    if (mask & kOverridePreset) t.preset = h->pass[pass].preset.load();
    if (mask & kOverrideAutoMask) t.autoMask = h->pass[pass].autoMask.load();
    return t;
}

inline uint64_t ShmLoad64(const std::atomic<uint32_t>& lo, const std::atomic<uint32_t>& hi) {
    return (uint64_t(hi.load()) << 32) | uint64_t(lo.load());
}

inline void ShmStore64(std::atomic<uint32_t>& lo, std::atomic<uint32_t>& hi, uint64_t v) {
    hi.store(uint32_t(v >> 32));
    lo.store(uint32_t(v & 0xFFFFFFFFu));
}

inline bool ShmMVecEnabled(const ShmHeader* h) { return h->mvecEnabled.load() != 0; }

inline uint32_t ShmMVecScaleMode(const ShmHeader* h) {
    const uint32_t m = h->mvecScaleMode.load();
    return m <= kMVecUv01 ? m : kMVecNormalized;
}

inline uint32_t ShmMVecQuality(const ShmHeader* h) {
    const uint32_t q = h->mvecQuality.load();
    return q <= kMVecQuality ? q : kMVecBalanced;
}
