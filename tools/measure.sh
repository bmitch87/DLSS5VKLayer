#!/usr/bin/env bash
# measure.sh -- change one setting, prove the helper acted on it, and measure the result.
#
# Three traps have already invalidated measurements on other people's benches, and two of
# them are ours as well. They are what this script exists to close; the metrics are the
# easy part.
#
#   1. Writing a setting does not prove the consumer read it. "dlssnr-shmctl set ..."
#      returns instantly whatever the helper is doing -- it may be mid-rebuild, may have
#      the channel marked dead, may not be running at all. Whole runs have been executed
#      elsewhere against a value the consumer never picked up, and nothing said so. So
#      every round here waits for the helper's own log to echo the value it latched, and
#      ABORTS rather than reporting a number it cannot attribute.
#
#   2. Measure on a still target. With the scene moving, the model's temporal behaviour
#      enters the number and it stops being a measurement of the setting. We have a better
#      still target than a paused game: holdFrame freezes the frame the pass works on and
#      re-runs the encode, the model and the resolve over the SAME picture, so nothing
#      about the scene can vary at all.
#
#   3. Absolute frame rates on a shared GPU are worthless. This script does not report any;
#      if you add an fps phase, interleave A/B/A/B and compare medians.
#
# Methodology from dlss5-for-all's tests/medir-residual.ps1 (MIT). Nothing is copied.
#
# usage:
#   tools/measure.sh <setting> <value-a> <value-b> [frames]
#   tools/measure.sh guard 2 8
#   tools/measure.sh transfer 0 1 8
#
# env:
#   DLSSNR_SHM     mapping path            (default /tmp/dlssnr-$UID/shm.bin)
#   DLSSNR_LOG     helper log to watch     (default $XDG_STATE_HOME/dlssnr/helper.log)
#   SHMCTL         dlssnr-shmctl binary    (default: build tree, then PATH)
#   METRICS        capture_metrics binary  (default: build tree, then PATH)
#   CAPTURE_DIR    where the layer writes  (default $XDG_STATE_HOME/dlssnr/captures)
#   ECHO_TIMEOUT   seconds to wait for the helper to echo a latched value (default 10)

set -euo pipefail

setting="${1:-}"
value_a="${2:-}"
value_b="${3:-}"
frames="${4:-8}"

if [ -z "$setting" ] || [ -z "$value_a" ] || [ -z "$value_b" ]; then
  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
state="${XDG_STATE_HOME:-$HOME/.local/state}"

shm="${DLSSNR_SHM:-/tmp/dlssnr-$(id -u)/shm.bin}"
log="${DLSSNR_LOG:-$state/dlssnr/helper.log}"
capture_dir="${CAPTURE_DIR:-$state/dlssnr/captures}"
echo_timeout="${ECHO_TIMEOUT:-10}"

find_tool() {
  local name="$1"; shift
  if [ -n "${2:-}" ] && [ -x "${2:-}" ]; then printf '%s' "$2"; return 0; fi
  local c
  for c in "$here/build/native/tools/$name" "$here/build/$name" "$(command -v "$name" 2>/dev/null || true)"; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
  done
  return 1
}

shmctl="${SHMCTL:-$(find_tool dlssnr-shmctl || true)}"
metrics="${METRICS:-$(find_tool capture_metrics || true)}"

[ -n "$shmctl" ] || { echo "measure.sh: dlssnr-shmctl not found (set SHMCTL)" >&2; exit 1; }
[ -n "$metrics" ] || { echo "measure.sh: capture_metrics not found (set METRICS; ninja -C build/native)" >&2; exit 1; }
[ -f "$shm" ] || { echo "measure.sh: no mapping at $shm -- is anything running?" >&2; exit 1; }

# Settings the model latches when a feature is built. Only these produce a "create tuning"
# line, so only these can be confirmed that way; everything else is confirmed by reading
# the header back, which proves the write landed but not that the helper consumed it.
is_latched() {
  case "$1" in
    preset|style|automask|intensity|localtone|localstructure|skinstructure|passes) return 0 ;;
    *) return 1 ;;
  esac
}

header_value() {
  "$shmctl" "$shm" settings | sed -n "s/^$1=//p" | head -1
}

# Wait for the helper to log a create-tuning line newer than the mark we took before the
# write. Trap 1: without this the round is unattributable.
wait_for_echo() {
  local since="$1" deadline=$((SECONDS + echo_timeout))
  if [ ! -f "$log" ]; then
    echo "  ! no helper log at $log; cannot confirm the helper acted" >&2
    return 1
  fi
  while [ "$SECONDS" -lt "$deadline" ]; do
    if tail -c +"$since" "$log" | grep -q '\[params\] create tuning:'; then
      tail -c +"$since" "$log" | grep '\[params\] create tuning:' | tail -1 | sed 's/^/  echo: /'
      return 0
    fi
    sleep 0.25
  done
  return 1
}

round() {
  local value="$1"
  echo "== $setting = $value =="

  local mark=1
  [ -f "$log" ] && mark=$(( $(wc -c < "$log") + 1 ))

  "$shmctl" "$shm" set "$setting" "$value" >/dev/null

  local back
  back="$(header_value "$setting" || true)"
  if [ -z "$back" ]; then
    echo "  ! '$setting' is not a known setting" >&2
    exit 1
  fi
  echo "  header now reports $setting=$back"

  if is_latched "$setting"; then
    if ! wait_for_echo "$mark"; then
      echo "  ! the helper did not echo a rebuild within ${echo_timeout}s." >&2
      echo "  ! DISCARDING this round rather than reporting a number it cannot attribute." >&2
      echo "  ! (helper not running? channel dead? rebuildms set very high?)" >&2
      exit 1
    fi
  else
    # A live setting produces no create-tuning line. Give the layer a moment to compose at
    # least one frame with it, then rely on the capture itself as the evidence.
    sleep 1
  fi

  "$shmctl" "$shm" capture "$frames" >/dev/null
  echo "  waiting for $frames captured pairs..."
  local deadline=$((SECONDS + 30))
  while [ "$SECONDS" -lt "$deadline" ]; do
    [ -f "$capture_dir/manifest.txt" ] && break
    sleep 0.5
  done
  if [ ! -f "$capture_dir/manifest.txt" ]; then
    echo "  ! no capture appeared in $capture_dir." >&2
    echo "  ! The layer writes captures, not the helper -- is a game presenting?" >&2
    exit 1
  fi

  "$metrics" "$capture_dir" | sed 's/^/  /'
  echo
}

echo "mapping   $shm"
echo "log       $log"
echo "captures  $capture_dir"
echo

# Trap 2: hold the frame, so the only thing that changes between the two rounds is the
# setting. Everything downstream of the capture still runs, so the picture is re-composed
# rather than merely re-shown.
echo "holding the frame (trap 2: a still target)"
"$shmctl" "$shm" set hold 1 >/dev/null
trap '"$shmctl" "$shm" set hold 0 >/dev/null 2>&1 || true' EXIT

before="$(header_value "$setting" || true)"
echo "starting value: $setting=$before"
echo

round "$value_a"
round "$value_b"

echo "restoring $setting=$before"
"$shmctl" "$shm" set "$setting" "$before" >/dev/null

echo
echo "Read the two tables together, not separately:"
echo "  * Laplacian up and Sobel flat  -> the change added noise, not detail."
echo "  * both up                      -> more real high-frequency content."
echo "  * PSNR is between before_ and after_ WITHIN a round, so it measures how far the"
echo "    pass moved that round's picture -- not how the two rounds differ from each other."
