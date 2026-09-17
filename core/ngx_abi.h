#pragma once
// Minimal NGX Vulkan ABI. Vtable order and Init_Ext signature per verified reference.
#include <windows.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define NVSDK_CONV __cdecl

enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Success                   = 0x1,
    NVSDK_NGX_Result_Fail                      = (int)0xBAD00000,
    NVSDK_NGX_Result_FAIL_Failure              = (int)0xBAD00001,
    NVSDK_NGX_Result_FAIL_PlatformError        = (int)0xBAD00002,  // caller-module gate
    NVSDK_NGX_Result_FAIL_IncompatibleTypes    = (int)0xBAD00003,
    NVSDK_NGX_Result_FAIL_FeatureNotFound      = (int)0xBAD00004,
    NVSDK_NGX_Result_FAIL_InvalidParameter     = (int)0xBAD00005,
    NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall= (int)0xBAD00006,
    NVSDK_NGX_Result_FAIL_NotInitialized       = (int)0xBAD00007,
    NVSDK_NGX_Result_FAIL_UnsupportedInputFormat=(int)0xBAD00008,
    NVSDK_NGX_Result_FAIL_RWFlagMissing        = (int)0xBAD00009,
    NVSDK_NGX_Result_FAIL_MissingInput         = (int)0xBAD0000A,
    NVSDK_NGX_Result_FAIL_UnableToInitializeFeature=(int)0xBAD0000B,
    NVSDK_NGX_Result_FAIL_OutOfDate            = (int)0xBAD0000C,
    NVSDK_NGX_Result_FAIL_OutOfGPUMemory       = (int)0xBAD0000D,
    NVSDK_NGX_Result_FAIL_UnsupportedFormat    = (int)0xBAD0000E,
    NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath=(int)0xBAD0000F,
    NVSDK_NGX_Result_FAIL_UnsupportedParameter = (int)0xBAD00010,
    NVSDK_NGX_Result_FAIL_Denied               = (int)0xBAD00011,
    NVSDK_NGX_Result_FAIL_NotImplemented       = (int)0xBAD00012,
    NVSDK_NGX_Result_FAIL_SEH                  = (int)0x8BADF00D,
};
#define NVSDK_NGX_SUCCEED(r) (((r) & 0xFFF00000) != 0xBAD00000)

typedef unsigned long long NVSDK_NGX_ULONGLONG;
typedef unsigned int NVSDK_NGX_Version;
#define NVSDK_NGX_Version_API_13 0x00000013u
#define NVSDK_NGX_Version_API_14 0x00000014u

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_FeatureDiscoveryInfo; // opaque; passed as nullptr

// Canonical NVIDIA Vulkan resource layout (union of image-view / buffer info).
enum NVSDK_NGX_Resource_VK_Type
{
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGE_VIEW = 0,
    NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER = 1,
};

struct NVSDK_NGX_ImageViewInfo_VK
{
    VkImageView ImageView;
    VkImage Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat Format;
    unsigned int Width;
    unsigned int Height;
};

struct NVSDK_NGX_BufferInfo_VK
{
    VkBuffer Buffer;
    unsigned int SizeInBytes;
};

struct NVSDK_NGX_Resource_VK
{
    union
    {
        NVSDK_NGX_ImageViewInfo_VK ImageViewInfo;
        NVSDK_NGX_BufferInfo_VK BufferInfo;
    } Resource;
    NVSDK_NGX_Resource_VK_Type Type;
    bool ReadWrite;
};

struct NVSDK_NGX_Parameter
{
    // Slot 0 (0x00)
    virtual void NVSDK_CONV Set(const char* InName, void* InValue) = 0;
    // Slot 1 (0x08)
    virtual void NVSDK_CONV Set(const char* InName, unsigned long long InValue) = 0;
    // Slot 2 (0x10)
    virtual void NVSDK_CONV Set(const char* InName, float InValue) = 0;
    // Slot 3 (0x18)
    virtual void NVSDK_CONV Set(const char* InName, double InValue) = 0;
    // Slot 4 (0x20)
    virtual void NVSDK_CONV Set(const char* InName, unsigned int InValue) = 0;
    // Slot 5 (0x28)
    virtual void NVSDK_CONV Set(const char* InName, int InValue) = 0;

    // Slot 6 (0x30)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, double* OutValue) const = 0;
    // Slot 7 (0x38)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, unsigned long long* OutValue) const = 0;
    // Slot 8 (0x40)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, void** OutValue) const = 0;
    // Slot 9 (0x48) - reserved / padding
    virtual NVSDK_NGX_Result NVSDK_CONV GetReserved9(const char* InName, void* OutValue) const = 0;
    // Slot 10 (0x50) - reserved / padding
    virtual NVSDK_NGX_Result NVSDK_CONV GetReserved10(const char* InName, void* OutValue) const = 0;
    // Slot 11 (0x58)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, int* OutValue) const = 0;
    // Slot 12 (0x60)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, unsigned int* OutValue) const = 0;
    // Slot 13 (0x68) - reserved / padding
    virtual NVSDK_NGX_Result NVSDK_CONV GetReserved13(const char* InName, void* OutValue) const = 0;
    // Slot 14 (0x70)
    virtual NVSDK_NGX_Result NVSDK_CONV Get(const char* InName, float* OutValue) const = 0;

