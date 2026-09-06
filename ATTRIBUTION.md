# Attribution

This fork carries DLSS 5 Neural Rendering work from several upstream projects. Everything below is
someone else's, and this file exists so that stays legible after the code has been adapted.

## The projects

| Project | Licence | What is taken |
|---|---|---|
| [OptiScaler](https://github.com/cdozdil/OptiScaler) | GPL-3.0 | `Shader_Vk` (the compute-shader base class), the output-scaling downscaler, the pattern scanner |
| [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) | GPL-3.0 | the DLSS-NR module: the composition shader and its constant block, the reversible proxy modes, frame hold, the apply-model toggle, supersampling, the white-point work |
| [y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG) | GPL-3.0 | multipass and per-pass model settings, the native-Vulkan feature path, MFG unlock |
| [RenoDX](https://github.com/clshortfuse/renodx) | MIT | the colour composition itself — see below |
| [xenmods/DLSSNR-Cost-Scaler](https://github.com/xenmods/DLSSNR-Cost-Scaler) | MIT | the native + edit enlargement — technique only, no code |
| [vkBasalt](https://github.com/DadSchoorse/vkBasalt) | zlib | referenced for correct layer structure; **no code taken** |
| [DLSS5VKLayer](https://github.com/bmitch87/DLSS5VKLayer) | — | the base project this forks |

Licence texts are in `third_party/`.

## The colour composition is RenoDX's

The heart of the pass — the two-branch luminance ratio, the OkLab hue correction, the blend between a
luminance-only result and the model's own colour, and the reversible neutral-axis gamut compression —
is **clshortfuse's design**, from RenoDX's DLSS 5 addon. It reached this repository through
OptiScaler, reimplemented there with different names, which does not make it anyone else's work.

`third_party/optiscaler/RenoDX_ATTRIBUTION.txt` is upstream's own account of exactly what is derived
and what is not, and it carries the MIT licence text that must ship with any build.

The OkLab conversion matrices are Björn Ottosson's published constants. The AP1 gamut matrices, the
sRGB transfer functions and SMPTE ST.2084 are standard colour science and specific to nobody.

The matched residual and its cube scaling are **hhkbble's**, from a multi-pass pull request against
the OptiScaler fork. It was reimplemented rather than merged, so there is no commit of theirs to
carry the credit; it is recorded here and in the source instead.

The third enlargement mode, **native + edit**, is the technique from **xen's** DLSSNR-Cost-Scaler
(MIT): its `CS_Resolve` is where the additive rule and the shape of the luminance guard come from. No
code was copied — the branch in `dlssnr.hlsl` is y4my4my4m's writing of it, carried here from
`nr-split-vulkan`.

## The vendored shader

`layer_linux/src/dlssnr/` holds the composition shader as a pair: `dlssnr.hlsl` as source, and
`DlssNr_Shader_Vk.spv` / `.h` as the compiled SPIR-V. **Editing the HLSL alone changes nothing** --
the module is what runs. Rebuild it with the same command upstream uses:

```
dxc -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi dlssnr.hlsl -Fo DlssNr_Shader_Vk.spv
python create_header.py DlssNr_Shader_Vk.spv DlssNr_Shader_Vk.h dlssnr_spv
```

`VK_MODE` is what strips the D3D12 game-exposure path, which is why this module has eight bindings
and not nine. `dxc` is in `directx-shader-compiler`; the fxc note in upstream's README applies only
to the Direct3D bytecode, which this project never builds.

`DlssNr_Layout.h` pins the constant block's offsets against the compiled module. If a re-vendor
changes the layout, that file stops compiling on purpose.

The `.spv` was originally copied from Dagherbou's tree. It is now built here, because the native +
edit branch came from a different fork than the rest of the shader and no upstream artifact contains
both. The command above reproduces the committed module byte for byte with
`dxc 1.9(1-0d3ee6b5)`; a different `dxc` will lay out the same program differently, which is
expected and harmless. Behaviour was checked against the artifact it replaces by capturing matched
input frames through the pass, and the difference sat inside the run-to-run noise of the pass itself
(see the commit that made the change).

## Licensing note

OptiScaler and both DLSS-NR forks are GPL-3.0, so a build combining this work must be distributed
under GPL-3.0. The base project carries no licence file of its own; that needs settling with its
author before any public release.
