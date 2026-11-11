# Optional ordered GPU image transport

The GPU transport carries the proxy and neural answer in exported Vulkan images,
with an exported timeline semaphore ordering the layer and the Windows helper.
Enable `DLSSNR_GPU_IMAGES=1` in both processes. It requires matching protocol-23
layer, helper and shmctl builds; do not mix components from older protocols.
The ordinary shared-memory transport remains available with this option unset.

The model still evaluates one frame at a time and preserves temporal history.
This is a single ordered cross-process transport, not a multiple-frame helper ring.
A renderer can separately retain bounded source images and submit unrelated work
on another queue while its presentation worker waits for the model. Per-queue
host leases allow independent queues to progress; a shared device gate excludes
all queue operations during device-idle lifecycle work. Actual Vulkan Present
continues to return its real result after the operation.

## Build and activate

Build the native layer, tools and Windows helper using the repository's existing
`tools/meson-build.sh`. The native tools build additionally produces
`build/native/tools/libdlssnr_unix_fd_bridge.so`. The project needs a C compiler
for this helper-only preload, in addition to its existing C++ toolchain.

The tested path uses Proton's shared-resource handles and Wine's
`wine_server_handle_to_fd` export. The preload closes exported Unix descriptors
in the actual helper process; it must not be injected into the game. In the helper
runner's environment set:

```sh
export DLSSNR_GPU_IMAGES=1
export DLSSNR_UNIX_BRIDGE_EXE=/absolute/path/to/dlssnr_helper.exe
export LD_PRELOAD=/absolute/path/to/libdlssnr_unix_fd_bridge.so
export DLSSNR_UNIX_BRIDGE_ID="$$-$RANDOM-$RANDOM"
```

Keep the same private `DLSSNR_SHM` path for the layer and helper, and use a fresh
bridge ID for each helper launch, including restarts. The helper executable path
must match its mapped file. Scope these variables to the helper command and retain
the existing launcher's private-prefix teardown. Descriptor adoption requires
permission to use pidfd_getfd on the helper. Unsupported drivers, Wine variants
or permission policies can prevent GPU transport; no system security policy is
changed by this source. Do not interpret model counters alone as visual validation.

`DLSSNR_SCENE_CUT=1` retains the SDR detector (default threshold 55 and its original
streak rule). GPU input copies at most 64x36 exact detector sample coordinates to
host memory before evaluation. HDR retains the existing detector-off behavior.
This bounded readback has a cost; it is not removed to inflate throughput.

## Failure and resource ownership

Each helper incarnation publishes an epoch. Requests and answers must match both
the exact request number and epoch. A failed request quarantines that epoch;
the layer submits no new capture or future model wait for it. A replacement helper
clears old requests/export advertisements and starts fresh temporal history.
The layer drains old local uses before retiring imported images and timelines.
It queries the imported timeline counter before submitting a model read, so a
helper death cannot leave a queued wait for a signal that will never arrive.

Image replacement withdraws old export readiness on failure and publishes a new
sequence after successful recreation, even when numeric handles are reused.
Resize resets temporal state. Shutdown withdraws export readiness and closes
owned Unix descriptors through the preload. A stale bridge file is reclaimed only
after checking its owner, permissions, inode and confirmed process death. A live
or ambiguous PID is never displaced. This is recovery from helper failure, not
recovery from a lost Vulkan device or an indefinitely hung GPU.

The source also retains stable shared-memory reservations, complete capture/compose
access masks and queue-present completion ordering required for the tested path.
No NVIDIA model, runtime DLL, game data or generated captures are added.

## Verification

`bash tests/run_transport.sh` compiles and runs the real queue lease implementation,
compares the existing scene detector over exact CPU/GPU sample grids, exercises
extracted production generation/import/response gates and resize export failures,
and checks dead-bridge reclamation. It uses authored fixtures and no model or game.
These tests do not establish Vulkan driver behavior or pixel equivalence.

The same transport source was built and exercised in an earlier private integrated
Linux/NVIDIA runtime with unchanged neural settings: initial operation, helper
kill/restart, orderly resource teardown and actual 1280x720 to 960x540 to 1280x720
swapchain resize succeeded. Paired pre/post-effect captures confirmed active output
at each geometry and were inspected for gross corruption. A matched A/B/A with an
application-side bounded native queue and second graphics queue measured
52.37 / 62.21 / 52.63 stationary FPS, +18.50% against the bracketing mean. This is
combination-specific evidence, not a standalone layer gain, a claim about every
game/GPU or a 120-FPS result. New upstream rebases still require their own gameplay
acceptance. Windows-native, other drivers and full-game visual quality are unproven.
