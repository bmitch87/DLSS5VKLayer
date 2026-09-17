# Phase 1 — Code Mining: DLSS NR (Feature 18) Pipeline Extraction

Repository mined: `./Magpie/` (SAOG0721/Magpie experimental fork, `experimental` line, v0.6.1-experimental era).
Method: ripgrep for `dlss_nr|DLSSNR|Feature 18|NVSDK_NGX|sl::|Streamline`, full read of every hit, plus PE export/import dump of `./binaries/*.dll`.

---

## 1. Search results summary

| Pattern | Result |
|---|---|
| `DLSSNR` / `dlss_nr` | Full native backend: `src/Magpie.Core/DLSSNRFilter.{h,cpp}` (2107 lines), placeholder HLSL `src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl`, factory `NativeEffectBackendFactory.cpp`, Renderer wiring, docs/TODOs |
| `Feature 18` | `DLSSNRFilter.cpp:31` (`static_cast<NVSDK_NGX_Feature>(18)`), status logs at `:1744`, `:1783`, `:1864`, `:1997`; factory failure log `NativeEffectBackendFactory.cpp:102` |
| `NVSDK_NGX` | `DLSSNRFilter.cpp`, `DLSSSRUpscaler.cpp`, `DLSSFrameGenerator.cpp`, `NgxD3D12Core.cpp`; headers `<nvsdk_ngx.h>`, `<nvsdk_ngx_helpers.h>`, `<nvsdk_ngx_helpers_dlssg.h>` are **NOT vendored** — supplied externally via `$(DLSSSdkDir)\include` (`src/Common.Post.props:19-23`), linked against `$(DLSSSdkDir)\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib` (`src/Magpie/Magpie.vcxproj:98`) |
| `sl::` (Streamline C++ API) | **Zero hits in source.** Streamline appears only as (a) a doc link to `NVIDIA-RTX/Streamline` ProgrammingGuideDLSS_G.md (`docs/experimental/todos/completed/20260829-164413-DLSS5-FrameGuidance-short-term-TODO.md:129`) and (b) DLLs in workspace `./binaries/` (see §9). The repo never loads `sl.interposer.dll`/`sl.dlss_nr.dll`. |
| `SAOG0721` | Fork owner identity only (release URLs, update feed: `version.json:4`, `src/Magpie/UpdateService.cpp:76`). Not a code symbol. |

Build gates: `MP_ENABLE_DLSSNR`, `MP_ENABLE_DLSS_SR`, `MP_ENABLE_DLSS_FRAME_GENERATION` (`src/Common.Post.props:18-22`, defaults `false` in `src/BuildOptions.props:20-24`; x64-only).

---

## 2. Feature IDs

| Feature | ID | Where |
|---|---|---|
| **DLSS NR** | **18** | `DLSSNRFilter.cpp:30-31`: `constexpr NVSDK_NGX_Feature FEATURE_DLSSNR = static_cast<NVSDK_NGX_Feature>(18);` — the only hard-coded feature ID in the repo (18 is not in public `NVSDK_NGX_Feature` enums of older SDKs) |
| DLSS SR | 4 (via `NGX_D3D11_CREATE_DLSS_EXT` / `NGX_D3D11_EVALUATE_DLSS_EXT` helper macros, `DLSSSRUpscaler.cpp:164,313`) | ID lives in non-vendored `nvsdk_ngx_helpers.h` |
| DLSS FG (DLSS 4/5 FG) | 9 (via `NGX_D3D12_CREATE_DLSSG` / `NGX_D3D12_EVALUATE_DLSSG`, `DLSSFrameGenerator.cpp:150,167`) | ID lives in non-vendored `nvsdk_ngx_helpers_dlssg.h` |

`NVSDK_NGX_Result_Success == 0x1` (logs assert `result=0x1`, TODO `20260829-DLSSNR-performance-observability-TODO.md:73`).

---

## 3. Two-part NGX bootstrap for Feature 18 (the critical API route)

