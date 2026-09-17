#!/usr/bin/env bash
# Regenerate the composition shader's SPIR-V and its embedded header.
#
# The checked-in module is a GLSLC build, and this script used to prefer dxc.
#
# `spirv-dis DlssNr_Shader_Vk.spv | head -3` says "Generator: Google Shaderc over Glslang", not dxc,
# so the comment that used to sit here -- "compiled with dxc, exactly as upstream does it ...
# reproduces it byte for byte" -- was wrong about the file it was describing. On a machine with dxc
# installed the script silently produced a different compiler's module: 46468 bytes against 40992,
# a 5 KB diff in the one file that does all of this project's colour work, presented as if it were
# the same shader. That is not a regen anybody can review.
#
# So the compiler is now an explicit choice and the default is the one the module was built with.
# Today's glslc against the checked-in module differs in 39 lines of disassembly out of about seven
# thousand, all of them constant-folding precision on a handful of literals (0.18, 5.5555, a * 16)
# -- version drift, not a different shader.
#
#   SPV_COMPILER=glslc   (default) matches the checked-in module
#   SPV_COMPILER=dxc     a deliberate toolchain change; expect a whole-file diff and say so
#   GLSLC=/path, DXC=/path   override either binary
#
# The .h is regenerated from the .spv by hand rather than with xxd -i because the file's shape --
# twelve bytes a line, the array named dlssnr_spv -- is what the layer includes.
set -euo pipefail
cd "$(dirname "$0")/.."

WANT="${SPV_COMPILER:-glslc}"
GLSLC="${GLSLC:-$(command -v glslc || true)}"
DXC="${DXC:-build/dxc}"
if [ ! -x "$DXC" ]; then
    DXC="$(command -v dxc || true)"
fi

SRC=layer_linux/src/dlssnr/dlssnr.hlsl
SPV=layer_linux/src/dlssnr/DlssNr_Shader_Vk.spv
HDR=layer_linux/src/dlssnr/DlssNr_Shader_Vk.h

case "$WANT" in
  glslc)
    [ -n "$GLSLC" ] || { echo "glslc not found (set GLSLC=, or SPV_COMPILER=dxc)" >&2; exit 1; }
    echo "compiling with $GLSLC ($("$GLSLC" --version 2>&1 | head -1))"
    "$GLSLC" -x hlsl -fshader-stage=compute -DVK_MODE -fentry-point=CSMain -O -c -o "$SPV" "$SRC"
    ;;
  dxc)
    [ -n "${DXC:-}" ] && [ -x "$DXC" ] || { echo "dxc not found (set DXC=)" >&2; exit 1; }
    echo "compiling with $DXC -- NOT the compiler the checked-in module was built with;"
    echo "the diff will be the whole file, so say so in the commit."
    "$DXC" -spirv -D VK_MODE -T cs_6_0 -E CSMain -Fo "$SPV" "$SRC"
    ;;
  *)
    echo "SPV_COMPILER must be glslc or dxc" >&2; exit 1 ;;
esac

python3 - "$SPV" "$HDR" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
if len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
    sys.exit('not a SPIR-V module')
lines = []
for i in range(0, len(data), 12):
    chunk = data[i:i + 12]
    lines.append('    ' + ', '.join('0x%02x' % b for b in chunk) + ',')
body = '\n'.join(lines).rstrip(',')
open(sys.argv[2], 'w').write(
    '#pragma once\n\n'
    'inline static const unsigned char dlssnr_spv[] = {\n' + body + '\n};\n')
print('wrote %s (%d bytes SPIR-V)' % (sys.argv[2], len(data)))
EOF
