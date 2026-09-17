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
    float loggedMVecScaleX = -1.0f, loggedMVecScaleY = -1.0f;
    bool loggedDepthBound = false;

    // Caller-identity spoof state
    void** iatSlot = nullptr;
    decltype(&GetModuleFileNameW) originalGetModuleFileNameW = nullptr;

    // Resource structs passed to the DLL (must outlive eval calls)
    NVSDK_NGX_Resource_VK resColor{}, resOut{}, resMV{}, resDepth{};

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
// What still depends on a rebuild is therefore the preset alone. MaintainPasses, tuningSeq
// and the GUI's AtCreate split are all sized for seven latched values and only one of them
// is -- worth revisiting, but that is a behaviour change and wants a picture behind it,
// because a key being READ at evaluate is not yet proof the model acts on it there.
struct NgxTuning {
    float intensity = 1.0f;
    float localTone = 1.0f;
    float localStructure = 1.0f;
    float skinStructure = -1.0f;
    uint32_t style = 0;
    uint32_t preset = 0;
    uint32_t autoMask = 1;

    bool operator==(const NgxTuning& o) const {
        return intensity == o.intensity && localTone == o.localTone &&
               localStructure == o.localStructure && skinStructure == o.skinStructure &&
               style == o.style && preset == o.preset && autoMask == o.autoMask;
    }
    bool operator!=(const NgxTuning& o) const { return !(*this == o); }
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
void NgxSetResources(NgxSnippet& s, const NVSDK_NGX_Resource_VK& color,
                     const NVSDK_NGX_Resource_VK& out, const NVSDK_NGX_Resource_VK& mv,
                     const NVSDK_NGX_Resource_VK& depth, uint32_t width, uint32_t height);
void NgxSetReset(NgxSnippet& s, bool reset, bool logValue = false);
// A no-op that logs once. This model has no sharpness parameter under any name -- see the
// definition. It was believed to be "the one strength the model reads at evaluate"; it is
// the one strength the model does not have.
void NgxSetSharpness(NgxSnippet& s, float sharpness);
// The six the model reads at EVERY evaluate, written per pass from that pass's own resolved
// tuning. Only the preset is latched at create; see NgxTuning.
void NgxSetEvaluateTuning(NgxSnippet& s, const NgxTuning& t);
// How the motion field's units are read. Written every evaluate, because it goes with the field.
void NgxSetMotionScale(NgxSnippet& s, float scaleX, float scaleY);
bool NgxEvaluatePass(NgxSnippet& s, uint32_t pass, VkCommandBuffer recordingCmd);
void NgxTeardown(NgxSnippet& s, VkDevice device);

}  // namespace dlssnr