Feature 18 is **not** created through the Core `nvngx.dll` route. Magpie uses a "signed snippet" route. Core is used only for parameter-block allocation and process-global init.

### 3.1 Core init — `src/Magpie.Core/NgxD3D12Core.cpp`

- Own D3D12 device on the same adapter: `D3D12CreateDevice(resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0, ...)` (`:123`).
- One-time init (`:30-37`):
  ```cpp
  NVSDK_NGX_D3D12_Init_with_ProjectID(
      "7c134ab9-9677-4af5-a2b2-bca943350861",   // custom ProjectID GUID string
      NVSDK_NGX_ENGINE_TYPE_CUSTOM,
      "Magpie-Experimental-0.5.7",              // engine version string
      applicationDirectory,                     // exe dir (wchar_t*)
      device,
      featureInfo,                              // NVSDK_NGX_FeatureCommonInfo
      NVSDK_NGX_Version_API);
  ```
  `featureInfo.PathListInfo.Path = { appDir }`, `Length = 1` (`:135-138`) — the DLL search path for feature runtimes.
- Parameter blocks: `NVSDK_NGX_D3D12_AllocateParameters` / `GetCapabilityParameters` / `DestroyParameters` (`:49,61,73`), ref-counted per consumer (`_activeConsumers`, `_activeParameterBlocks`).
- Shutdown: `NVSDK_NGX_D3D12_Shutdown1(device)` (`:85`).
- Renderer owns one `NgxD3D12Core _ngxD3D12Core;` per session (`Renderer.h:174`); DLSSNR and DLSSFG acquire/release it by name (`"DLSSNR"` / `"DLSSFG"`).

### 3.2 Signed snippet route — `src/Magpie.Core/DLSSNRFilter.cpp`

`InitializeSignedSnippet()` (`:1146-1195`):

1. `LoadLibraryExW(appDir\nvngx_dlssnr.dll, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` (`:1150-1154`).
2. `GetProcAddress` for exactly five exports (`:1160-1169`):
   - `NVSDK_NGX_D3D12_Init_Ext`
   - `NVSDK_NGX_D3D12_CreateFeature`
   - `NVSDK_NGX_D3D12_EvaluateFeature`
   - `NVSDK_NGX_D3D12_ReleaseFeature`
   - `NVSDK_NGX_D3D12_Shutdown1`
3. **Caller-identity IAT hook** (`InstallSnippetCallerCompatibility`, `:654-718`): parses the snippet DLL's own import table (`FindImportedFunctionSlot`, `:604-652`), finds its `KERNEL32!GetModuleFileNameW` IAT slot, and patches it to `SnippetGetModuleFileNameW` (`:565-594`) which reports the caller module as **`L"nvngx.dll"`** (`:571`) when the snippet asks about Magpie's module. This is the signature/authorization bypass: the snippet verifies its caller is `nvngx.dll`. Verified against the shipped binary: `nvngx_dlssnr.dll` imports `GetModuleFileNameW` from `KERNEL32.dll` (ordinal 0x291).
4. Init call (`SnippetInitExtFn`, `:380-382`, invoked `:549-563`):
   ```cpp
   NVSDK_NGX_Result NVSDK_CONV Init_Ext(
       0x0876232Cull,            // DLSSNR_SIGNED_SNIPPET_APPLICATION_ID (:32)
       applicationDirectory,     // data path (the DLL's own dir)
       device12,
       NVSDK_NGX_Version_API,
       nullptr);                 // no capability param block
   ```
5. Create/Evaluate/Release/Shutdown all go through the **snippet's** exports, never Core (`:1727-1734`, `:1965-1970`, `:826-828`, `:846-849`). Core `CreateFeature(18)` is a compile-time-disabled diagnostic (`ENABLE_CORE_FEATURE18_DIAGNOSTIC = false`, `:33-35`).

