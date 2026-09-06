#pragma once
// The C++ constant struct and the compiled shader's cbuffer, pinned together.
//
// DlssNr_Common.h and DlssNr_Shader_Vk.spv are vendored as a pair from upstream, and nothing in the
// build makes the host struct and the shader's uniform block agree -- a member added to one and not
// the other compiles, links, runs, and quietly feeds the shader the wrong numbers from that member
// onward. The failure looks like a settings bug, not a layout bug.
//
// These offsets were read out of the vendored module itself:
//
//     spirv-dis DlssNr_Shader_Vk.spv | grep 'OpMemberDecorate %type_Params'
//
// If a re-vendor changes the block, this stops compiling and whoever did it is pointed at the one
// command that says what the new layout is. That is the whole intent: it is cheaper to re-read the
// offsets than to debug a frame that is subtly wrong.
#include "DlssNr_Common.h"

#include <cstddef>

static_assert(sizeof(DlssNrConstants) == 256, "the cbuffer is declared alignas(256); see DlssNr_Common.h");

#define DLSSNR_PIN(member, offset)                                                                 \
    static_assert(offsetof(DlssNrConstants, member) == (offset),                                   \
                  "DlssNrConstants::" #member " no longer matches the vendored SPIR-V")

DLSSNR_PIN(Mode, 0);
DLSSNR_PIN(WhitePoint, 4);
DLSSNR_PIN(Width, 8);
DLSSNR_PIN(Height, 12);
DLSSNR_PIN(TransferStrength, 16);
DLSSNR_PIN(ColourStrength, 20);
DLSSNR_PIN(DebugView, 24);
DLSSNR_PIN(MaxRatio, 28);
DLSSNR_PIN(Passthrough, 32);
DLSSNR_PIN(MvScaleX, 36);
DLSSNR_PIN(MvScaleY, 40);
DLSSNR_PIN(GuideWidth, 44);
DLSSNR_PIN(GuideHeight, 48);
DLSSNR_PIN(CompareMode, 52);
DLSSNR_PIN(CompareSplit, 56);
DLSSNR_PIN(CompareZoom, 60);
DLSSNR_PIN(CompareSwap, 64);
DLSSNR_PIN(Transfer, 68);
DLSSNR_PIN(DebugScale, 72);
DLSSNR_PIN(ReversibleMode, 76);
DLSSNR_PIN(ApplyModel, 80);
// The last two are the D3D12 zero-latency exposure path, which VK_MODE compiles out of the shader.
// They stay in the struct because the block's layout is shared with the D3D12 build; the layer
// leaves UseGameExposure at 0 and the shader never reads either.
DLSSNR_PIN(UseGameExposure, 84);
DLSSNR_PIN(ExposurePreMul, 88);

#undef DLSSNR_PIN
