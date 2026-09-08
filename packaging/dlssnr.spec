%global debug_package %{nil}
%global _enable_debug_packages 0
%global _include_debuginfo_sources 0
%global pkg_release 5

Name:           dlssnr
Version:        0.2.5
Release:        %{pkg_release}%{?dist}
Summary:        DLSS5 Neural Rendering Vulkan layer and helper
License:        MIT
Source0:        %{name}-%{version}-%{pkg_release}-linux-x86_64.tar.gz

BuildArch:      x86_64

Requires:       bash
Requires:       vulkan-loader
Requires:       qt6-qtbase
Requires:       pciutils
Recommends:     wine

%description
DLSS5VKLayer installs a Vulkan implicit layer that forwards presented frames
to a Windows NGX helper running under Wine or a custom Proton compatibility
tool. This public package does not include NVIDIA proprietary NGX DLLs.

%prep
%setup -q -n %{name}-%{version}-%{pkg_release}-linux-x86_64

%build
# Prebuilt binary payload.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a root/usr %{buildroot}/usr

%files
%defattr(-,root,root,-)
%{_libdir}/dlssnr
%{_bindir}/dlssnr-helper
%{_bindir}/dlssnr-gui
%{_bindir}/dlssnr-runner-probe
%{_bindir}/dlssnr-shmctl
%{_datadir}/vulkan/implicit_layer.d/VK_LAYER_NV_dlssnr.*.json
%{_datadir}/applications/dlssnr.desktop
%doc %{_datadir}/doc/dlssnr/dxvk-license.txt

%post
/sbin/ldconfig || :
# Pin the Vulkan layer order so this layer runs before Smooth Motion's VK_LAYER_NV_present.
# Written per-user into ~/.config/environment.d (the package owns no files there); an existing
# file is left untouched. systemd user sessions pick it up on the next login.
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

%preun
if [ "$1" -eq 0 ]; then
    getent passwd | awk -F: '$3 >= 1000 && $3 < 65534 {print $6}' | while IFS= read -r home; do
        conf="$home/.config/environment.d/dlssnr.conf"
        if [ -f "$conf" ] && [ "$(cat "$conf" 2>/dev/null)" = 'VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present"' ]; then
            rm -f "$conf"
        fi
    done
fi

%postun
/sbin/ldconfig || :

%changelog
* Tue Sep 08 2026 DLSS5VKLayer - 0.2.5-5
- Runner discovery: system-wide compatibility tools are now found. On top of the per-user dirs, every
  directory in $XDG_DATA_DIRS and the XDG defaults /usr/local/share and /usr/share are scanned -- the
  defaults always, not merely as a fallback, because Steam Runtime rewrites XDG_DATA_DIRS inside its
  container. This is where CachyOS ships Proton-CachyOS (/usr/share/steam/compatibilitytools.d), and
  the XDG_DATA_DIRS scan covers other distro layouts (NixOS, custom prefixes) without further edits.
  User dirs are scanned first and the sort is stable, so a user-installed tool wins a name/score tie
  against a system copy. runner_probe and the dlssnr-helper shell fallback were updated in lockstep.
- Helper: detect_steam_root now honours $XDG_DATA_HOME and also checks ~/.steam/root.

* Tue Sep 08 2026 DLSS5VKLayer - 0.2.5-4
- Helper: the nvapi64.dll load is now exception-guarded and skipped when the runner already supplies
  NVAPI (the launcher sets DLSSNR_SKIP_NVAPI for Proton, where DXVK-NVAPI answers NVAPI). Forcing the
  vendored nvapi64.dll on top of DXVK-NVAPI faulted inside its DllMain with no guard around it, which
  took the whole helper down before nvngx.dll was ever reached; a bad nvapi64 now degrades to "no
  NVAPI" instead. The nvngx.dll core load is guarded for the same reason.