Documented failure codes (TODO `20260829-DLSSNR-performance-observability-TODO.md:30,75`):
- Core `CreateFeature(18)` → `0xbad0000b` (Core has no NR implementation).
- Direct `Init_Ext` without correct AppID/data dir/caller hook → `0xbad00002`.
- Working route: `created=true path=signed-snippet`, Evaluate `result=0x1`.

Teardown order (fixed, `:820-877` + TODO `20260830-DLSS-combination-compatibility-short-term-TODO.md:52`):
GPU drain → snippet `ReleaseFeature` → Core `DestroyParameters` → snippet `Shutdown1` → restore IAT → `FreeLibrary` → Core `Shutdown1`.

### 3.3 Calling conventions / SEH

- All NGX entry points use `NVSDK_CONV` (`__cdecl`; no-op on x64). Fn-pointer typedefs: `DLSSNRFilter.cpp:380-391`.
- Every call crosses a `__try/__except` boundary (`CallCreateFeatureSafely` `:492`, `CallEvaluateFeatureSafely` `:508`, `CallReleaseFeatureSafely` `:523`, `CallShutdownSafely` `:536`, `CallSnippetInitSafely` `:549`, parameter writes `:1132/:1232`); SEH code captured, fail-closed → pass-through fallback.
- `NVSDK_NGX_Parameter` is used via member `Set(name, value)` overloads (uint/int/float/const char*/`ID3D12Resource*`/function pointer) in NR; FG/SR also use free functions `NVSDK_NGX_Parameter_SetI/SetUI/SetULL/GetI/GetUI` (`DLSSFrameGenerator.cpp:91-139`, `DLSSSRUpscaler.cpp:141`).

---

## 4. Feature 18 Create parameters (`SetCreateParametersUnsafe`, `DLSSNRFilter.cpp:1104-1130`)

| Parameter string | Value | Line |
|---|---|---|
| `DLSSNR.Width` / `DLSSNR.Height` | `width/height` (same-res target) | 1108-1109 |
| `DLSSNR.InputWidth` / `DLSSNR.InputHeight` | same | 1110-1111 |
| `DLSSNR.OutputWidth` / `DLSSNR.OutputHeight` | same | 1112-1113 |
| `DLSSNR.Output.Width` / `DLSSNR.Output.Height` | same | 1114-1115 |
| `DLSSNR.Upscaling` | `0u` | 1116 |
| `DLSSNR.Scale` | `1.0f` | 1117 |
| `DLSSNR.ScalingRatio` | `1.0f` | 1118 |
| `DLSSNRComputeScalingRatioCallback` | fn ptr → sets `DLSSNR.ScalingRatio=1.0f` (`:1092-1102`) | 1119-1121 |
| `DLSSNR.Hint.Render.Preset` | `settings.preset` (0–3) | 1122 |
| `NVSDK_NGX_Parameter_Width` / `_Height` | same | 1123-1124 |
| `NVSDK_NGX_Parameter_PerfQualityValue` | `NVSDK_NGX_PerfQuality_Value_Balanced` | 1125-1127 |

> Note: the layer ships `UltraPerformance` (3) here, not `Balanced` (1). The two have
> disagreed since some point after this note was written. The key is live -- the DLL
> validates it and refuses some values -- so which is right is a measurement, not a
> transcription error to silently correct. See `core/ngx_abi.h`, `NVSDK_NGX_PerfQuality_Value`.

| `NVSDK_NGX_Parameter_CreationNodeMask` / `_VisibilityNodeMask` | `1u` | 1128-1129 |

NR is **same-resolution only**: init rejects mismatched input/output extents (`:1557-1562`). Format contract: input `R8G8B8A8_UNORM` or `B8G8R8A8_UNORM` (BGRA→RGBA converted by compute), output must be `R8G8B8A8_UNORM` (`:1563-1570`).

Create call: `snippetCreateFeature(commandList12, FEATURE_DLSSNR, parameters, &feature)` (`:1732-1734`), then `Close()` + execute + fence-wait the init command list (`:1750-1759`).

