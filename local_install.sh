#!/bin/bash
set -euo pipefail

LIBDIR="$HOME/.local/lib/dlssnr"
BINDIR="$HOME/.local/bin"
DATADIR="$HOME/.local/share"
DOCDIR="$DATADIR/doc/dlssnr"
MANIFEST_DIR="$DATADIR/vulkan/implicit_layer.d"
APP_DIR="$DATADIR/applications"

mkdir -p "$LIBDIR/layer" "$LIBDIR/layer32" "$LIBDIR/helper" "$LIBDIR/bin"
mkdir -p "$BINDIR" "$MANIFEST_DIR" "$APP_DIR" "$DOCDIR"

# Replace by rename, never by writing over the file in place.
#
# A shared library is mmap'd by every process that loaded it, and cp truncates and rewrites the
# same inode -- so the pages under a running program change beneath it, which is a SIGBUS and not a
# subtle one. A rename leaves the old inode alone for whoever still has it open and points the name
# at the new one, so a game or player that is already running keeps the layer it started with and
# picks the new one up next time it starts. It also means an install no longer fails merely because
# the GUI happens to be open.
install_file() {
  src="$1"; dst="$2"
  cp -a "$src" "$dst.new.$$"
  mv -f "$dst.new.$$" "$dst"
}

install_file build/native/layer_linux/libVkLayer_NV_dlssnr.so "$LIBDIR/layer/libVkLayer_NV_dlssnr.so"
install_file build/linux32/layer_linux/libVkLayer_NV_dlssnr.so "$LIBDIR/layer32/libVkLayer_NV_dlssnr.so"
install_file build/windows/windows/dlssnr_helper.exe "$LIBDIR/helper/dlssnr_helper.exe"
install_file build/native/tools/runner_probe "$LIBDIR/bin/runner_probe"
install_file build/native/tools/dlssnr-shmctl "$LIBDIR/bin/dlssnr-shmctl"

install_file dlssnr-helper "$BINDIR/dlssnr-helper"
install_file build/native/gui/dlssnr_gui "$BINDIR/dlssnr-gui"
install_file packaging/dlssnr.desktop "$APP_DIR/dlssnr.desktop"

ln -sf ../lib/dlssnr/bin/runner_probe "$BINDIR/dlssnr-runner-probe"
ln -sf ../lib/dlssnr/bin/dlssnr-shmctl "$BINDIR/dlssnr-shmctl"

sed -e "s#./libVkLayer_NV_dlssnr.so#$LIBDIR/layer/libVkLayer_NV_dlssnr.so#" \
    -e 's#"implementation_version"#"library_arch": "64",\n    "implementation_version"#' \
    layer_linux/manifest/VK_LAYER_NV_dlssnr.json > "$MANIFEST_DIR/VK_LAYER_NV_dlssnr.x86_64.json"
sed -e "s#./libVkLayer_NV_dlssnr.so#$LIBDIR/layer32/libVkLayer_NV_dlssnr.so#" \
    -e 's#"VK_LAYER_NV_dlssnr"#"VK_LAYER_NV_dlssnr_32"#' \
    -e 's#"implementation_version"#"library_arch": "32",\n    "implementation_version"#' \
    layer_linux/manifest/VK_LAYER_NV_dlssnr.json > "$MANIFEST_DIR/VK_LAYER_NV_dlssnr.i686.json"

chmod 755 "$BINDIR/dlssnr-helper" "$BINDIR/dlssnr-gui" "$LIBDIR/bin/runner_probe" "$LIBDIR/bin/dlssnr-shmctl"
chmod 755 "$LIBDIR/layer/libVkLayer_NV_dlssnr.so" "$LIBDIR/layer32/libVkLayer_NV_dlssnr.so"
chmod 755 "$LIBDIR/helper/dlssnr_helper.exe"

echo "Installed successfully to $HOME/.local/"
