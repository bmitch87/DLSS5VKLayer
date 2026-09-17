#!/usr/bin/env bash
# reconstruct_ab.sh -- settle the reconstruction filter on YOUR content.
#
# Five projects hold five positions on how the model's answer should be enlarged when it ran below
# the frame's resolution, and not one of them shipped a picture. This takes the picture.
#
# What it does, with a game already running under the layer:
#
#   1. Holds a frame, so every round below composes the SAME source picture. Without this the
#      rounds are different frames of a moving scene and none of the numbers mean anything.
#   2. Captures at 100% model resolution. That is the REFERENCE -- the answer with no enlargement
#      in it at all, which is what every candidate is trying to be.
#   3. Captures at the reduced resolution once per filter.
#   4. Reports each candidate against the reference.
#
# Read it as: higher PSNR is closer to the reference. The sharpness ratios say which WAY a
# candidate differs, because two filters can be equally far from the reference with one of them
# softer -- and "sharper but further away" is a real answer, not a better score.
#
# usage:
#   tools/reconstruct_ab.sh [scale] [frames]
#   tools/reconstruct_ab.sh 0.5 8
#   tools/reconstruct_ab.sh 0.75
#
# env: the same as measure.sh (DLSSNR_SHM, SHMCTL, METRICS, CAPTURE_DIR, OUT_DIR).
set -euo pipefail

scale="${1:-0.5}"
frames="${2:-8}"

here="$(cd "$(dirname "$0")/.." && pwd)"
state="${XDG_STATE_HOME:-$HOME/.local/state}"
shm="${DLSSNR_SHM:-/tmp/dlssnr-$(id -u)/shm.bin}"
capture_dir="${CAPTURE_DIR:-$state/dlssnr/captures}"
out_dir="${OUT_DIR:-$state/dlssnr/reconstruct-ab}"

find_tool() {
  local c
  for c in "$here/build/native/tools/$1" "$here/build/$1" "$(command -v "$1" 2>/dev/null || true)"; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
  done
  return 1
}
shmctl="${SHMCTL:-$(find_tool dlssnr-shmctl || true)}"
metrics="${METRICS:-$(find_tool capture_metrics || true)}"
[ -n "$shmctl" ]  || { echo "reconstruct_ab.sh: dlssnr-shmctl not found (set SHMCTL)" >&2; exit 1; }
[ -n "$metrics" ] || { echo "reconstruct_ab.sh: capture_metrics not found (set METRICS)" >&2; exit 1; }
[ -f "$shm" ]     || { echo "reconstruct_ab.sh: no mapping at $shm -- is anything running?" >&2; exit 1; }

# The composition has to be ON. With it bypassed the model's raw answer is presented and the
# reconstruction filter is not in the picture at all -- which would produce four identical rounds
# and look like a result.
bypass="$("$shmctl" "$shm" settings | sed -n 's/^bypass=//p')"
if [ "$bypass" != "0" ]; then
  echo "reconstruct_ab.sh: the composition is bypassed (bypass=$bypass), so the reconstruction" >&2
  echo "filter is not used. Run '$shmctl $shm set bypass 0' first." >&2
  exit 1
fi

was_scale="$("$shmctl" "$shm" settings | sed -n 's/^workingscale=//p')"
was_recon="$("$shmctl" "$shm" settings | sed -n 's/^reconstruct=//p')"
was_hold="$("$shmctl" "$shm" settings | sed -n 's/^hold=//p')"
restore() {
  "$shmctl" "$shm" set workingscale "$was_scale" >/dev/null 2>&1 || true
  "$shmctl" "$shm" set reconstruct "$was_recon" >/dev/null 2>&1 || true
  "$shmctl" "$shm" set hold "$was_hold" >/dev/null 2>&1 || true
}
trap restore EXIT

rm -rf "$out_dir"
mkdir -p "$out_dir"

