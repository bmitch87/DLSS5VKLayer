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
- HDR input: on an HDR swapchain the model is shown a float16 proxy of the frame's real light --
  PQ-decoded first when the swapchain carries PQ -- instead of a tone-mapped 8-bit copy.
- Qt GUI for live controls:
  - enable/disable neural processing
  - multi-pass rendering
  - DLSS5 Native/Natural/Cinematic presets
  - intensity, tone, structure, skin structure, and sharpness
  - per-pass overrides
  - synthetic motion-vector enable, scale mode, and quality
  - importing the NVIDIA NGX DLLs (gear menu -> **NGX binaries**)
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

Only `nvngx_dlssnr.dll` is required -- neural processing stays off without it. The rest are optional and
depend on the runner:

- `nvngx_dlssnr.dll` -- required (the model itself)
- `nvngx.dll`, `nvapi64.dll` -- used by the core/NVAPI path; optional under Proton, where DXVK-NVAPI
  already supplies NVAPI (the helper skips the vendored `nvapi64.dll` there)
- `sl.*.dll` -- Streamline DLLs; not loaded by this build

Import them with the CLI:

```bash
dlssnr-helper import-binaries /path/to/dlls
```

or from the GUI: gear menu -> **NGX binaries** -> **Import binaries...**. On first launch the GUI also
prompts to import when `nvngx_dlssnr.dll` is missing. Either way the files are copied into
`$XDG_DATA_HOME/dlssnr/binaries`; restart the helper afterwards to load them.

The personal package variant includes these DLLs. Only redistribute the personal variant if you have the rights to do so.

## Install From RPM

