# Attribution

This repository combines upstream code, adapted upstream techniques, vendored dependencies, and
project-original work. The entries below distinguish those things so that upstream authors are
credited without attributing project work to them.

## Project provenance

The base DLSS5VKLayer project is by [bmitch87](https://github.com/bmitch87). The Vulkan layer,
shared-memory protocol, helper integration, GUI, capture and frame-hold path, scaling and meter
implementation, motion-vector implementation, packaging, and subsequent HDR/zero-copy work are
project-specific implementations. They must not be attributed wholesale to the upstream projects
listed below. The git history identifies the project contributors, including bmitch87 and Thomas
Eric, for the respective changes.

## Upstream Code And Techniques

| Project | License | What is taken |
|---|---|---|
| [OptiScaler](https://github.com/cdozdil/OptiScaler) | GPL-3.0 | `Shader_Vk` and the output-scaling shader lineage. The Vulkan dispatch-table port and current layer integration are project work. |
| [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) | GPL-3.0 | The original DLSS-NR shader/module snapshot, imported from commit `97376162`. The current shader is a project-maintained derivative; its later HDR, controls, and Vulkan integration are not Dagherbou's work. |
| [RenoDX](https://github.com/clshortfuse/renodx) | MIT | The DLSS 5 colour-composition design reimplemented in `dlssnr.hlsl`; see below. |
| [xenmods/DLSSNR-Cost-Scaler](https://github.com/xenmods/DLSSNR-Cost-Scaler) | MIT | The native + edit enlargement technique only. No code was copied. |
| [vkBasalt](https://github.com/DadSchoorse/vkBasalt) | zlib | Consulted for layer-structure and input-handling ideas. No vkBasalt code is identified in this repository. |
| [DXVK](https://github.com/doitsujin/dxvk) | zlib | Vendored runtime `third_party/dxvk/2.7.1/x64/vulkan-1.dll`; its license is shipped as `third_party/dxvk/2.7.1/LICENSE.txt` and included in packages. |
| [DXVK-NVAPI](https://github.com/jp7677/dxvk-nvapi) | MIT | The Wine runtime may download its unmodified `x64/nvapi64.dll` release asset at runtime; it is not bundled in this repository or packages. |
| [Khronos Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 / applicable header notices | Vendored Vulkan and video headers under `standalone_runner/third_party/`; the headers retain their Khronos copyright and SPDX notices. |
| [stb](https://github.com/nothings/stb) | Public domain / applicable embedded notice | Vendored `stb_image.h` and `stb_image_write.h`; their author and license notices remain in the headers. |

The GPL-3.0 text for the OptiScaler-derived material is in `third_party/optiscaler/LICENSE`.
RenoDX's MIT notice is in `third_party/optiscaler/RenoDX_ATTRIBUTION.txt`. Other dependencies retain
their notices at the paths given above; not every notice is a separate file in `third_party/`.

## Findings From Reviewed Projects

Some changes here act on a **finding** reported by another project — a fact about NVIDIA's runtime, a
driver or the Wine stack — rather than on its code. No code from these projects is present. They are
credited because the finding is theirs and because the next reader should be able to reach the source
that prompted a change.

| Project | License | Finding acted on |
|---|---|---|
| [Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation](https://github.com/Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation) | MIT | That a repeated frame and a scene cut both produce a zero motion field and are opposites — a duplicate is a *confident* statement that nothing moved (`dlss_engine/frame_interpolation/guides.py:37-62`). Also `cp --remove-destination` when installing DLLs into a Wine prefix: a plain `cp` writes *through* a prefix symlink into the file it points at (`docs/linux.md:186-190`). Also the Wine heap flags that keep NGX from crashing during cleanup, and `dxgi.hideNvidiaGpu = False` / `PROTON_HIDE_NVIDIA_GPU=0` (`docs/linux.md:194`, `:213-222`, `:365-367`). |
| [pcdofafa/dlss5-for-all](https://github.com/pcdofafa/dlss5-for-all) | MIT | That a magnitude threshold cannot catch an out-of-range motion estimate — above its search range the engine returns a *small* wrong vector, not a large one — so the per-block cost buffer has to be enabled to tell them apart (`motion_estimate.h:28-42`). Also reporting Laplacian variance and Sobel gradient energy **together**, with the stated reason that they fail differently, and discarding a border before measuring (`tests/nitidez_png.cpp`). Also the measurement discipline in `tools/measure.sh`: verify the consumer echoed the setting or discard the round, measure on a still target, and interleave anything timed (`tests/medir-residual.ps1`). The metrics themselves are standard; both tools are project implementations. |
| [LCPD15/DXL](https://github.com/LCPD15/DXL) | **AGPL-3.0** — findings only, no code | That a parameter container should record which keys the DLL actually *read*, because "set" looks like "in effect" and is not (`src/core/NgxParameterBag.h:126-146`); and that a run of guessed `DLSSNR.*` key names does not exist in the binary (`src/core/DlssNrFilter.cpp:35-39`). Both were re-derived here against our own runtime before acting. The probe in `core/ngx_param.h` is a project implementation written from the description; no AGPL code is present, and none may be taken. Also the framing of the diagnostic kill switches: one switch per explanation, ordered so that what survives each rung names the culprit, and each cutting at the smallest possible early-out rather than becoming a mode (`src/core/DlssNrFilter.h:68-85`). Their three cuts are for an in-frame D3D12 filter; ours are `DLSSNR_DRY_RUN`, `DLSSNR_NO_ROUNDTRIP` and `DLSSNR_SKIP_EVALUATE`, written for a two-process Vulkan present hook. Also the breadcrumb ring and the question it is built to answer -- *when it hung, who had control?* -- with the stage list ordered so that the last stage recorded names the culprit, and the argument that it has to be cheap enough to stay on always because a diagnostic switched on after the fact is not one (`src/core/FreezeWatchdog.h:3-20`, `:31-72`); and that the GPU needs breadcrumbs of its own because a device-removed code names no command (`src/core/FreezeWatchdog.cpp:19-24`), whose D3D12 DRED table maps exactly onto `VK_NV_device_diagnostic_checkpoints`. Description only; no AGPL code is present. |
| [NIGos/dlss5-bridge](https://github.com/NIGos/dlss5-bridge) | MIT | That a neighbouring build must be identified by its version resource AND a hash, because the version number does not identify one — which is how the model's own `NGXMinimumDriverVersion` and `NGXGpuArchitecture` fields were found (`README.md:97-103`). Also that `VK_KHR_push_descriptor` removes the pool, the set and the "update a set that may still be in flight" problem for a one-shot compute pass (`src/synth.inc:3667-3675`). |
| [kkyleeb21/OptiScaler_DLSSNR](https://github.com/kkyleeb21/OptiScaler_DLSSNR) | GPL-3.0 | The untested theory that `CreateFeature` may need `GetScratchBufferSize(18)` satisfied first (`OptiScaler/dlssnr/FORWARDER_INVESTIGATION.md:54`). Tested here: it reports zero. |
| [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer) | MIT | That NGX's shutdown and module unload have been observed to wedge after a successful feature-18 evaluation, so normal teardown should not call them (`src/core/neural_bridge.py:3-9`); and that a fault guard does not catch a hang, so native calls need a watchdog and a poisoned state that refuses later calls (`src/core/neural_bridge.py:885-927`). |
| [mattjaas/OptiScaler_DLSSNR_Autoexposure](https://github.com/mattjaas/OptiScaler_DLSSNR_Autoexposure) | GPL-3.0 | Setting an NGX resource under a *generic* name, which prompted testing every non-`DLSSNR.` key we write against the runtime — that is how `Sharpness` and `Feature_Flags` were found to be absent (`tools/apply_dlssnr_autoexposure.py:448-455`). |
| [y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG) | GPL-3.0 | That everything the model is told except the preset is an evaluate argument, not a create-time latch (`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp:1513-1517`; read in machachu56's fork). Confirmed here by instrumenting our own parameter container. Also that extra passes should be priced against measured video memory before they are built, and that a VRAM failure inside the snippet can arrive as `VK_ERROR_DEVICE_LOST` rather than as a null handle (`OptiScaler/dlssnr/README.md:120-124`). Also that a present is not a frame once anything downstream generates them, so a present-counted quantity is wrong by the generation multiplier -- and that the honest response is to carry the ratio rather than to guess at detection (`OptiScaler/upscalers/IFeature.cpp:300-304`, `OptiScaler/hooks/Vulkan_Hooks.cpp:313-321`). Theirs reads the multiplier from their own Streamline hooks, which we do not have; ours counts presents against round trips, which needs no detection at all. |
| [SAOG0721/Magpie](https://github.com/SAOG0721/Magpie) | GPL-3.0 | That NVIDIA's optical-flow engine keeps temporal hints of its own and they must be disabled on any frame carrying a reset reason (`src/Magpie.Core/NvidiaOpticalFlowProvider.cpp:804-808`, and their reading of NVIDIA's programming guide). Also: that a guide is a typed resource with a valid region, and that the MVec subrect must describe what the field actually holds rather than the whole frame (`src/Magpie.Core/FrameGuidanceTypes.h`, `DLSSNRFilter.cpp:1467-1474`); NVIDIA's published NVOFA comparison, transcribed and read, showing that a 1x1 flow grid is not the highest-quality mode (`docs/NVOFA_Application_Note_中文解读.md`); that after an NGX fault the SDK must never be re-entered, with two crash dumps and a held critical section behind it (`docs/experimental/reviews/20260906-v0.6.6-ngx-deadlock.md`); and the 250 ms rule: a frame gap longer than that invalidates temporal history, and that a repeated frame is not a new sample -- history advances only on a new, successfully completed capture (`docs/experimental/design/20260914-dlssnr-temporal-stabilization-routes.md`, `20260914-screenshot-dlssnr-flicker-fix-todo.md`). Also the shape that makes a ring-buffer frame trace still readable once it has been useful: capacity counted in events rather than seconds, the overwrite count written into the file, whole-session totals kept outside the ring, a second ring for the slow events alone, a clock header line and a completeness marker the reader refuses a file without, and the recording switched on in the same binary rather than a second build (`docs/FRAME_TRACE_GUIDE.md`, `src/Magpie.Core/FrameTrace.cpp`) -- along with the caveats they state up front rather than discover: CPU wall clock is not GPU time, and nested durations must never be summed. |
| [2600th/dlss5-video-player](https://github.com/2600th/dlss5-video-player) | MIT | A per-megapixel GPU-cost floor to prove the model ran, since a pass that creates, evaluates and returns its input unchanged is invisible to every other counter (`src/NeuralRenderTypes.h:116-147`) — the method is taken, the constant is derived here. Also the forward/backward round-trip gate — a vector is evidence only if the backward vector where it lands comes back, judged against the energy of the pair (`src/FlowGate.h:33-47`), shipped inert because the thresholds are not calibrated (`src/NvofResolveShader.h:51-58`). Also the capability ladder: ask the optical-flow engine for the most it offers and give up one capability at a time, so a device that refuses an extra still gets the session it had (`src/OpticalFlowNvof.cpp:276-296`). Also that the flow grid should be upsampled with NEAREST rather than bilinear, because across a disocclusion the neighbouring cells describe different surfaces (`src/NvofResolveShader.h:41-45`); and that a scene-cut detector needs a strong arm that fires immediately and a debounced weak arm, and that a detected cut must raise the model's own history reset and not only clear the motion field (`src/TemporalGuides.cpp:72-116`). The §3.13 reasoning is NVIDIA's documentation. |
| [MagicalPrincessUnicorn/NeuRotic-an-OptiScaler-DLSSNR-fork](https://github.com/MagicalPrincessUnicorn/NeuRotic-an-OptiScaler-DLSSNR-fork) | GPL-3.0 | That a Present-time neural pass has no reset signal from the game, so its own continuity is the only one it can observe and every interruption must start the next frame fresh (`OptiScaler/dlssnr/DlssNr_PresentHistory.h:9-11`). |
| [onestep00/OptiScaler-Glass-Motion](https://github.com/onestep00/OptiScaler-Glass-Motion) | GPL-3.0 | That requiring a particular queue type for a timestamp query silently disables the measurement instead of failing the thing being measured, and that a GPU timer should state the scope of what it brackets (`OptiScaler/framegen/glass/GlassGpuTimer.h:11-14`, `:85-86`, `:132-135`). |
| [Blueforcer/ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) | MIT | Estimating optical flow at a fixed target WIDTH rather than at a fraction of the frame, so the cost is the same at 1080p and 4K, with the vectors rescaled by the actual ratio after even-rounding (`dlss5/motion.py:44-52`, `:88-94`). Also the `NVSDK_NGX_PerfQuality_Value` mapping, which is what showed that our `PerfQualityValue = 3` was commented "Balanced" when 3 is UltraPerformance (`dlss5/settings.py:28-35`). |
| [lrnolivia/RTXForge-MFG](https://github.com/lrnolivia/RTXForge-MFG) | GPL-3.0 | That a highlight guard of 1.0 makes the ratio clamp a constant and leaves every transfer mode producing the same picture, so a slider must not reach it (commit `208e64f6`). |
| [RH-RunningHub/ComfyUI-RH-DLSS5](https://github.com/RH-RunningHub/ComfyUI-RH-DLSS5) | MIT | That a Wine prefix must be validated by its CONTENTS rather than by `drive_c` existing (`dlss5nr/linux_backend.py:175-180`). Also that an inherited `LD_LIBRARY_PATH` (a CUDA install, typically) breaks the Wine-side NVIDIA shims, and that DXVK/VKD3D logging has to be silenced where the child's output is the log people attach to bug reports (`dlss5nr/linux_backend.py:199-205`). Also the adapter-pinning precedence — an explicit choice, then an ordinal, with the UUID filtered at the DXVK layer so the ordinal is trivially 0 (`dlss5nr/common.py:311-338`). |

## What Is Derived From RenoDX

The heart of the pass, including the two-branch luminance ratio, OkLab hue correction, blend between
a luminance-only result and the model's own colour, and reversible neutral-axis gamut compression,
is **clshortfuse's design**, from RenoDX's DLSS 5 addon. It reached this repository through
OptiScaler and was reimplemented here; different names do not make that design original work.

`third_party/optiscaler/RenoDX_ATTRIBUTION.txt` is upstream's own account of what is derived and what
is not, and carries the MIT license text that must ship with any build.

The OkLab conversion matrices are Bjorn Ottosson's published constants. The AP1 gamut matrices,
sRGB transfer functions, and SMPTE ST.2084 are standard colour science and are not specific to RenoDX.

The matched residual and its cube scaling are **hhkbble's**, from a multi-pass pull request against
the OptiScaler fork. They were reimplemented rather than merged and are recorded here and in the
source.

The third enlargement mode, **native + edit**, is the technique from **xen/xenmods'**
DLSSNR-Cost-Scaler (MIT): its `CS_Resolve` is where the additive rule and the shape of the luminance
guard come from. No code was copied; the branch in `dlssnr.hlsl` is a project implementation.

## The DLSS-NR Shader

`layer_linux/src/dlssnr/` holds the composition shader as `dlssnr.hlsl`, plus its compiled SPIR-V
and embedded header. **Editing the HLSL alone changes nothing**: the compiled module is what runs.
Rebuild it with `tools/gen_dlssnr_spv.sh`, which invokes `dxc` and embeds the resulting SPIR-V:

```sh
tools/gen_dlssnr_spv.sh
```

The checked-in shader and compiled module are a project-maintained derivative of the Dagherbou
import, with RenoDX-derived composition and project-specific additions. Matching `dxc` versions are
recommended because compiler versions may produce different but equivalent SPIR-V layouts.

`DlssNr_Layout.h` pins the constant block's offsets against the compiled module. If a re-vendor
changes the layout, that file stops compiling on purpose.

## Licensing Note

The copied or adapted OptiScaler and DLSS-NR portions are GPL-3.0. Distribution of a combined
derivative must comply with GPL-3.0 for those portions and preserve all applicable third-party
notices. RenoDX and DLSSNR-Cost-Scaler contributions are MIT; vkBasalt is zlib; Vulkan-Headers and
stb have the notices described above; and DXVK's license is shipped with its binary. DXVK-NVAPI is
downloaded only as an unmodified upstream runtime and remains under its MIT license.
