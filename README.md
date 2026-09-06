# DLSS5VKLayer

DLSS5VKLayer is a Linux Vulkan layer plus helper service that forwards presented frames to a Windows NGX neural-rendering helper running under Wine or a custom Proton compatibility tool.

This project is experimental. It is intended for local testing and research.

## Features

- Vulkan implicit layer for native Linux games and Proton games.
- Fail-open design: if the helper is missing or neural initialization fails, games continue presenting original frames.
- Windows NGX helper runs under:
  - custom Steam compatibility tools such as Proton-CachyOS, Proton-GE, Wine-GE, and similar tools
  - system Wine with a managed prefix and vendored DXVK 2.7.1
- Synthetic motion vectors generated with `VK_NV_optical_flow` when available.
- Qt GUI for live controls:
  - enable/disable neural processing
  - multi-pass rendering
  - DLSS5 Native/Natural/Cinematic presets
  - intensity, tone, structure, skin structure, and sharpness
  - per-pass overrides
  - synthetic motion-vector enable, scale mode, and quality
- CLI helper manager:
  - runner discovery
  - start/stop/status
  - diagnostics
  - NVIDIA NGX binary import
- XDG-aware paths:
  - config: `$XDG_CONFIG_HOME/dlssnr/config.ini`
  - runtime/SHM/PID: `/tmp/dlssnr-$UID/` (not `$XDG_RUNTIME_DIR`, which Steam's container makes private)
  - logs/state: `$XDG_STATE_HOME/dlssnr/`, fallback `~/.local/state/dlssnr/`
  - managed prefix: `~/.local/share/dlssnr/prefix/`

## Requirements

- NVIDIA GPU and NVIDIA driver.
- Vulkan loader.
- Qt 6 for the GUI.
- One of:
  - a custom Steam compatibility tool that bundles DXVK-NVAPI, such as Proton-CachyOS or Proton-GE
  - system Wine for the fallback path

Valve's official Proton releases and Proton Experimental are not targeted as primary runners because they do not bundle the required DXVK-NVAPI stack.

## NVIDIA NGX DLLs

The public package does not include NVIDIA proprietary NGX DLLs.

You must provide the required DLLs yourself, for example:

- `nvngx_dlssnr.dll`
- `nvngx.dll`
- `nvapi64.dll`
- `sl.*.dll`

Import them with:

```bash
dlssnr-helper import-binaries /path/to/dlls
```

or use the GUI import flow if available.

The personal package variant includes these DLLs. Only redistribute the personal variant if you have the rights to do so.

## Install From RPM

Public package:

```bash
sudo rpm -Uvh dist/dlssnr-0.2.2-3.fc44.x86_64.rpm
```

Personal package:

```bash
sudo rpm -Uvh dist/dlssnr-personal-0.2.2-3.fc44.x86_64.rpm
```

`wine` is a recommended package, not a hard dependency, so Proton-only users are not forced to install host Wine.

## Install From Tarball

Extract the tarball:

```bash
tar -xzf dist/dlssnr-0.2.2-3-linux-x86_64.tar.gz
cd dlssnr-0.2.2-3-linux-x86_64
```

User install, no root required:

```bash
./install.sh --user
```

System install:

```bash
sudo ./install.sh --system
```

For user installs, make sure `~/.local/bin` is in your `PATH`.

## First Run

Initialize configuration:

```bash
dlssnr-helper init
dlssnr-helper doctor
```

Start the helper:

```bash
dlssnr-helper start
```

Check status:

```bash
dlssnr-helper status
```

Stop the helper:

```bash
dlssnr-helper stop
```

GUI:

```bash
dlssnr-gui
```

## Game Usage

Launch a game with the layer enabled:

```bash
VKLayer_DLSS5=1 ./your_native_game
```

For Steam:

```text
VKLayer_DLSS5=1 %command%
```

The layer is disabled unless `VKLayer_DLSS5=1` is present.

The layer is intended to coexist with the Steam overlay. If a game crashes during Vulkan device creation, make sure you are using `0.2.1-3` or newer.

## Steam / Proton Containers

Steam games run inside the Steam Linux Runtime / `pressure-vessel` container, which gives the game a
**private** `tmpfs` at `$XDG_RUNTIME_DIR`. Anything the helper puts there is simply not present inside
the game, so the layer would create its own empty mapping at a path that reads identically in the log
and then wait forever for a helper that is answering on the other file:

```text
[dlssnr-layer] [shm] attached /run/user/1000/dlssnr/shm.bin seq_req=0 seq_resp=0
[dlssnr-layer] [shm] no answer in 20 ms x4 (helper not running); passing frames through
```

For that reason the mapping lives in **`/tmp/dlssnr-$UID/`**, which the container bind-mounts from the
host. Nothing needs to be added to the launch options for this; `VKLayer_DLSS5=1 %command%` is enough.

If you saw the symptom above with an older build, remove the mapping it left behind:

```bash
rm -rf "$XDG_RUNTIME_DIR/dlssnr"
```

`dlssnr-helper doctor` reports a leftover if one is still there.

### Custom `DLSSNR_SHM` paths

If you point `DLSSNR_SHM` somewhere else, that directory has to be visible inside the container as
well. Either keep it under `/tmp`, or expose it:

```text
PRESSURE_VESSEL_FILESYSTEMS_RW=/home/USERNAME/.local/share/dlssnr VKLayer_DLSS5=1 %command%
```

## Runner Discovery

Custom compatibility tools are discovered from:

```text
$XDG_DATA_HOME/Steam/compatibilitytools.d
~/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d
~/snap/steam/common/.local/share/Steam/compatibilitytools.d
```

List discovered runners:

```bash
dlssnr-helper runners
```

You can also set a custom runner path manually in:

```text
~/.config/dlssnr/config.ini
```

Example:

```ini
runner_type=proton
runner_path=/path/to/compatibilitytools.d/Proton-CachyOS/proton
```

## Diagnostics

Useful commands:

```bash
dlssnr-helper doctor
dlssnr-helper runners
dlssnr-helper status
dlssnr-helper config
```

If you previously used an older build, remove stale shared-memory files:

```bash
rm -f /tmp/dlssnr_shm.bin
rm -rf "${XDG_RUNTIME_DIR:-/nonexistent}/dlssnr"
```

Current builds use `/tmp/dlssnr-$UID/shm.bin` by default, so neither games nor the helper need `DLSSNR_SHM` set manually -- including under Steam's container.

Logs are written to the XDG state directory:

```text
$XDG_STATE_HOME/dlssnr/helper.log
```

Fallback:

```text
~/.local/state/dlssnr/helper.log
```

## Building From Source

Install build dependencies:

- `gcc-c++`
- `mingw64-gcc-c++`
- `qt6-qtbase-devel`
- `vulkan-loader-devel` or equivalent Vulkan headers, though Vulkan headers are vendored

Build:

```bash
./build.sh
```

Outputs:

```text
build/layer/libVkLayer_NV_dlssnr.so
build/dlssnr_helper.exe
build/runner_probe
build/gui/dlssnr_gui
```

## GUI Settings

Start the GUI with:

```bash
dlssnr-gui
```

Every setting is a row in the scrolling list, grouped by what it acts on. The model's own profile is
under **Model**, as `Style`:

```text
Default     maps to DLSSNR.Style 0
Natural     maps to DLSSNR.Style 1
Cinematic   maps to DLSSNR.Style 2
```

Motion vectors are under **Motion**:

```text
Estimate motion vectors   on by default
Motion quality            Fast, Balanced (default), Quality
Motion units              Normalised, Pixels (default), UV 0..1
```

The Passes dialog overrides settings for one pass at a time, field by field: each row has its own
checkbox, and a field left unchecked follows the global value rather than restating it.

Settings are written to shared memory and take effect on the next processed frame. Closing the GUI requests a helper stop, so the helper does not need to be stopped manually.

## Synthetic Motion Vectors

The helper can generate screen-space motion vectors between `Frame[N-1]` and `Frame[N]` and bind them to `DLSSNR.MVec` before calling Feature 18.

Current behavior:

- `DLSSNR.Depth` is left as `nullptr`.
- `DLSSNR.UseAutoMask` is set to `1`.
- `DLSSNR.Reset` is set on the first frame and on simple CPU-detected scene cuts.
- NVIDIA Optical Flow (`VK_NV_optical_flow`) is used when available.
- The helper keeps optical-flow input images in VRAM and runs the flow pass before `VULKAN_EvaluateFeature(18)`.
- `DLSSNR.MVec` is filled as `R16G16_SFLOAT` half-float vectors in source-pixel units, current-frame-to-previous-frame by default.
- If the optical-flow output format is not directly accepted by NGX, the helper tries a direct GPU blit, then a hybrid low-res CPU conversion plus GPU float upscale, then full CPU conversion.
- Motion-vector quality controls the NVOF performance level and output grid: Fast prefers a smaller grid, Balanced uses a medium grid, and Quality prefers full-resolution flow.

Environment controls:

```text
DLSSNR_MVEC=0                 disable synthetic motion vectors
DLSSNR_MVEC_GPU=0             force CPU flow conversion (diagnostics)
DLSSNR_MVEC_DIRECTION=0       use previous-to-current flow direction instead of current-to-previous
DLSSNR_MVEC_FILTER=1          force linear GPU upscale filter when supported
DLSSNR_MVEC_DEBUG=1           log first few flow/MVec statistics
DLSSNR_SCENE_CUT=0            disable scene-cut reset detection
DLSSNR_SCENE_CUT_THRESHOLD=55 mean luma difference threshold
```

Build:

```bash
DLSSNR_SKIP_MANIFEST_INSTALL=1 ./build.sh
```

Test parameter ingestion:

```bash
DLSSNR_HELPER_EXE="$PWD/build/dlssnr_helper.exe" DLSSNR_VERBOSE=1 DLSSNR_TIME=1 ./dlssnr-helper start
WINEPREFIX="$HOME/.local/share/dlssnr/prefix/pfx" \
PROTON_ENABLE_NVAPI=1 \
STEAM_COMPAT_DATA_PATH="$HOME/.local/share/dlssnr/prefix" \
VKLayer_DLSS5=1 \
DLSSNR_SMOKE_FRAMES=5 \
"/home/hunter/.local/share/Steam/compatibilitytools.d/Proton-CachyOS Latest/proton" run "$PWD/build/smoke.exe"
```

Expected helper log lines:

```text
[mvec] NV optical flow enabled ... gpu_convert=1 dir=1 xfer=1
[params] evaluate contract set: ok ... UseAutoMask=1 ... depth=null
[params] MVecScaleX=1.000000 MVecScaleY=1.000000
[mvec] first optical-flow pass completed
[time] passes=1 ... flow=... eval=... total=... ms
```

If optical flow is unavailable, the helper falls back to zero motion vectors and continues without disabling neural processing.

## Packaging

Build public and personal tarballs plus RPMs:

```bash
./packaging/make-dist.sh
```

Artifacts are written to `dist/`.

The public package does not include NVIDIA DLLs. The personal package does.

## Uninstall

RPM:

```bash
sudo dnf remove dlssnr
```

or:

```bash
sudo dnf remove dlssnr-personal
```

Tarball user install:

```bash
./uninstall.sh --user
```

Tarball system install:

```bash
sudo ./uninstall.sh --system
```

Add `--purge` to also remove user config, state, runtime data, and the managed prefix.

## Troubleshooting

If the helper starts and immediately logs `shutting down`, update to `0.2.1-3` or newer and remove stale runtime state:

```bash
rm -f "/tmp/dlssnr-$UID/shm.bin"
```

If a Steam game crashes in `steamoverlayvulkanlayer.so`, update to `0.2.1-3` or newer.

If a Steam/Proton game logs `no answer ... (helper not running)` while the helper is waiting for
frames, it is on a different mapping than the helper -- see [Steam / Proton
Containers](#steam--proton-containers).

## Important Notes

- The helper and game run in separate processes, so frames cross a GPU/CPU/GPU path. This is not a zero-copy integration.
- The layer currently assumes a present-time swapchain layout that works for the tested games and emulators. Some games may need layout handling work.
- Steam runtime issues may require per-game or per-runtime debugging.
- Do not use the personal package publicly unless you are certain you may redistribute the bundled NVIDIA binaries.