The RPMs are the packaged builds under `dist/`, produced by [Packaging](#packaging). Install with
`dnf` so the dependencies (Qt 6, the Vulkan loader) come with it:

Public package:

```bash
sudo dnf install ./dist/dlssnr-[0-9]*.x86_64.rpm
```

Personal package:

```bash
sudo dnf install ./dist/dlssnr-personal-*.x86_64.rpm
```

`wine` is a recommended package, not a hard dependency, so Proton-only users are not forced to install host Wine.

Installing also writes `~/.config/environment.d/dlssnr.conf` for each user -- a systemd user-session
snippet pinning `VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"` so this layer orders
correctly alongside Smooth Motion. It takes effect on the next login, never overwrites an existing
file, and is removed on uninstall if you have not edited it.

### Updating an RPM Install

Stop the helper first if it is running, then upgrade in place -- user config, state and the managed
prefix survive the update:

```bash
dlssnr-helper stop
sudo dnf upgrade ./dist/dlssnr-[0-9]*.x86_64.rpm
```

(`rpm -Uvh ./dist/dlssnr-[0-9]*.x86_64.rpm` does the same job on systems without `dnf`.)
Relaunch any game that was presenting through the layer so it picks up the new layer library.

The two variants carry the same files and conflict with each other, so switching between them is a
swap, not an install:

```bash
sudo dnf swap dlssnr dlssnr-personal      # or the other way round
```

## Install From Tarball

The tarball runs on any x86_64 glibc distro -- Fedora, openSUSE, Debian and friends, and Arch and
CachyOS. Since `0.2.5-2` the GUI binary reaches Qt's meta-object data symbols through the GOT, so it
loads against both the default-visibility Qt (Fedora) and the protected-visibility Qt (Arch/CachyOS);
older tarballs die at exec on Arch-based systems with
`GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS`.

Extract the tarball (`*` matches whatever version you just built; use the
`dlssnr-personal-*` tarball for the personal variant):

```bash
tar -xzf dist/dlssnr-[0-9]*-linux-x86_64.tar.gz
cd dlssnr-*-linux-x86_64
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

`install.sh` also writes `~/.config/environment.d/dlssnr.conf` (system installs:
`/etc/environment.d/dlssnr.conf`) pinning `VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"`
so this layer orders correctly alongside Smooth Motion. It takes effect on the next login.

### Updating a Tarball Install

`install.sh` overwrites the files it owns in place, so an update is just the new tarball installed
over the old one -- user config, state and the managed prefix are not touched:

```bash
dlssnr-helper stop
tar -xzf dist/dlssnr-[0-9]*-linux-x86_64.tar.gz
cd dlssnr-*-linux-x86_64
./install.sh --user        # or: sudo ./install.sh --system
```

Relaunch any game that was presenting through the layer so it picks up the new layer library.

Stick to one install mode and one packaging format. The `--user` and `--system` trees and the tarball
and the RPM all own the same layer manifest, and the loader reads whichever it finds first -- if you
switched modes or came from the RPM, remove the other copy first (`sudo ./uninstall.sh --system`, or
`sudo dnf remove dlssnr`).

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
$XDG_DATA_DIRS/steam/compatibilitytools.d (each entry, plus /usr/local/share and /usr/share always)
```

System-wide tools (CachyOS ships Proton-CachyOS under `/usr/share/steam/compatibilitytools.d`) are
discovered from every directory in `$XDG_DATA_DIRS` and, because Steam Runtime rewrites that
variable inside its container, from the XDG defaults `/usr/local/share` and `/usr/share` regardless.
User directories are scanned first, so a user-installed tool wins ties against a system copy.

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
- `mingw64-gcc-c++` for the Windows helper
- `qt6-qtbase-devel` for the GUI (provides `qmake6`)
- `clang` -- the GUI is linked with `clang++` plus `-Wl,-z,nocopyreloc` so its binary loads on
  distros whose Qt exports the meta-object data symbols with protected visibility (Arch/CachyOS).
  Without clang the GUI still builds with g++, but then it only loads on default-visibility Qt.
- `rpm-build` if you want the RPMs
- optional: a 32-bit multilib toolchain (`glibc-devel.i686` + `libstdc++-devel.i686`) for the
  32-bit layer; it is skipped with a notice when absent

Vulkan headers are vendored, so no Vulkan devel package is needed.

Build only:

```bash
./build.sh
```

A plain build also installs the layer manifest into `~/.local/share` so locally launched games pick
it up; set `DLSSNR_SKIP_MANIFEST_INSTALL=1` to skip that.

Build and package in one step:

```bash
./build.sh --tar     # + the .tar.gz tarballs (public + personal)
./build.sh --rpm     # + the RPMs (public + personal)
./build.sh --dist    # + both
```

Outputs:

```text
build/layer/libVkLayer_NV_dlssnr.so     64-bit layer
build/layer32/libVkLayer_NV_dlssnr.so   32-bit layer (when a multilib toolchain is present)
build/dlssnr_helper.exe                 Windows NGX helper
build/smoke.exe                         smoke-test program
build/runner_probe                      runner discovery probe
build/dlssnr-shmctl                     shared-memory settings CLI
build/gui/dlssnr_gui                    Qt GUI
build/binder_test                       GUI binder regression test (run it offscreen)
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

The gear menu (bottom right) also holds **NGX binaries**: import the NVIDIA DLLs into
`$XDG_DATA_HOME/dlssnr/binaries`, or open that folder to place them yourself. On first launch, if
`nvngx_dlssnr.dll` is not there, the GUI prompts to import it -- otherwise the helper can only report
"no NGX binaries" and games keep presenting their own frames.

## Synthetic Motion Vectors

The helper can generate screen-space motion vectors between `Frame[N-1]` and `Frame[N]` and bind them to `DLSSNR.MVec` before calling Feature 18.

Current behavior:

- `DLSSNR.Depth` is left as `nullptr`.
- `DLSSNR.UseAutoMask` is set to `1`.
- `DLSSNR.Reset` is set on the first frame and on simple CPU-detected scene cuts.
- NVIDIA Optical Flow (`VK_NV_optical_flow`) is used when available.
- The helper keeps optical-flow input images in VRAM and runs the flow pass before `VULKAN_EvaluateFeature(18)`.
- `DLSSNR.MVec` is filled as `R16G16_SFLOAT` half-float vectors in source-pixel units, current-frame-to-previous-frame by default.
- The flow output is decoded, deadzone-filtered and upscaled entirely on the GPU by a compute pass (one shader variant per flow format, `R16G16_SFIXED5_NV` or `R16G16_SFLOAT`). If that pass cannot be built, estimated motion vectors are disabled rather than converted on the CPU.
- Motion-vector quality controls the NVOF performance level and output grid: Fast prefers a smaller grid, Balanced uses a medium grid, and Quality prefers full-resolution flow.

Environment controls:

```text
DLSSNR_MVEC=0                 disable synthetic motion vectors
DLSSNR_MVEC_DIRECTION=0       use previous-to-current flow direction instead of current-to-previous
DLSSNR_MVEC_COMPUTE=0         disable the GPU deadzone pass (disables estimated motion vectors)
DLSSNR_MVEC_DEBUG=1           log first few flow/MVec statistics
DLSSNR_SCENE_CUT=0            disable scene-cut reset detection
DLSSNR_SCENE_CUT_THRESHOLD=55 mean luma difference threshold
```

Transport controls (read by both the layer and the helper):

```text
DLSSNR_DMABUF=0               disable the dma-buf zero-copy transport (host copies instead)
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
"$HOME/.local/share/Steam/compatibilitytools.d/Proton-CachyOS Latest/proton" run "$PWD/build/smoke.exe"
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

## HDR Input

A game presenting in HDR used to be shown to the model as an 8-bit tone-mapped copy: the highlights
the model exists to judge were compressed away before it ever saw them. With the HDR path on, the
proxy crosses as `R16G16B16A16_SFLOAT` carrying linear light normalised by the white point, and the
model -- asked to create itself as HDR -- works on the frame's real range.

Detection is automatic. A float swapchain (`R16G16B16A16_SFLOAT`) is linear light; a 10-bit swapchain
whose colour space is HDR10/PQ carries ST 2084 code, which is decoded to linear nits on the way in
and re-encoded on the way out. A 10-bit swapchain in an SDR colour space is just more precision on a
finished picture and stays on the 8-bit path.

The GUI's **HDR input** row (Composition tab, Color group) decides what happens with that:

```text
Auto              HDR when the swapchain is HDR (default)
Off               8-bit proxy whatever the game presents
Force float16     float proxy even for an SDR swapchain -- an A/B tool, not a preference
```

`dlssnr-shmctl <shm> set hdrmode 0|1|2` does the same from a shortcut.

Three things have the last word over the layer's detection, in this order: the device (it must be
able to hold a float16 storage surface), the model (its feature contract is asked for HDR, and if it
refuses the helper rebuilds everything 8-bit on its own and says so in the log), and the frame
(it is never read at the wrong width -- the frame that switches formats is presented as the game
drew it, one frame, rather than misread). The GUI status line shows what was detected and which
proxy is live.

The white point means the same thing on both paths -- the composition works in units where 1.0 is
paper white -- but on a PQ frame it acts as a multiplier on the fixed 203-nit reference rather than
dividing an unknown game scale. Measured white point works on HDR frames too: the meter measures
light, not code.

## Packaging

Everything lands in `dist/`. The tarballs are staged from `build/` (running `make-dist.sh` builds
first if the artifacts are missing), and the RPMs install the staged tarball as their payload, so an
RPM build also leaves the tar.gz behind.

```bash
./packaging/make-dist.sh          # tarballs + RPMs (same as ./build.sh --dist)
./packaging/make-dist.sh tar      # tarballs only
./packaging/make-dist.sh rpm      # RPMs only
```

Both variants are always staged: `dlssnr` (public, no NVIDIA DLLs) and `dlssnr-personal` (bundles the
DLLs from `binaries/`). The public package does not include NVIDIA DLLs. The personal package does --
only redistribute it if you have the rights to do so.

Artifacts:

```text
dist/dlssnr-<version>-<release>-linux-x86_64.tar.gz
dist/dlssnr-personal-<version>-<release>-linux-x86_64.tar.gz
dist/dlssnr-<version>-<release>.fcXX.x86_64.rpm
dist/dlssnr-personal-<version>-<release>.fcXX.x86_64.rpm
```

### Making a Release

1. Bump `Version:` in both specs and the `DLSSNR_VERSION` default in `packaging/make-dist.sh`.
2. Bump `%global pkg_release` in **both** specs together. The tarball name is read from
   `dlssnr.spec` and both specs unpack that same tarball, so the two releases must match.
   `DLSSNR_VERSION` / `DLSSNR_RELEASE` env-var overrides are available for throwaway builds.
3. Add a `%changelog` entry to both specs.
4. Build everything and package it:

   ```bash
   ./build.sh --dist
   ```

   The GUI is relinked automatically when the chosen compiler or linker flags change, so no stale
   g++-built binary can slip into a tarball.

## Uninstall

RPM:

```bash
sudo dnf remove dlssnr
```

or:

```bash
sudo dnf remove dlssnr-personal
```

Tarball installs -- the scripts ship inside the tarball, so run them from the extracted tree (the
same tree you installed from; an update's tree removes the files fine too):

```bash
./uninstall.sh --user
```

Tarball system install:

```bash
sudo ./uninstall.sh --system
```

Both modes remove the binaries, the desktop entry, the per-architecture layer manifests
(`VK_LAYER_NV_dlssnr.*.json`) and the `environment.d/dlssnr.conf` layer-order snippet, so the loader
stops looking for the layer. Add `--purge` to also remove user config, state, runtime data, and the
managed prefix. The RPM removes the snippet too, but only if it is still exactly what the package
wrote.

## Troubleshooting

If the helper starts and immediately logs `shutting down`, update to `0.2.1-3` or newer and remove stale runtime state:

```bash
rm -f "/tmp/dlssnr-$UID/shm.bin"
```

If a Steam game crashes in `steamoverlayvulkanlayer.so`, update to `0.2.1-3` or newer.

If a Steam/Proton game logs `no answer ... (helper not running)` while the helper is waiting for
frames, it is on a different mapping than the helper -- see [Steam / Proton
Containers](#steam--proton-containers).

If you run Smooth Motion alongside this layer, the two implicit layers can end up in the wrong order
and the frames are layered incorrectly. The installers pin the order via
`VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"` in `~/.config/environment.d/dlssnr.conf`
(see [Install From Tarball](#install-from-tarball)); if you installed before `0.2.5-3`, add that file
yourself and log out and back in.

## Important Notes

- The helper and game run in separate processes. Where the driver allows it, frames cross as
  dma-buf memory (`VK_EXT_external_memory_dma_buf`) and stay in VRAM end to end;
  `DLSSNR_DMABUF=0` turns that off. The helper exports both images that cross the boundary and
  names them in the shared header; the layer adopts each descriptor with `pidfd_getfd` (falling
  back to `/proc/<pid>/fd` on older kernels), which needs the same uid and a permissive yama
  setting (`ptrace_scope` 0). Any step that cannot be honoured -- no extension, a refused export,
  a blocked descriptor adoption -- falls back to the
  host-transport path, and then to plain staging copies, so the frame always crosses somehow.
- The layer currently assumes a present-time swapchain layout that works for the tested games and emulators. Some games may need layout handling work.
- Steam runtime issues may require per-game or per-runtime debugging.
- Do not use the personal package publicly unless you are certain you may redistribute the bundled NVIDIA binaries.