---

## 5. Feature 18 Evaluate parameters (`SetEvaluateParametersUnsafe`, `DLSSNRFilter.cpp:1197-1230`)

| Parameter string | Value | Notes |
|---|---|---|
| `DLSSNR.Color` | `ID3D12Resource*` sharedInput12 | NT-handle-shared RGBA8 texture |
| `DLSSNR.Output` | `ID3D12Resource*` sharedOutput12 | UAV-written by the net |
| `DLSSNR.MVec` | `guidanceInterop->Motion()` → `ID3D12Resource*` | **zero-filled dummy when no real MVs** |
| `DLSSNR.Depth` | `guidanceInterop->Depth()` → `ID3D12Resource*` | **zero-filled dummy when no real depth** |
| `DLSSNR.{Color,Output}Subrect{BaseX,BaseY,Width,Height}` | full extent | `RESOURCE_PARAMETERS[0..1]`, `:76-85`, `SetSubrect :1081` |
| `DLSSNR.MVecSubrect*` | `guidance.motion.metadata.validRegion` | |
| `DLSSNR.DepthSubrect*` | `guidance.depth.metadata.validRegion` | |
| `DLSSNR.MVecScaleX/Y` | `1.0f` | MVs are in source pixels, current→previous |
| `DLSSNR.DepthInverted` | `1` | relative inverse depth (near=1) |
| `DLSS.Indicator.Invert.X.Axis` / `Y.Axis` | `0` | |
| `DLSSNR.Enabled` | `1` | |
| `DLSSNR.Reset` | `1` on first frame / guidance reset else `0` | |
| `DLSSNR.Style` / `Intensity` / `LocalToneStrength` / `LocalStructureStrength` / `SkinStructureStrength` / `UseAutoMask` / `UICorrection` | settings | |

Evaluate call: `snippetEvaluateFeature(commandList, feature, parameters, /*callback*/nullptr)` (`:386-388`, `:1968-1970`).

The DLL's own string table confirms the full accepted namespace (dumped from `./binaries/nvngx_dlssnr.dll`): `DLSSNR.{Width,Height,Color,Output,MVec,Depth,ControlMask,UI,UIAlpha,BidirectionalDistortionField,Backbuffer}` + `*Subrect{BaseX,BaseY,Width,Height}` variants + `DepthInverted,Enabled,Reset,Style,Intensity,LocalToneStrength,LocalStructureStrength,SkinStructureStrength,UseAutoMask,UICorrection,Hint.Render.Preset,ScalingRatio,MVecScaleX/Y`. Magpie uses only the Color/Output/MVec/Depth subset; `DLSSNR.Backbuffer*` is the Streamline-facing alias (see §9).

---

## 6. How the neural pass is fed WITHOUT motion vectors or depth (dummy resources)

This is the exact mechanism requested:

1. **Zero texture set** — `ZeroFrameGuidanceProvider.cpp:22-77` (`ZeroFrameGuidanceResources::_CreateTextures`), shared by `ZeroDepthProvider` + `ZeroMotionVectorProvider` (`ZeroFrameGuidanceProvider.h:8-32`):
   - Depth: `DXGI_FORMAT_R32_FLOAT`, source extent
   - Motion: `DXGI_FORMAT_R16G16_FLOAT`
   - Confidence: `DXGI_FORMAT_R8_UNORM`
   - All: `D3D11_USAGE_DEFAULT`, `BIND_SHADER_RESOURCE|BIND_UNORDERED_ACCESS`, `MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`
   - Cleared once with `ClearUnorderedAccessViewFloat(..., {0,0,0,0})` (`:68-71`) and never rewritten → **permanently zero**.
   - Metadata: `isZero=true`, `valid=true`, `requiresHistoryReset` on Initialize/Resize/SceneChange/CaptureInterrupted/DeviceRecreated/LongPause/ProviderFailure (`:79-92`; reasons `FrameGuidanceTypes.h:37-46`).