# Take a round: set the scale and filter, capture, and move the result somewhere it will survive
# the next round. Aborts rather than reporting a round whose capture never appeared.
round() {
  local name="$1" sc="$2" rc="$3"
  "$shmctl" "$shm" set workingscale "$sc" >/dev/null
  "$shmctl" "$shm" set reconstruct "$rc" >/dev/null

  # A model-raster change makes the helper release its feature and build a new one, which takes
  # about a tenth of a second plus the rebuild spacing -- and occasionally does not come back at
  # all (see the note at the end of this file). Waited for rather than slept through: the round is
  # abandoned if the helper stops answering, instead of capturing frames the model never touched.
  local deadline=$((SECONDS + 25)) ok=""
  while [ "$SECONDS" -lt "$deadline" ]; do
    local req resp
    req=$("$shmctl" "$shm" status | sed -n 's/^seq_req=//p')
    resp=$("$shmctl" "$shm" status | sed -n 's/^seq_resp=//p')
    if [ -n "$req" ] && [ -n "$resp" ] && [ "$req" -le "$((resp + 2))" ]; then ok=1; break; fi
    sleep 0.5
  done
  [ -n "$ok" ] || {
    echo "  ! the helper stopped answering after the raster change (seq_req ran away from" >&2
    echo "  ! seq_resp). Restart it -- and kill any leftover Wine process first, because a new" >&2
    echo "  ! 'start' will otherwise attach to the wedged one." >&2
    exit 1
  }
  sleep 1                        # and let the composition settle at the new size
  rm -rf "$capture_dir"
  "$shmctl" "$shm" capture "$frames" >/dev/null
  local deadline=$((SECONDS + 30))
  while [ "$SECONDS" -lt "$deadline" ]; do
    [ -f "$capture_dir/manifest.txt" ] && break
    sleep 0.5
  done
  [ -f "$capture_dir/manifest.txt" ] || {
    echo "  ! no capture appeared for '$name'. The layer writes captures, not the helper --" >&2
    echo "  ! is a game still presenting?" >&2
    exit 1
  }
  sleep 1                        # let the last pair finish landing
  cp -r "$capture_dir" "$out_dir/$name"
  echo "  captured $name (scale $sc, filter $rc)"
}

echo "mapping   $shm"
echo "captures  $out_dir"
echo
echo "holding the frame, so every round below is the same picture"
"$shmctl" "$shm" set hold 2 >/dev/null     # arm: the next completed frame is the one held
sleep 2
echo

round reference   1.0    "$was_recon"
round bilinear    "$scale" 0
round nearest     "$scale" 1
round catmullrom  "$scale" 2

for cand in bilinear nearest catmullrom; do
  echo
  echo "================ $cand at $scale against the 100% answer ================"
  "$metrics" "$out_dir/reference" "$out_dir/$cand" | sed -n '/^mean/,$p'
done

echo
echo "The reference is the model's answer with no enlargement in it. A candidate that is closer to"
echo "it reconstructed better; one that is further away but sharper reconstructed differently, and"
echo "which of those you want is a judgement this script cannot make for you."

# A note on the one thing that can stop this run.
#
# Changing the model raster makes the helper release its NGX feature and re-enter the whole load
# and init path -- LoadLibraryEx, GetProcAddress, VULKAN_Init_Ext -- on an NGX that is already
# initialised. That INTERMITTENTLY wedges: the helper logs "nvngx_dlssnr.dll loaded at ..." and
# never reaches the next line, the layer's round trips time out, and the pass stops for the rest of
# the session. Seen three times across 1280x720 -> 500x500 and 500x500 -> 250x250, and not seen on
# several other runs of the same changes, so it is a race rather than a rule.
#
# Two things make it confusing rather than merely annoying. The watchdog does not fire, because the
# hang is not inside a guarded NGX call it is timing. And a wedged helper leaves a Wine process
# behind that the next "dlssnr-helper start" attaches to, so the restart appears to wedge
# immediately -- the giveaway is "nvngx_dlssnr.dll was already loaded in this process" on what
# should be a first load. Kill the leftover process before restarting.
#
# The rounds above check for it rather than reporting numbers from a model that stopped answering.
