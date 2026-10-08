#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
usage: $0 [--user|--system] [--purge]

removes DLSS5VKLayer installed by install.sh
EOF
}

MODE="user"
PURGE=false

while [ $# -gt 0 ]; do
  case "$1" in
    --user) MODE="user" ;;
    --system) MODE="system" ;;
    --purge) PURGE=true ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

if [ "$MODE" = "system" ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "error: --system requires root privileges" >&2
    exit 1
  fi
  rm -rf /usr/lib64/dlssnr
  rm -f /usr/bin/dlssnr-helper /usr/bin/dlssnr-gui /usr/bin/dlssnr-runner-probe /usr/bin/dlssnr-shmctl
  rm -f /usr/share/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.json
  rm -f /usr/share/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.*.json
  rm -f /usr/share/applications/dlssnr.desktop
  rm -rf /usr/share/doc/dlssnr
  rm -rf /usr/share/dlssnr/translations
  rm -f /etc/environment.d/dlssnr.conf
else
  user_manifest_dir="$HOME/.local/share/vulkan/implicit_layer.d"
  rm -rf "$HOME/.local/lib/dlssnr"
  rm -f "$HOME/.local/bin/dlssnr-helper" "$HOME/.local/bin/dlssnr-gui" "$HOME/.local/bin/dlssnr-runner-probe" "$HOME/.local/bin/dlssnr-shmctl"
  rm -f "$user_manifest_dir"/VK_LAYER_NV_dlssnr.json "$user_manifest_dir"/VK_LAYER_NV_dlssnr.*.json
  rm -f "$HOME/.local/share/applications/dlssnr.desktop"
  rm -rf "$HOME/.local/share/doc/dlssnr"
  rm -rf "$HOME/.local/share/dlssnr/translations"
  rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/environment.d/dlssnr.conf" "$HOME/.config/environment.d/dlssnr.conf"
fi

if [ "$PURGE" = true ]; then
  rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/dlssnr"
  rm -rf "$HOME/.config/dlssnr"
  rm -rf "${XDG_DATA_HOME:-$HOME/.local/share}/dlssnr"
  rm -rf "$HOME/.local/share/dlssnr"
  rm -rf "${XDG_STATE_HOME:-$HOME/.local/state}/dlssnr"
  rm -rf "$HOME/.local/state/dlssnr"
  if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    rm -rf "$XDG_RUNTIME_DIR/dlssnr"
  fi
  rm -rf "/tmp/dlssnr-$UID"
  rm -f "/tmp/dlssnr_shm.bin"
fi

echo "DLSS5VKLayer removed"