2. **Selection/fallback** — `DLSSNRFilter::SelectGuidance` (`:1813-1842`): produced guidance must satisfy `IsValidFor(frameId, extent)` (`FrameGuidanceTypes.h:133-142`: depth `R32_FLOAT`, motion `R16G16_FLOAT`, confidence `R8_UNORM`, equal `validRegion`, same `frameId`) or the whole group falls back to `context.zeroFrameGuidance`. `guidanceMode`: 0=available, 1=force zero, 2=motion only (zero depth), 3=depth only (zero motion+confidence) (`:1515-1532`, `:1821-1833`).
3. **D3D11→D3D12 handoff** — `FrameGuidanceD3D12Interop.cpp`:
   - `Update()` (`:46-75`) validates motion contract (`CurrentToPrevious`, `SourcePixels`, `RelativeInverse`, `depthInverted==true`) then `_OpenShared()`: `IDXGIResource1::CreateSharedHandle(GENERIC_ALL)` → `ID3D12Device::OpenSharedHandle` → `ID3D12Resource*` (`:30-44`).
   - `WaitForProducer()` (`:77-89`) waits the producer's `ID3D11Fence` sync point on the D3D11 context (zero provider has `sync.fence==nullptr` → ordered on immediate context, no wait).
   - `Transition()` (`:91-105`) emits `COMMON → NON_PIXEL_SHADER_RESOURCE` (and back) for motion+depth on the D3D12 list.
4. **Per-frame Draw sequence** (`DLSSNRFilter::Draw`, `:1844-2077`):
   - Duplicate `frameId` → reuse cached output, no Evaluate (`:1851-1860`).
   - `PrepareInput` (`:1248-1298`): `CopyResource` or BGRA→RGBA compute (`ConvertToRgba`, 8×8 groups) into `sharedInput11`.
   - Guidance select + interop update + `context11->Signal(fence11, n)` / `Flush()` / `queue12->Wait(fence12, n)` (`:1913-1922`).
   - Barriers: Color `COMMON→NON_PIXEL_SHADER_RESOURCE`, Output `COMMON→UNORDERED_ACCESS` (`:1932-1945`); guidance `COMMON→NON_PIXEL_SHADER_RESOURCE` (`:1946-1948`).
   - Set evaluate params → `snippetEvaluateFeature` on the D3D12 list (timestamps bracket it, `:1959-1981`).
   - Reverse barriers, `Close`, `ExecuteCommandLists`, `queue12->Signal` → `context11->Wait(fence11)` (`:2006-2030`).
   - `CopyResource(output, sharedOutput11)` (or residual composite). On any failure: `disabled=true`, pass-through copy of input next frame (`:1861-1868`, `:1883-1890`, `:2040-2043`).
   - 4-slot command allocator/list ring (`COMMAND_SLOT_COUNT=4`, `:370-378`).
5. **Confidence is never passed to NGX** — only `DLSSNR.MVec` + `DLSSNR.Depth` resources are bound; confidence drives only Magpie's own guidance-downsample blending.

When `enableInputResolutionScaling` is on, color/guidance are downsampled by Magpie compute (`COLOR_DOWNSAMPLE_HLSL`, `GUIDANCE_DOWNSAMPLE_HLSL`, `:100-203`; dispatch `(w+7)/8 × (h+7)/8`), the net runs at reduced res, and a Lanczos-3 residual is composited back (`RESIDUAL_HORIZONTAL_HLSL`, `RESIDUAL_VERTICAL_COMPOSITE_HLSL`, `:205-308`; `CompositeResidual :1441-1506`). `ResampleConstants` cbuffer is exactly 32 bytes (`:310-320`).

The neural dispatch itself is **inside `nvngx_dlssnr.dll`** — the repo's `DLSSNR_AI_Filter.hlsl` is a documented pass-through placeholder (`:1-2` "The native D3D12 backend replaces this pass").

---

## 7. Shared color path (D3D11↔D3D12)

