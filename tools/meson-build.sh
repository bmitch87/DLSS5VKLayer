#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

MESON=${MESON:-meson}
BUILD_ROOT=${DLSSNR_BUILD_ROOT:-build}

setup_and_compile() {
    local dir="$1"
    shift
    local config_stamp="$dir/.dlssnr-meson-config"
    local requested_config
    requested_config="$(printf '%s\n' "$@"; sha256sum meson/*.ini)"

    if [ -f "$dir/meson-private/coredata.dat" ] &&
       [ -f "$config_stamp" ] &&
       [ "$(cat "$config_stamp")" = "$requested_config" ]; then
        "$MESON" setup --reconfigure "$dir" "$@"
    elif [ -f "$dir/meson-private/coredata.dat" ]; then
        "$MESON" setup --wipe "$dir" "$@"
    else
        "$MESON" setup "$dir" "$@"
    fi
    "$MESON" compile -C "$dir"
    printf '%s' "$requested_config" > "$config_stamp"
}

setup_and_compile "$BUILD_ROOT/native" --native-file meson/native-clang.ini
setup_and_compile "$BUILD_ROOT/linux32" \
    --native-file meson/native-clang.ini \
    --cross-file meson/cross-clang-linux32.ini
setup_and_compile "$BUILD_ROOT/windows" \
    --native-file meson/native-clang.ini \
    --cross-file meson/cross-clang-mingw64.ini

echo "built:"
echo "  $BUILD_ROOT/native/layer_linux/libVkLayer_NV_dlssnr.so"
echo "  $BUILD_ROOT/linux32/layer_linux/libVkLayer_NV_dlssnr.so"
echo "  $BUILD_ROOT/windows/windows/dlssnr_helper.exe"
echo "  $BUILD_ROOT/windows/windows/smoke.exe"
echo "  $BUILD_ROOT/native/tools/runner_probe"
echo "  $BUILD_ROOT/native/tools/dlssnr-shmctl"
echo "  $BUILD_ROOT/native/gui/dlssnr_gui"
echo "  $BUILD_ROOT/native/test_gui/binder_test"