    // Slot 15 (0x78)
    virtual void NVSDK_CONV Reset() = 0;

    // Inline helpers to route resource pointers directly through Slot 0 and Slot 8
    void Set(const char* InName, const NVSDK_NGX_Resource_VK* InValue) {
        Set(InName, (void*)InValue);
    }
    NVSDK_NGX_Result Get(const char* InName, const NVSDK_NGX_Resource_VK** OutValue) const {
        return Get(InName, (void**)OutValue);
    }
};

typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_NVSDK_NGX_ProgressCallback)(float progress);

// Feature 18: DLSS Neural Rendering (DLSS 5 NR)
#define FEATURE_DLSSNR 18
#define DLSSNR_SIGNED_SNIPPET_APPLICATION_ID 0x0876232Cull

// NVSDK_NGX_PerfQuality_Value (public SDK values). Spelled out because the value was
// being passed as a bare integer with a comment beside it, and the comment said
// "Balanced" next to a 3. 3 is UltraPerformance; Balanced is 1. A named constant cannot
// disagree with itself that way.
//
// Which value is right for this pass is a separate question and is not settled here. The
// pass does no upscaling -- DLSSNR.ScalingRatio is pinned to 1.0 and the ratio callback
// answers 1.0 -- so the mode that MEANS "native resolution" is DLAA. What is established
// is that the key is live: nvngx_dlssnr.dll carries the strings
//   "Error: missing PerfQualityValue for DLSSNR scaling ratio computation"
//   "Error: unsupported PerfQualityValue %u for DLSSNR scaling ratio computation"
// so it is read, it is validated, and some values are refused. Sweep it with a held frame
// and watch for the second string before changing the shipped value.
enum NVSDK_NGX_PerfQuality_Value {
    NVSDK_NGX_PerfQuality_Value_MaxPerf          = 0,
    NVSDK_NGX_PerfQuality_Value_Balanced         = 1,
    NVSDK_NGX_PerfQuality_Value_MaxQuality       = 2,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance = 3,
    NVSDK_NGX_PerfQuality_Value_UltraQuality     = 4,
    NVSDK_NGX_PerfQuality_Value_DLAA             = 5,
};

// NVSDK_NGX_DLSS_Feature_Flags (public SDK values; "Feature_Flags" create param).
enum NVSDK_NGX_DLSS_Feature_Flags {
    NVSDK_NGX_DLSS_Feature_Flags_IsHDR              = 0x1,
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted      = 0x2,
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening       = 0x4,
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure       = 0x8,
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes           = 0x10,
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered         = 0x20,
    NVSDK_NGX_DLSS_Feature_Flags_ResetRenderProfile = 0x100,
};

// ---- snippet (nvngx_dlssnr.dll) Vulkan export signatures ----
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkInitExt)(
    unsigned long long InApplicationId, const wchar_t* InApplicationDataPath,
    VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice,
    NVSDK_NGX_Version InSDKVersion, const NVSDK_NGX_FeatureDiscoveryInfo* InFeatureInfo);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkCreateFeature)(
    VkCommandBuffer InCmdList, int feature, NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** handle);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkEvaluateFeature)(
    VkCommandBuffer InCmdList, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkReleaseFeature)(NVSDK_NGX_Handle* handle);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkShutdown1)(VkDevice InDevice);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkGetScratchBufferSize)(
    int feature, NVSDK_NGX_Parameter* parameters, size_t* size);

// ---- core (nvngx.dll) Vulkan export signatures ----
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkAllocateParameters)(NVSDK_NGX_Parameter** parameters);
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkDestroyParameters)(NVSDK_NGX_Parameter* parameters);typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkGetFeatureRequirements)(VkInstance, VkPhysicalDevice, NVSDK_NGX_Parameter*);

struct NVSDK_NGX_SDK_VERSION { unsigned int Major; unsigned int Minor; };
struct NVSDK_NGX_FeatureRequirements {
    NVSDK_NGX_SDK_VERSION Version;
    unsigned int FeatureFlags;
    unsigned int MinGPUMode;
    unsigned int InGPUMode;
    unsigned int MinCSMajorVersion;
    unsigned int MinCSMinorVersion;
};
typedef NVSDK_NGX_Result (NVSDK_CONV* FnVkGetFeatureReqs2)(VkInstance, VkPhysicalDevice, NVSDK_NGX_FeatureRequirements*);
