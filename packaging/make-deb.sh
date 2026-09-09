#!/usr/bin/env bash
# Build .deb packages from the staged tarball trees.
#
# Usage:
#   ./packaging/make-deb.sh
#
# This reuses the same staged payload the RPMs install
# (dist/<pkg>-<version>-<release>-linux-x86_64/root/usr),
# so run ./packaging/make-dist.sh deb first --
# this script does that for you if the tarballs are missing.
#
# Output:
#   dist/dlssnr_<version>-<release>_amd64.deb
#   dist/dlssnr-personal_<version>-<release>_amd64.deb
# (DLSSNR_VARIANTS=public builds only the public package.)
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="${DLSSNR_VERSION:-$(sed -n 's/^%{!?dlssnr_version: %global dlssnr_version \([^}]*\)}.*/\1/p' packaging/dlssnr.spec | head -1)}"
RELEASE="${DLSSNR_RELEASE:-$(sed -n 's/^%{!?dlssnr_release: %global dlssnr_release \([^}]*\)}.*/\1/p' packaging/dlssnr.spec | head -1)}"
VERSION="${VERSION:-0.2.5}"
RELEASE="${RELEASE:-1}"
MAINTAINER="${DEB_MAINTAINER:-DLSS5VKLayer <dlssnr@localhost>}"
DIST="dist"

# Which variants to package: public only, personal only, or both.
# DLSSNR_VARIANTS=public skips the -personal .deb (used by CI, which
# never has the proprietary NGX DLLs anyway).
VARIANTS="${DLSSNR_VARIANTS:-both}"
case "$VARIANTS" in
    public|personal|both) ;;
    *) echo "error: DLSSNR_VARIANTS must be public, personal or both" >&2; exit 1 ;;
esac

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "error: dpkg-deb not found (install dpkg-dev)" >&2
    exit 1
fi

# Stage the tarballs first if they are missing; this is a no-op otherwise.
need_stage=0
if [ "$VARIANTS" = "public" ] || [ "$VARIANTS" = "both" ]; then
    ls "$DIST"/dlssnr-"$VERSION"-"$RELEASE"-linux-x86_64.tar.gz >/dev/null 2>&1 || need_stage=1
fi
if [ "$VARIANTS" = "personal" ] || [ "$VARIANTS" = "both" ]; then
    ls "$DIST"/dlssnr-personal-"$VERSION"-"$RELEASE"-linux-x86_64.tar.gz >/dev/null 2>&1 || need_stage=1
fi
if [ "$need_stage" = 1 ]; then
    DLSSNR_VARIANTS="$VARIANTS" ./packaging/make-dist.sh tar
fi

build_deb() {
    local pkg_name="$1"      # dlssnr | dlssnr-personal
    local deb_name="$1"      # same (debian package name)
    local stage="$DIST/$pkg_name-$VERSION-$RELEASE-linux-x86_64"
    local work="$DIST/deb-$pkg_name"
    local deb="$DIST/${deb_name}_${VERSION}-${RELEASE}_amd64.deb"

    [ -d "$stage/root/usr" ] || { echo "error: missing $stage/root/usr" >&2; exit 1; }

    rm -rf "$work"
    mkdir -p "$work/DEBIAN"
    cp -a "$stage/root/usr" "$work/usr"

    local conflicts=""
    local provides=""
    local description=""
    if [ "$pkg_name" = "dlssnr-personal" ]; then
        conflicts="Conflicts: dlssnr
"
        provides="Provides: dlssnr
"
        description="DLSS5 Neural Rendering Vulkan layer and helper with bundled NGX DLLs.
 This personal package includes bundled NVIDIA NGX DLLs and should only
 be redistributed if you have the rights to do so."
    else
        description="DLSS5 Neural Rendering Vulkan layer and helper.
 Installs a Vulkan implicit layer that forwards presented frames
 to a Windows NGX helper running under Wine or a custom Proton
 compatibility tool. This public package does not include NVIDIA
 proprietary NGX DLLs."
    fi

    cat > "$work/DEBIAN/control" <<EOF
Package: $deb_name
Version: $VERSION-$RELEASE
Architecture: amd64
Maintainer: $MAINTAINER
Section: utils
Priority: optional
Depends: bash, libvulkan1 | libvulkan2, libqt6widgets6t64 | libqt6widgets6, pciutils
Recommends: wine
${conflicts}${provides}Description: $description
EOF

    # Same per-user layer-order snippet the RPM %post writes: an existing
    # file is never overwritten; prerm removes it only if still identical.
    cat > "$work/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
ldconfig || true
getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 {print $1 "|" $6}' | while IFS='|' read -r user home; do
    [ -d "$home" ] || continue
    group=$(id -gn "$user" 2>/dev/null) || group="$user"
    conf="$home/.config/environment.d/dlssnr.conf"
    if [ ! -e "$conf" ]; then
        install -d -m 0755 "$home/.config" "$home/.config/environment.d"
        printf 'VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"\n' > "$conf"
        chown "$user:$group" "$home/.config" "$home/.config/environment.d" "$conf"
        chmod 0644 "$conf"
    fi
done
EOF
    cat > "$work/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ]; then
    getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 {print $6}' | while IFS= read -r home; do
        conf="$home/.config/environment.d/dlssnr.conf"
        if [ -f "$conf" ] && [ "$(cat "$conf" 2>/dev/null)" = 'VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"' ]; then
            rm -f "$conf"
        fi
    done
fi
EOF
    cat > "$work/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
ldconfig || true
EOF
    chmod 755 "$work/DEBIAN/postinst" "$work/DEBIAN/prerm" "$work/DEBIAN/postrm"

    dpkg-deb --build --root-owner-group "$work" "$deb"
    echo "built $deb"
}

if [ "$VARIANTS" = "public" ] || [ "$VARIANTS" = "both" ]; then
    build_deb dlssnr
fi
if [ "$VARIANTS" = "personal" ] || [ "$VARIANTS" = "both" ]; then
    build_deb dlssnr-personal
fi

echo
echo "artifacts:"
ls -1 "$DIST"/*.deb 2>/dev/null || true