`CreateSharedTexture` (`DLSSNRFilter.cpp:879-919`): `R8G8B8A8_UNORM`, `SHARED|SHARED_NTHANDLE`, `CreateSharedHandle(GENERIC_ALL)` → `OpenSharedHandle`. Fence: `ID3D11Fence` (`D3D11_FENCE_FLAG_SHARED`) shared to D3D12 the same way (`:1648-1666`). GPU timing via `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` (8 queries) + readback buffer (`:1673-1706`).

---

## 8. Sibling NGX consumers (for the combined DLSS-5 route)

### DLSS SR (`DLSSSRUpscaler.cpp`) — D3D11-only, separate Core init
- `NVSDK_NGX_D3D11_Init_with_ProjectID("7c134ab9-...", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Magpie-DLSS-SR-2", L"logs", device11)` (`:116-122`) — **does not share `NgxD3D12Core`**.
- Preset J: `NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_J)` (`:141-145`).
- Create: `NGX_D3D11_CREATE_DLSS_EXT(dc, &feature, params, &NVSDK_NGX_DLSS_Create_Params{InWidth/InHeight/InTargetWidth/InTargetHeight, InPerfQualityValue=Balanced, InFeatureCreateFlags=MVLowRes|AutoExposure[|DepthInverted], InEnableOutputSubrects=false})` (`:147-164`).
- Eval: `NGX_D3D11_EVALUATE_DLSS_EXT` with `NVSDK_NGX_D3D11_DLSS_Eval_Params`: zero `R16G16_FLOAT` MV + zero `R32_FLOAT` depth + `R8_UNORM` bias mask cleared to `(0.5,0.5,0.5,0.5)` per frame (`:219-286`), `InSharpness=0.3f`, optional Halton(2,3) jitter (`:200-209`), `InMVScaleX/Y=1`, `InPreExposure=InExposureScale=1` (`:288-318`).

### DLSS FG (`DLSSFrameGenerator.cpp`) — D3D12, shares `NgxD3D12Core`
- Capability gate: `NVSDK_NGX_Parameter_FrameGeneration_Available` / `_FeatureInitResult` (`:496-509`); multi-frame cap `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax` clamped 1–3 (`:514-521`).
- Declares never-provided resources: `NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags = HUDLess|UI|UIAlpha|BidirectionalDistortionField|OutputReal` (`:538-547`).
- Create: `NGX_D3D12_CREATE_DLSSG(cmdlist, 1, 1, &feature, params, &NVSDK_NGX_DLSSG_Create_Params{Width,Height,NativeBackbufferFormat,RenderWidth,RenderHeight,DynamicResolutionScaling=false})` (`:554-564`).
- Eval: `NGX_D3D12_EVALUATE_DLSSG` with `NVSDK_NGX_D3D12_DLSSG_Eval_Params{pBackbuffer,pDepth,pMVecs,pOutputInterpFrame,pOutputDisableInterpolation}` + `NVSDK_NGX_DLSSG_Opt_Eval_Params` with **identity camera matrices**, `cameraUp=(0,1,0)`, `cameraRight=(1,0,0)`, `cameraFwd=(0,0,1)`, `near=0.1`, `far=1000`, `FOV=1.04719755` (60°), `mvecScale=(1,1)`, `depthInverted`, `cameraMotionIncluded=realMotion`, `motionVectorsDilated=realMotion`, subrects from guidance `validRegion` (`:752-811`).
- Dummy MV/depth here are **pure D3D12** committed textures (`CreateZeroTexture`, `:265-315`): `R16G16_FLOAT` + `R32_FLOAT`, cleared via `ClearUnorderedAccessViewFloat`, then transitioned to `NON_PIXEL_SHADER_RESOURCE`.
- `NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID` set per frame (`:714-716`).

---

## 9. Streamline / binary evidence (workspace `./binaries/`, not referenced by repo code)

PE dump findings:

