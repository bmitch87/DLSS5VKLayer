#pragma once
#include "ngx_abi.h"
#include <windows.h>
#include <string>
#include <vulkan/vulkan.h>
#include "../common/shm_protocol.h"

namespace dlssnr {

// Owns the nvngx_dlssnr.dll snippet lifecycle: caller-identity IAT spoof,
// core (nvngx.dll) init, parameter block, Feature 18 create/evaluate/teardown.
// All NGX calls are SEH-guarded; any failure latches `disabled` (pass-through).
struct NgxSnippet {
    HMODULE snippet = nullptr;
    HMODULE core = nullptr;
    HMODULE nvapi = nullptr;

    FnVkInitExt initExt = nullptr;
    FnVkInitExt initExt2 = nullptr;
    FnVkInitExt initPlain = nullptr;
    FnVkCreateFeature createFeature = nullptr;
    FnVkEvaluateFeature evaluateFeature = nullptr;
    FnVkReleaseFeature releaseFeature = nullptr;
    FnVkShutdown1 shutdown1 = nullptr;

    NVSDK_NGX_Parameter* params = nullptr;
    FnVkDestroyParameters paramsDestroy = nullptr;
    bool ownParams = false;
    // One NGX feature per pass, not one feature evaluated repeatedly.
    //
    // A feature carries its own temporal history, and the passes are meant to be a chain of distinct
    // models over one frame rather than the same model shown its own output. Sharing a handle meant
    // every pass in a frame wrote into one history, which is not what "run the model N times" means.
    NVSDK_NGX_Handle* features[kMaxPasses] = {};
    uint32_t featureCount = 0;
    uint32_t featureW = 0, featureH = 0;

    bool ready = false;     // snippet init + CreateFeature(18) succeeded
    // The MODULE is loaded, its exports are resolved and NVSDK_NGX_VULKAN_Init has succeeded.
    //
    // Separate from `ready`, which means "a feature exists" and is cleared by
    // NgxReleaseAllPasses. Those are two different facts and conflating them is what made a raster
    // change redo the whole process-level load and init; see the guard at the top of
    // NgxLoadAndInit. Cleared only by the teardown, which is the one place the module really does
    // go away.
    bool initialised = false;
    bool disabled = false;  // latched failure -> pass-through forever

    // Captured from NVSDK_NGX_VULKAN_GetFeatureRequirements (bit 0 = HDR path).
    unsigned int featureFlags = 0;
    bool hdrCapable = false;
    // Whether the feature is being built as HDR. The helper sets it before the first create and on
    // every switch; NgxCreatePass rewrites the create flags and the tonemap hint from it, so a
    // rebuild after a toggle carries the new contract without re-running init.
    bool hdrActive = false;

    // Last values the evaluate contract reported, so it says something only when it changes.
    unsigned int loggedAutoMask = 0xFFFFFFFFu;
    unsigned int loggedUiCorrection = 0xFFFFFFFFu;
    float loggedMVecScaleX = -1.0f, loggedMVecScaleY = -1.0f;
    bool loggedDepthBound = false;

    // Caller-identity spoof state
    void** iatSlot = nullptr;
    decltype(&GetModuleFileNameW) originalGetModuleFileNameW = nullptr;

    // Resource structs passed to the DLL (must outlive eval calls)
    NVSDK_NGX_Resource_VK resColor{}, resOut{}, resMV{}, resDepth{}, resControlMask{};

    std::wstring binDir;
};

// Layer module handle (set in DllMain), used as the spoofed caller identity.
extern HMODULE g_layerModule;


// The model's tuning. This comment used to say that all of it is latched when a feature is
// built and that writing any of it at evaluate does nothing at all. That was wrong, and it
// was stated as settled fact, so a lot of machinery was built on it.
//
// Measured, with the parameter container instrumented over a live session
// (DLSSNR_FORCE_OWNPARAM=1, OwnParam::DumpUnread): only the PRESET is read at create.
// Intensity, localTone, localStructure, skinStructure, style and autoMask are read at every
// EVALUATE. They are written per pass by NgxSetEvaluateTuning; NgxSetCreateTuning still
// writes the lot before a create, which is harmless and keeps the preset correct.
//
// What still depends on a rebuild is therefore the preset alone -- and that now has a picture
// behind it, not only the probe. A key being READ at evaluate was never proof the model ACTS
// on it there, so it was measured: see SameCreateParams below for the numbers and the method.
// MaintainPasses, tuningSeq, the GUI's latch split and both SameCreateParams have since been
// cut down to the preset, and a live retune raises the model's history reset for one frame
// instead of rebuilding its feature.
struct NgxTuning {
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
    uint32_t style = 0;
    uint32_t preset = 0;
    uint32_t autoMask = 1;
    // DLSSNR.UICorrection. Read by the DLL at evaluate and never at create, so it is deliberately
    // absent from the comparison below: changing it must not provoke a rebuild, because the running
    // feature already follows it.
    uint32_t uiCorrection = 0;

