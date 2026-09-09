#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# What to produce: tarballs only, RPMs only, or both. The RPMs install the staged
# tarball as their payload, so "rpm" still stages (and leaves behind) the tar.gz.
# Which variants to stage: public only, personal only, or both.
# DLSSNR_VARIANTS=public skips the -personal tarball/RPM (used by CI, which
# never has the proprietary NGX DLLs anyway).
MODE="${1:-both}"
case "$MODE" in
    tar|rpm|both) ;;
    *) echo "usage: $0 [tar|rpm|both]" >&2; exit 1 ;;
esac
VARIANTS="${DLSSNR_VARIANTS:-both}"
case "$VARIANTS" in
    public|personal|both) ;;
    *) echo "error: DLSSNR_VARIANTS must be public, personal or both" >&2; exit 1 ;;
esac

VERSION="${DLSSNR_VERSION:-$(sed -n 's/^Version:[[:space:]]*//p' packaging/dlssnr.spec | head -1)}"
VERSION="${VERSION:-0.2.5}"
RELEASE="${DLSSNR_RELEASE:-$(sed -n 's/^%{!?dlssnr_release: %global dlssnr_release \([^}]*\)}.*/\1/p' packaging/dlssnr.spec | head -1)}"
RELEASE="${RELEASE:-1}"
DIST="dist"
BUILD="${DLSSNR_BUILD_DIR:-build}"
export DLSSNR_SKIP_MANIFEST_INSTALL=1

[ -f "$BUILD/dlssnr_helper.exe" ] || ./build.sh
[ -f "$BUILD/runner_probe" ] || ./build.sh
[ -f "$BUILD/dlssnr-shmctl" ] || ./build.sh
[ -f "$BUILD/gui/dlssnr_gui" ] || ./build.sh

mkdir -p "$DIST"