- GUI: import the NVIDIA NGX DLLs from the interface instead of the CLI. A "NGX binaries" submenu in
  the gear menu imports (copy nvngx_dlssnr.dll/nvngx.dll/nvapi64.dll/sl.*.dll/*.license.txt into the
  data dir) and opens the target folder; the GUI prompts on startup when nvngx_dlssnr.dll is missing.
- GUI: long tooltips are now multi-line rather than one unbroken line.

* Mon Sep 07 2026 DLSS5VKLayer - 0.2.5-3
- Install ~/.config/environment.d/dlssnr.conf (per user) pinning
  VK_INSTANCE_LAYERS="VK_LAYER_NV_dlssnr:VK_LAYER_NV_present", so the layer orders correctly alongside
  Smooth Motion instead of being layered after its VK_LAYER_NV_present. An existing file is never
  overwritten; uninstall removes the file only if it is still exactly what we wrote. Applies on the
  next login.
* Mon Sep 07 2026 DLSS5VKLayer - 0.2.5-2
- GUI: the binary now loads on distros whose Qt6 exports the meta-object data symbols with protected
  visibility (CachyOS/Arch). GCC bakes copy relocations against QSpinBox::staticMetaObject and friends
  even into a PIE, and glibc 2.41+ refuses to copy-relocate a protected symbol, so the GUI died at exec
  with GNU_PROPERTY_1_NEEDED_INDIRECT_EXTERN_ACCESS. The GUI is now built with clang++ plus
  -Wl,-z,nocopyreloc -Wl,-z,indirect-extern-access, which reaches every Qt data symbol through the GOT
  -- legal under either visibility, and unchanged on the distros that always worked.

* Mon Sep 07 2026 DLSS5VKLayer - 0.2.5-1
- HDR input. On an HDR swapchain -- float16 linear, or 10-bit with a PQ colour space -- the proxy
  crosses as float16 carrying linear light normalised by the white point, PQ-decoded on the way in
  and re-encoded on the way out, and the model's feature contract is created as HDR. The GUI's
  "HDR input" row (and shmctl hdrmode) choose Auto / Off / Force; Auto decides from the swapchain's
  format and colour space. The device and the model have veto power: a device that cannot hold a
  float16 surface or a model that refuses the float contract falls back to the 8-bit proxy by
  itself, and no frame is ever read at the wrong width.
- Protocol bumped to v10: the pixel regions carry eight bytes a pixel, and the header names the HDR
  decision, the detection, the live proxy format and the width of the bytes in the request.

* Mon Sep 07 2026 DLSS5VKLayer - 0.2.4-1
- Transport: frames can now cross as dma-buf memory (VK_EXT_external_memory_dma_buf). The
  helper exports the proxy and the answer as dma-bufs and names them in the shared header; the
  layer adopts each with pidfd_getfd and writes/samples them as its own memory -- no host
  round trip when the driver allows it (same uid, yama ptrace_scope 0). DLSSNR_DMABUF=0 disables
  it; every failed step falls back to the host transport.
- Protocol bumped to v9: the answer echoes its raster size, and the header carries the
  dma-buf export descriptors and the importer's sequence echo.
- Fix: blue/red pixel garbage when a second swapchain (Steam overlay, window resize) raced
  the shared channel -- one swapchain now drives the helper, the rest present raw.
- Fix: host-transport imports failed at nearly every resolution because the allocation size
  was not rounded to the driver's import alignment; the zero-copy host path now engages.
- Fix: the helper never loaded vkGetMemoryHostPointerPropertiesEXT, silently disabling its
  side of the host-import path.
- GUI: composition moved to its own tab, with the colour controls beside it.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.3-1
- Transport: the shared-memory pixel regions are imported as Vulkan buffers via
  VK_EXT_external_memory_host, so the proxy upload and the answer readback are GPU copies
  into and out of the mapping itself. Staging copies remain as the fallback when the driver
  refuses the import or the view is misaligned. Protocol bumped to v7 (regions at 64 KiB).
- Layer: the compose leg no longer stalls the game thread. Its fence is collected at the
  start of the next present, where the reused surfaces actually need it.
- Layer: the white-point meter runs on the GPU (reduce compute pass + mirrored result);
  the measured value is patched into the pass constants device-side.
- Helper: the optical-flow deadzone/upscale pass runs entirely on the GPU for both flow
  formats (two compile-time shader variants); the CPU and hybrid conversion paths are gone.
- Cross-process sequence handshakes now carry explicit release/acquire fences.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.2-4
- Layer: side-by-side and wipe compare now work with composition off. The raw-answer path returned ahead of the compare overlay, so compare did nothing while composition was off; the raw answer is now presented through the replace-mode exact inverse, which carries the overlay.
- GUI: the presenting indicator no longer opens claiming Active. The first poll seeds the frame counter rather than judging it against zero.
- GUI: "Rebuild spacing (ms)" moves from the Cost group to the gear menu as an inline spinbox; the menu opens attached under the gear.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.2-3
- Multipass: changing one pass's model settings rebuilds only that pass. It used to tear down the whole chain and rebuild every pass, one per settle window, which is what made a single slider feel like it crawled through each pass.
- Multipass: a retuned pass keeps answering with its old tuning until its replacement is ready, so changing settings no longer drops the chain mid-rebuild.
- Rebuild pacing is now wall-clock milliseconds instead of frames (framerate no longer decides it) and is a setting: "Rebuild spacing (ms)" in the Cost group, default 250, 0 rebuilds immediately and chains the rest back to back. Raise it if the model ever stops answering after changing settings.
- Shared-memory protocol v6. The helper logs how long each feature build takes.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.2-2
- GUI: settings move to tabs -- Rendering (neural rendering, cost, composition and model), Motion, Colour, Inspect. "How much of it lands" is renamed "Composition".
- Add the composition switch (shared-memory protocol v5): off by default, the model's raw answer is presented as the frame with no blend, guard or enlargement; the composition controls hide while it is off.
- Status header shows an Active/Inactive presenting indicator instead of the composed-frames debug line; project and SHM paths are no longer printed.
- Start/Stop enable and disable with the helper's actual liveness (launcher PID file, same check the CLI uses).
- Gear menu (bottom right): reset all settings, open the helper log. Settings now persist to config.ini and restore on launch; window size is remembered.
- Layer: fix a stale clamp that silently killed the "Native + edit" enlargement mode.
- Layer, helper and GUI now report a shared-memory version mismatch loudly (log line + status banner) instead of silently re-initialising each other's header -- a stale layer made every new setting look dead.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.2-1
- Declare `library_arch` in each implicit-layer manifest so the loader skips the wrong word size by reading it instead of dlopening, ending the "wrong ELF class" log spam in every process.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.1-4
- Offload NVOF MVec post-processing to the GPU: compute pass decodes SFIXED5 flow, applies a deadzone and upscales the grid into `DLSSNR.MVec` with no host readback.
- Add `DLSSNR_MVEC_DEADZONE` (default 0.5 px) and `DLSSNR_MVEC_COMPUTE=0` kill switch; static geometry now yields exactly zero motion vectors.
- Overlap prep/flow/post command buffers across graphics and optical-flow queues via fence ring + semaphores; fix fence-ring slot reuse that caused device loss.
- Read NVOF output in-place through a `R16G16_UINT` storage view; image-to-image copies out of SFIXED5 flow output fault the driver.
- Set `TOP_OF_PIPE` semaphore wait stages (ALL_COMMANDS is invalid on the optical-flow-only queue).
- Add HDR/SDR tonemap hint, pre-exposure parameters and DoSharpening/AutoExposure feature flags at create; route Sharpness per evaluate.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.1-3
- Fix runtime synthetic motion-vector re-enable so OFF -> ON resets NVOF history and DLSSNR temporal state immediately.
- Clear stale MVec buffers and re-prime optical-flow history on motion-vector reactivation.
- Force `DLSSNR.Reset` on the reactivation frame and log reset/reactivation transitions.

* Sun Sep 06 2026 DLSS5VKLayer - 0.2.1-2
- Fix present synchronization and layout so processed frames are not presented as raw alternating frames.
- Add shared-memory `seq_ok` and layer processed/raw counters to verify helper success before presenting.
- Fix NVOF current-to-previous session binding and history copy behavior.
- Force NVOF grid to prefer 4x4 and disable cost/hint inputs to avoid Quality-mode 1x1 collapse.
- Add GPU timestamp logging for the optical-flow window and reset/scene-cut diagnostics.
- Bind a zero-filled depth resource and log `DLSSNR.Reset` slot 11 readback.

* Sun Sep 06 2026 DLSS5VKLayer - 0.1.2-1
- Fix synthetic motion vectors to encode `DLSSNR.MVec` as `R16G16_SFLOAT` half-floats.
- Default motion-vector scale to source pixels and flow direction to current-to-previous.
- Add hybrid low-res CPU conversion plus GPU float upscale when direct GPU flow conversion is invalid.
- Make Fast/Balanced/Quality motion-vector modes use different NVOF grids and performance levels.

* Sat Sep 05 2026 DLSS5VKLayer - 0.1.1-1
- Add GUI settings for synthetic motion vectors and DLSS5 presets.
- Add global and per-pass DLSS5 Native/Natural/Cinematic preset controls.
- Add motion-vector enable, scale mode, and quality controls.
- Document Steam pressure-vessel shared-memory visibility fix.

* Sat Sep 05 2026 DLSS5VKLayer - 0.1.0-4
- Detect bundled binaries and runner defaults in doctor/start when config is empty.
- Log stale helper quit flags and explicit helper quit reasons.

* Sat Sep 05 2026 DLSS5VKLayer - 0.1.0-3
- Fix Steam overlay conflict by preserving the loader device create-info chain.
- Clear stale helper quit state on helper startup.
- Sync stale shared-memory sequence counters when the helper attaches.

* Sat Sep 05 2026 DLSS5VKLayer - 0.1.0-2
- Use XDG runtime shared-memory path by default.
- Add helper heartbeat recovery.
- Build helper as a GUI-subsystem executable to avoid Proton console windows.

* Sat Sep 05 2026 DLSS5VKLayer - 0.1.0-1
- Initial packaged build.