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
| [Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation](https://github.com/Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation) | MIT | `cp --remove-destination` when installing DLLs into a Wine prefix: a plain `cp` writes *through* a prefix symlink into the file it points at (`docs/linux.md:186-190`). |
| [pcdofafa/dlss5-for-all](https://github.com/pcdofafa/dlss5-for-all) | MIT | Reporting Laplacian variance and Sobel gradient energy **together**, with the stated reason that they fail differently, and discarding a border before measuring (`tests/nitidez_png.cpp`). The metrics themselves are standard; `tools/capture_metrics.cpp` is a project implementation. |
| [LCPD15/DXL](https://github.com/LCPD15/DXL) | **AGPL-3.0** — findings only, no code | That a parameter container should record which keys the DLL actually *read*, because "set" looks like "in effect" and is not (`src/core/NgxParameterBag.h:126-146`). The probe in `core/ngx_param.h` is a project implementation written from the description; no AGPL code is present, and none may be taken. |

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