stage_variant() {
  local variant="$1"
  local pkg_name="$2"
  local pkg_dir="$DIST/$pkg_name-$VERSION-$RELEASE-linux-x86_64"
  local root="$pkg_dir/root"

  rm -rf "$pkg_dir"
  mkdir -p \
    "$root/usr/lib64/dlssnr/layer" \
    "$root/usr/lib64/dlssnr/layer32" \
    "$root/usr/lib64/dlssnr/helper" \
    "$root/usr/lib64/dlssnr/bin" \
    "$root/usr/lib64/dlssnr/dxvk/2.7.1" \
    "$root/usr/bin" \
    "$root/usr/share/vulkan/implicit_layer.d" \
    "$root/usr/share/applications" \
    "$root/usr/share/doc/dlssnr"

  cp "$BUILD/layer/libVkLayer_NV_dlssnr.so" "$root/usr/lib64/dlssnr/layer/"
  # The 32-bit layer is optional: it only exists when a multilib toolchain was present at build time.
  if [ -f "$BUILD/layer32/libVkLayer_NV_dlssnr.so" ]; then
    cp "$BUILD/layer32/libVkLayer_NV_dlssnr.so" "$root/usr/lib64/dlssnr/layer32/"
  fi
  cp "$BUILD/dlssnr_helper.exe" "$root/usr/lib64/dlssnr/helper/"
  cp "$BUILD/runner_probe" "$root/usr/lib64/dlssnr/bin/"
  cp "$BUILD/dlssnr-shmctl" "$root/usr/lib64/dlssnr/bin/"
  cp "$BUILD/gui/dlssnr_gui" "$root/usr/bin/dlssnr-gui"
  cp dlssnr-helper "$root/usr/bin/dlssnr-helper"
  ln -sf ../lib64/dlssnr/bin/runner_probe "$root/usr/bin/dlssnr-runner-probe"
  ln -sf ../lib64/dlssnr/bin/dlssnr-shmctl "$root/usr/bin/dlssnr-shmctl"
  cp third_party/dxvk/2.7.1/x64/vulkan-1.dll "$root/usr/lib64/dlssnr/dxvk/2.7.1/"
  cp third_party/dxvk/2.7.1/LICENSE.txt "$root/usr/share/doc/dlssnr/dxvk-license.txt"
  cp packaging/dlssnr.desktop "$root/usr/share/applications/"
  cp packaging/install.sh packaging/uninstall.sh "$pkg_dir/"

  # One manifest per architecture, each naming its own library and its own layer name. The loader
  # keys implicit layers by name, so sharing one name means it keeps a single entry and then rejects
  # it for the wrong word size -- which is why Steam's overlay is _32 beside _64.
  sed -e "s#./libVkLayer_NV_dlssnr.so#/usr/lib64/dlssnr/layer/libVkLayer_NV_dlssnr.so#" \
      -e 's#"implementation_version"#"library_arch": "64",\n    "implementation_version"#' \
    layer_linux/manifest/VK_LAYER_NV_dlssnr.json \
    > "$root/usr/share/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.x86_64.json"

  if [ -f "$BUILD/layer32/libVkLayer_NV_dlssnr.so" ]; then
    sed -e "s#./libVkLayer_NV_dlssnr.so#/usr/lib64/dlssnr/layer32/libVkLayer_NV_dlssnr.so#" \
        -e 's#"VK_LAYER_NV_dlssnr"#"VK_LAYER_NV_dlssnr_32"#' \
        -e 's#"implementation_version"#"library_arch": "32",\n    "implementation_version"#' \
      layer_linux/manifest/VK_LAYER_NV_dlssnr.json \
      > "$root/usr/share/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.i686.json"
  fi

  chmod 755 "$pkg_dir/install.sh" "$pkg_dir/uninstall.sh"
  chmod 755 "$root/usr/bin/dlssnr-helper" "$root/usr/bin/dlssnr-gui"
  chmod 755 "$root/usr/lib64/dlssnr/bin/runner_probe" "$root/usr/lib64/dlssnr/bin/dlssnr-shmctl"
  chmod 755 "$root/usr/lib64/dlssnr/layer/libVkLayer_NV_dlssnr.so"
  chmod 755 "$root/usr/lib64/dlssnr/layer32/libVkLayer_NV_dlssnr.so" 2>/dev/null || true
  chmod 755 "$root/usr/lib64/dlssnr/helper/dlssnr_helper.exe"

  if [ "$variant" = "personal" ]; then
    mkdir -p "$root/usr/lib64/dlssnr/helper/binaries"
    cp binaries/*.dll "$root/usr/lib64/dlssnr/helper/binaries/" 2>/dev/null || true
    cp binaries/*.license.txt "$root/usr/lib64/dlssnr/helper/binaries/" 2>/dev/null || true
    chmod 644 "$root/usr/lib64/dlssnr/helper/binaries/"* 2>/dev/null || true
  fi

  tar -C "$DIST" -czf "$DIST/$pkg_name-$VERSION-$RELEASE-linux-x86_64.tar.gz" "$pkg_name-$VERSION-$RELEASE-linux-x86_64"
  echo "built $DIST/$pkg_name-$VERSION-$RELEASE-linux-x86_64.tar.gz"
}

build_rpm() {
  local spec="$1"
  local topdir="$PWD/$DIST/rpmbuild"
  mkdir -p "$topdir"
  rpmbuild --define "_topdir $topdir" --define "_sourcedir $PWD/$DIST" \
      --define "dlssnr_version $VERSION" --define "dlssnr_release $RELEASE" -bb "$spec"
  cp "$topdir"/RPMS/x86_64/*.rpm "$DIST/" 2>/dev/null || true
}

if [ "$VARIANTS" = "public" ] || [ "$VARIANTS" = "both" ]; then
    stage_variant public dlssnr
fi
if [ "$VARIANTS" = "personal" ] || [ "$VARIANTS" = "both" ]; then
    stage_variant personal dlssnr-personal
fi

if [ "$MODE" != "tar" ]; then
    if [ "$VARIANTS" = "public" ] || [ "$VARIANTS" = "both" ]; then
        build_rpm packaging/dlssnr.spec
    fi
    if [ "$VARIANTS" = "personal" ] || [ "$VARIANTS" = "both" ]; then
        build_rpm packaging/dlssnr-personal.spec
    fi
fi

echo
echo "artifacts:"
if [ "$MODE" != "rpm" ]; then
    ls -1 "$DIST"/dlssnr-*.tar.gz 2>/dev/null || true
fi
if [ "$MODE" != "tar" ]; then
    ls -1 "$DIST"/dlssnr-*.rpm 2>/dev/null || true
fi