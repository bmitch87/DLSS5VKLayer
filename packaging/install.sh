#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
usage: $0 [--user|--system] [--prefix DIR]

installs DLSS5VKLayer from the packaged tarball
EOF
}

MODE="user"
PREFIX=""

while [ $# -gt 0 ]; do
  case "$1" in
    --user) MODE="user" ;;
    --system) MODE="system" ;;
    --prefix)
      PREFIX="${2:-}"
      if [ -z "$PREFIX" ]; then
        echo "error: --prefix requires a directory" >&2
        exit 1
      fi
      shift
      ;;
    --prefix=*) PREFIX="${1#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

root="$(cd "$(dirname "$0")" && pwd)/root"
[ -d "$root/usr" ] || { echo "error: missing root/usr in package" >&2; exit 1; }

if [ "$MODE" = "system" ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "error: --system requires root privileges" >&2
    exit 1
  fi
  LIBDIR="${PREFIX:-/usr}/lib64/dlssnr"
  BINDIR="${PREFIX:-/usr}/bin"
  DATADIR="${PREFIX:-/usr}/share"
  DOCDIR="$DATADIR/doc/dlssnr"
else
  if [ -n "$PREFIX" ]; then
    LIBDIR="$PREFIX/lib/dlssnr"
    BINDIR="$PREFIX/bin"
    DATADIR="$PREFIX/share"
    DOCDIR="$DATADIR/doc/dlssnr"
  else
    LIBDIR="$HOME/.local/lib/dlssnr"
    BINDIR="$HOME/.local/bin"
    DATADIR="$HOME/.local/share"
    DOCDIR="$DATADIR/doc/dlssnr"
  fi
fi

MANIFEST_DIR="$DATADIR/vulkan/implicit_layer.d"
APP_DIR="$DATADIR/applications"

mkdir -p "$LIBDIR" "$BINDIR" "$MANIFEST_DIR" "$APP_DIR" "$DOCDIR"

cp -a "$root/usr/lib64/dlssnr/." "$LIBDIR/"
cp -a "$root/usr/bin/dlssnr-helper" "$BINDIR/dlssnr-helper"
cp -a "$root/usr/bin/dlssnr-gui" "$BINDIR/dlssnr-gui"
cp -a "$root/usr/share/applications/dlssnr.desktop" "$APP_DIR/dlssnr.desktop"
[ -f "$root/usr/share/doc/dlssnr/dxvk-license.txt" ] && cp -a "$root/usr/share/doc/dlssnr/dxvk-license.txt" "$DOCDIR/dxvk-license.txt"

if [ "$MODE" = "system" ]; then
  ln -sf ../lib64/dlssnr/bin/runner_probe "$BINDIR/dlssnr-runner-probe"
else
  ln -sf ../lib/dlssnr/bin/runner_probe "$BINDIR/dlssnr-runner-probe"
fi

for arch_manifest in "$root"/usr/share/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.*.json; do
  [ -f "$arch_manifest" ] || continue
  sed -e "s#/usr/lib64/dlssnr/layer/#$LIBDIR/layer/#" \
      -e "s#/usr/lib64/dlssnr/layer32/#$LIBDIR/layer32/#" \
    "$arch_manifest" > "$MANIFEST_DIR/$(basename "$arch_manifest")"
done

# Older installs put a single, architecture-less manifest here. Left behind it would load the 64-bit
# layer a second time under a different name.
rm -f "$MANIFEST_DIR/VK_LAYER_NV_dlssnr.json"

chmod 755 "$BINDIR/dlssnr-helper" "$BINDIR/dlssnr-gui" "$LIBDIR/bin/runner_probe" "$LIBDIR/bin/dlssnr-shmctl" 2>/dev/null || true
chmod 755 "$LIBDIR/layer/libVkLayer_NV_dlssnr.so" "$LIBDIR/layer32/libVkLayer_NV_dlssnr.so" 2>/dev/null || true
chmod 755 "$LIBDIR/helper/dlssnr_helper.exe" 2>/dev/null || true

echo "installed DLSS5VKLayer to $LIBDIR"
echo "manifest: $MANIFEST_DIR/VK_LAYER_NV_dlssnr.json"
echo "run: dlssnr-helper init && dlssnr-helper start"
echo "or:  dlssnr-gui"