- `sl.interposer.dll` exports: `slGetPluginFunction`, `slGetFeatureFunction`, `slGetFeatureRequirements`, `slGetFeatureVersion`, `slGetData`, `slGetNativeInterface`, `slGetNewFrameToken`, `DllMain` (classic Streamline 1.x/2.x interposer surface; **no `QueryInterface`** → this is the pre-`ISlQueryInterface` era API).
- `sl.dlss_nr.dll` exports only `DllMain` + `slGetPluginFunction` (Streamline feature plugin). Its strings reuse the **same `DLSSNR.*` parameter namespace** plus `DLSSNR.Backbuffer*` aliases, and it loads `nvngx_dlssnr.dll` ("DLSS-NR feature is not supported. Please check if you have a valid nvngx_dlssnr.dll...").
- `nvngx_dlssnr.dll` (165 MB, community-modified 310.8.0.0 per `docs/RELEASE_NOTES_v0.5.7-experimental.md:51`) exports the **complete NGX family for all three APIs**, including:
  - `NVSDK_NGX_D3D12_{Init,Init_Ext,CreateFeature,EvaluateFeature,ReleaseFeature,Shutdown1,GetFeatureRequirements,GetScratchBufferSize,PopulateParameters_Impl}` (what Magpie uses)
  - `NVSDK_NGX_D3D11_*` and `NVSDK_NGX_CUDA_*` equivalents
  - **`NVSDK_NGX_VULKAN_{Init,Init_Ext,Init_Ext2,CreateFeature,CreateFeature1,EvaluateFeature,ReleaseFeature,Shutdown1,GetFeatureRequirements,GetScratchBufferSize,GetFeatureInstanceExtensionRequirements,GetFeatureDeviceExtensionRequirements,PopulateParameters_Impl}`**
  - `NVSDK_NGX_GetAPIVersion`, `GetApplicationId`, `GetDriverVersionEx`, `GetGPUArchitecture`, `GetSnippetVersion`, telemetry/callback setters.
  - Required Vulkan extensions embedded in the DLL: `VK_NVX_binary_import`, `VK_NVX_image_view_handle`, `VK_KHR_push_descriptor`, `VK_KHR_buffer_device_address`, `VK_KHR_get_physical_device_properties2`.
  - Imports `GetModuleFileNameW` from `KERNEL32.dll` (the exact IAT slot Magpie hooks).
- `nvngx.dll` (478 KB Core), `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.{common,dlss,dlss_g,nis,pcl,reflex}.dll` also present.

---

## 10. Renderer wiring (who calls what)

- `NativeEffectBackendFactory.cpp:65-109`: effect name `DLSSNR\DLSSNR_AI_Filter` → `DLSSNRSettings` from effect options (`enableInputResolutionScaling, inputResolutionPercent, residualMultiplier, nrPreset, style, intensity, localToneStrength, localStructureStrength, skinStructureStrength, useAutoMask, uiCorrection, guidanceMode, depthInferenceInterval`) → `DLSSNRFilter::Initialize(resources, ngxCore, input, output, settings)`; failure logs `DLSSNR STATUS: Feature=18 created=false path=unavailable fallback=pass-through`.
- `Renderer.cpp:1559-1580`: guidance requirements merged across backends (`CollectFrameGuidanceRequirements :37`); real providers (NVIDIA Optical Flow, DepthAnythingV2) instantiated only if a live backend requests them.
- `Renderer.cpp:1649-1671`: per-pass `NativeEffectDrawContext{input, output, frameId, produced, zero}` → `backend->Draw`.
- `Renderer.cpp:1675-1699`: FG consumes `guidance.produced/zero` and publishes interpolated frames through `_PublishBackendTexture`.
- Preset: `presets/ScalingModes-v0.5.7-experimental.json:37-46` registers `DLSSNR\DLSSNR_AI_Filter`.

---

## 11. Implications for the VK-layer API route (facts only)