    bool operator==(const NgxTuning& o) const {
        return intensity == o.intensity && localTone == o.localTone &&
               localStructure == o.localStructure && skinStructure == o.skinStructure &&
               style == o.style && preset == o.preset && autoMask == o.autoMask;
    }
    bool operator!=(const NgxTuning& o) const { return !(*this == o); }

    // What actually costs a feature rebuild: the preset, and nothing else.
    //
    // MEASURED, on this model, with the rebuild debounce pushed to ten minutes so that every value
    // below was evaluated by one feature built once and never rebuilt (zero rebuilds, zero creates
    // in the helper log across every run). Two measuring runs per value, and every value revisited:
    //
    //   intensity        0.0 -> 591588/591588   1.0 -> 591926/591846   4.0 -> 592134/592165
    //   localTone        0.0 -> 591736/591687   1.0 -> 591953/592174   4.0 -> 601239/601432
    //   localStructure   0.0 -> 592151/592151   1.0 -> 591695/591757   4.0 -> 595033/595230
    //   skinStructure      0 -> 592083/591903     2 -> 591847/592184     4 -> 592537/592653
    //   style              0 -> 592653/592625     1 -> 571481/571328     2 -> 589679/589736
    //   autoMask           0 -> 591948/591953     1 -> 592653/592625
    //   preset       0, 3, 7 and 15 all inside the run-to-run spread -- not live, and separately
    //                measured not to change the picture through a rebuild either.
    //
    // Each value reproduces its own pair on a later visit, so this is separation rather than noise;
    // localTone at 4.0 moves the digest by 1.6% and style by 3.5%. Six of the seven are read at
    // evaluate and act at evaluate, which is what the parameter probe said and is now confirmed in
    // the picture.
    //
    // So a change to any of the six is followed by the feature that is already running, and
    // rebuilding for it bought a stutter and nothing else. See ns.passDirty.
    bool SameCreateParams(const NgxTuning& o) const { return preset == o.preset; }
};

// Writes the create-time block. Must be called before NgxCreatePass, never instead of it.
void NgxSetCreateTuning(NgxSnippet& s, const NgxTuning& t);
void NgxSetHdr(NgxSnippet& s, bool want);

// Loads the snippet, initialises it, and builds pass 0.
//
// The tuning is passed in rather than set by the caller beforehand because this function writes its
// own create contract -- preset among it -- and would otherwise overwrite whatever the caller had
// just chosen. It is applied last, immediately before the feature is built.
bool NgxLoadAndInit(NgxSnippet& s, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                    uint32_t width, uint32_t height, VkCommandBuffer recordingCmd,
                    const NgxTuning& tuning);

bool NgxCreatePass(NgxSnippet& s, uint32_t pass, uint32_t width, uint32_t height,
                   VkCommandBuffer recordingCmd);
void NgxReleasePass(NgxSnippet& s, uint32_t pass, VkDevice device);
void NgxReleaseAllPasses(NgxSnippet& s, VkDevice device);
// mvecWidth/mvecHeight are the region of the motion field that actually holds measurements.
// They are normally the full frame; they are not when the field was produced at a reduced
// size, when a grid did not divide the raster evenly, or when a confidence gate has zeroed
// part of it. Zero means "the field carries nothing", which is a clearer statement to the
// model than a full rectangle of zeros -- and the DLL infers no subrect from a resource, so
// whatever is not said here stays at its own default.
void NgxSetResources(NgxSnippet& s, const NVSDK_NGX_Resource_VK& color,
                     const NVSDK_NGX_Resource_VK& out, const NVSDK_NGX_Resource_VK& mv,
                     const NVSDK_NGX_Resource_VK& depth, uint32_t width, uint32_t height,
                     uint32_t mvecWidth, uint32_t mvecHeight,
                     // DLSSNR.ControlMask, for the probe only; null in every ordinary frame.
                     const NVSDK_NGX_Resource_VK* controlMask = nullptr);
void NgxSetReset(NgxSnippet& s, bool reset, bool logValue = false);
// A no-op that logs once. This model has no sharpness parameter under any name -- see the
// definition. It was believed to be "the one strength the model reads at evaluate"; it is
// the one strength the model does not have.
void NgxSetSharpness(NgxSnippet& s, float sharpness);
// The minimum driver version the loaded model declares in its own version resource, or 0
// when it did not say. Read from the binary rather than from a table, so it stays correct
// when the model is swapped.
extern double g_modelMinDriver;
// The six the model reads at EVERY evaluate, written per pass from that pass's own resolved
// tuning. Only the preset is latched at create; see NgxTuning.
void NgxSetEvaluateTuning(NgxSnippet& s, const NgxTuning& t);
// How the motion field's units are read. Written every evaluate, because it goes with the field.
void NgxSetMotionScale(NgxSnippet& s, float scaleX, float scaleY);
bool NgxEvaluatePass(NgxSnippet& s, uint32_t pass, VkCommandBuffer recordingCmd);
void NgxTeardown(NgxSnippet& s, VkDevice device);

}  // namespace dlssnr