1. Two proven routes exist for Feature 18: (a) Magpie's **D3D12 signed-snippet** route (`nvngx_dlssnr.dll` exports + AppID `0x0876232C` + `GetModuleFileNameW` caller spoof reporting `nvngx.dll`), or (b) the **Vulkan exports the same DLL carries** (`NVSDK_NGX_VULKAN_Init_Ext/Init_Ext2/CreateFeature/EvaluateFeature` + `GetFeature{Instance,Device}ExtensionRequirements`), which additionally demand `VK_NVX_binary_import` + `VK_NVX_image_view_handle` (driver-private binary/compute + opaque-handle image views).
2. The Streamline route (`sl.interposer.dll` + `sl.dlss_nr.dll`, `slGetPluginFunction` ABI) wraps the identical `DLSSNR.*` parameter namespace; `sl.dlss_nr.dll` itself loads `nvngx_dlssnr.dll`. No repo code exercises it.
3. The neural pass accepts **zero-filled dummy MV (`R16G16_FLOAT`) and depth (`R32_FLOAT`)** with `MVecScale=1`, `DepthInverted=1`, full-extent subrects, same-res Color/Output `R8G8B8A8_UNORM`, `DLSSNR.Upscaling=0`, `Scale/ScalingRatio=1` + ratio callback returning 1.0 — exactly what `DLSSNRFilter.cpp` binds per frame.
4. `NVSDK_NGX_Parameter` blocks must come from the matching API family's allocator (`NVSDK_NGX_D3D12_AllocateParameters` used for the snippet path; Vulkan path would require its own allocator/capability route — Core init with ProjectID `7c134ab9-9677-4af5-a2b2-bca943350861`, `ENGINE_TYPE_CUSTOM`).
5. All Feature-18 calls must be wrapped in SEH/VEH-style guards; failure codes `0xbad0000b` (Core CreateFeature 18) and `0xbad00002` (snippet Init_Ext rejected) are the known rejection signatures; success is `0x1`.

---

## 12. File index

| File | Role |
|---|---|
| `src/Magpie.Core/DLSSNRFilter.cpp` | Feature 18 lifecycle, snippet load, IAT hook, params, D3D11/12 sync, resample compute |
| `src/Magpie.Core/DLSSNRFilter.h` | `DLSSNRSettings`, backend interface |
| `src/Magpie.Core/NgxD3D12Core.{h,cpp}` | Shared Core init/params/shutdown, ProjectID GUID |
| `src/Magpie.Core/FrameGuidanceTypes.h` | Guidance structs, format contract, reset reasons |
| `src/Magpie.Core/ZeroFrameGuidanceProvider.{h,cpp}` | Zero-filled dummy MV/depth/confidence |
| `src/Magpie.Core/FrameGuidanceD3D12Interop.{h,cpp}` | NT-handle open, fence waits, state transitions |
| `src/Magpie.Core/DLSSSRUpscaler.cpp` | D3D11 DLSS SR (feature 4) + zero MV/depth/bias |
| `src/Magpie.Core/DLSSFrameGenerator.cpp` | D3D12 DLSSG (feature 9) + D3D12 zero MV/depth |
| `src/Magpie.Core/NativeEffectBackendFactory.cpp` | Name→backend dispatch, settings parsing |
| `src/Magpie.Core/Renderer.cpp` | Guidance service ownership, draw loop, FG publish |
| `src/Effects/DLSSNR/DLSSNR_AI_Filter.hlsl` | Pass-through placeholder + parameter schema |
| `src/Common.Post.props`, `src/BuildOptions.props`, `src/Magpie/Magpie.vcxproj` | SDK dir, static lib link, DLL copy steps (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `$(DLSSNRRuntimeDir)\nvngx_dlssnr.dll`) |
| `docs/experimental/todos/*` | API-route history, failure codes, lifecycle rules |
| `./binaries/*.dll` (workspace root) | Runtime evidence: exports incl. `NVSDK_NGX_VULKAN_*`, Streamline `slGet*` ABI |