// VK_LAYER_NV_dlssnr — Linux-side Vulkan layer (loaded by the host loader,
// including the one inside Wine/winevulkan). Hooks the swapchain lifecycle;
// on present, ships the frame to the Windows DLSSNR helper over shared
// memory and presents the neural-processed result.
//
// Enabled implicitly via enable_environment DLSSNR_ENABLE=1 (winevulkan
// rejects explicitly-named layers, so implicit enable is required under Wine).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "../../common/shm_protocol.h"
#include "composition.h"
#include "hotkey.h"
#include "vk_table.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>

// The layer's name has to differ per architecture.
//
// The loader keys implicit layers by name, so two manifests claiming the same name are one layer to
// it: it keeps whichever it read first and then rejects it for the process's word size, reporting
// only "Requested layer VK_LAYER_NV_dlssnr was wrong bit-type" -- with the manifest that would have
// worked sitting unread beside it. Steam hit the same wall and answered it the same way, which is why
// its overlay is VK_LAYER_VALVE_steam_overlay_32 next to _64 rather than one name twice.
#ifdef DLSSNR_LAYER_32
#define VK_LAYER_NAME "VK_LAYER_NV_dlssnr_32"
#else
#define VK_LAYER_NAME "VK_LAYER_NV_dlssnr"
#endif

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void Log(const char* fmt, ...) {
    static FILE* f = [] {
        const char* p = getenv("DLSSNR_LOG");
        return p && *p ? fopen(p, "a") : stderr;
    }();
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // One mutex for the sink, not one per line: the previous form allocated a fresh
    // std::mutex on every call and leaked it, which at present rates is a leak per frame.
    static std::mutex sinkMutex;
    std::lock_guard<std::mutex> lk(sinkMutex);
    fprintf(f, "[dlssnr-layer] %s\n", buf);
    fflush(f);
}

static bool TimeEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_TIME");
        return p && p[0] == '1';
    }();
    return v;
}

static int TimeInterval() {
    static const int v = [] {
        const char* p = getenv("DLSSNR_TIME_EVERY");
        return p && *p ? atoi(p) : 30;
    }();
    return v > 0 ? v : 30;
}

static bool VerboseEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_VERBOSE");
        return p && p[0] == '1';
    }();
    return v;
}

static inline double NowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline void CpuYield() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// ---------------------------------------------------------------------------
// Shared memory transport
// ---------------------------------------------------------------------------
struct ShmMap {
    // Three mappings rather than one.
    //
    // The file is a header followed by two pixel regions each large enough for the biggest frame the
    // protocol allows, which is a quarter of a gigabyte in total. Mapping all of it was fine on
    // 64-bit and is not on 32-bit: a 32-bit game has about 3 GB of address space and would be handing
    // a tenth of it to a reservation it never touches. Both region offsets are page-aligned by
    // construction, so each can be mapped on its own at the size actually in use -- a few megabytes
    // for a real frame instead of 265.
    int fd = -1;
    ShmHeader* hdr = nullptr;
    uint8_t* inPixels = nullptr;
    uint8_t* outPixels = nullptr;
    size_t mappedFrameBytes = 0;
    uint32_t seq = 0;
    uint32_t timeouts = 0;

    // Liveness, so a game is never made to wait on a helper that is not there.
    uint32_t firstHeartbeat = 0;
    bool everAnswered = false;
    double retryAfterMs = 0.0;
    uint32_t lastControlSeq = 0;
    uint32_t lastHeartbeat = 0;
    bool dead = false;
    // The file path, kept so the dma-buf socket can be named beside it.
    std::string path;
};

// The directory now lives under /tmp, which is world-writable, so it is worth checking that what we
// are about to open really is ours: a directory, owned by this uid, with nothing granted to anyone
// else. Anything else and we refuse rather than create the file inside it.
static bool EnsureParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;
    std::string dir = path.substr(0, slash);
    size_t pos = 1;
    while ((pos = dir.find('/', pos)) != std::string::npos) {
        mkdir(dir.substr(0, pos).c_str(), 0700);
        pos += 1;
    }
    mkdir(dir.c_str(), 0700);

    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0) { Log("[shm] %s is missing", dir.c_str()); return false; }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        Log("[shm] refusing %s: it is not a private directory owned by this user", dir.c_str());
        return false;
    }
    return true;
}

// Maps a file range at a hint address, trying successive 2 MiB-aligned slots until one is free.
// The hint is 64 KiB-aligned and MAP_FIXED_NOREPLACE either takes that exact address or fails, so
// a success here is a pointer the driver will accept for VK_EXT_external_memory_host -- NVIDIA
// demands minImportedHostPointerAlignment, which is 64 KiB. If every slot is taken the plain map
// still works; the composition checks the pointer's alignment and falls back to staging.
static void* ShmMapAligned(int fd, off_t offset, size_t want, uintptr_t hint) {
#if defined(MAP_FIXED_NOREPLACE) && UINTPTR_MAX > 0xFFFFFFFFull
    for (int i = 0; i < 128; ++i) {
        void* p = mmap((void*) (hint + size_t(i) * (2u << 20)), want, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset);
        if (p != MAP_FAILED) return p;
    }
#else
    (void) hint;
#endif
    return mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
}

// Maps the two pixel regions at the size this frame needs, remapping when the size changes.
static bool ShmMapFrames(ShmMap& s, size_t bytes) {
    if (s.mappedFrameBytes >= bytes && s.inPixels && s.outPixels) return true;
    if (bytes > kMaxFrame) return false;

    // Round up so a small change in resolution does not remap every frame, and to a 64 KiB multiple so
    // a host-pointer import of the whole region (whose allocation size must be a multiple of the
    // driver's import alignment) stays inside the mapping.
    const size_t kImportAlign = 65536;
    const size_t want = ((bytes + kImportAlign - 1) / kImportAlign) * kImportAlign;

    if (s.inPixels) munmap(s.inPixels, s.mappedFrameBytes);
    if (s.outPixels) munmap(s.outPixels, s.mappedFrameBytes);
    s.inPixels = s.outPixels = nullptr;
    s.mappedFrameBytes = 0;

    // The file offsets of both regions are already 64 KiB multiples (v7 of the protocol moved the
    // header to 64 KiB and kMaxFrame is an exact multiple); what the kernel adds is the address.
#if UINTPTR_MAX > 0xFFFFFFFFull
    const uintptr_t kHint = UINT64_C(0x200000000000);
    const uintptr_t outHint = kHint + size_t(130) * (2u << 20) + ((want + ((2u << 20) - 1)) & ~size_t((2u << 20) - 1));
#else
    const uintptr_t kHint = 0, outHint = 0;
#endif

    void* in = ShmMapAligned(s.fd, (off_t) kHeaderBytes, want, kHint);
    if (in == MAP_FAILED) { Log("[shm] could not map the input region (%zu bytes)", want); return false; }

    void* out = ShmMapAligned(s.fd, (off_t) (kHeaderBytes + kMaxFrame), want, outHint);
    if (out == MAP_FAILED) {
        munmap(in, want);
        Log("[shm] could not map the output region (%zu bytes)", want);
        return false;
    }

    s.inPixels = (uint8_t*) in;
    s.outPixels = (uint8_t*) out;
    s.mappedFrameBytes = want;
    return true;
}

static bool ShmOpen(ShmMap& s) {
    if (s.hdr) return true;
    const char* path = getenv("DLSSNR_SHM");
    std::string p = (path && *path) ? path : ShmDefaultPath();
    if (!EnsureParentDir(p)) return false;
    int fd = open(p.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) { Log("[shm] open %s failed", p.c_str()); return false; }
    // The file still spans the whole protocol -- the offsets are fixed and both sides agree on them --
    // but it is sparse, so the size on disk is what has actually been written.
    size_t total = ShmTotalBytes();
    struct stat st{};
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < total) {
        if (ftruncate(fd, (off_t)total) != 0) { close(fd); return false; }
    }

    void* m = mmap(nullptr, kHeaderBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { Log("[shm] mmap of the header failed"); close(fd); return false; }
    s.fd = fd;
    s.hdr = (ShmHeader*)m;
    // A mapping left by an older build has a different magic, a different version, or a header
    // laid out differently; re-initialising is the only safe reading of any of those.
    //
    // But say so, loudly. A live process on the other side of the mismatch keeps re-initialising the
    // other way, and the two then silently reset each other's settings forever -- the layer keeps
    // composing with its old field set and every setting the newer side writes is invisible. That is
    // indistinguishable from "the new feature does nothing", which is how a stale layer reads until
    // someone checks the log.
    if (s.hdr->magic.load() != kShmMagic || s.hdr->version.load() != kShmVersion ||
        s.hdr->passes.load() == 0) {
        if (s.hdr->magic.load() == kShmMagic && s.hdr->version.load() != kShmVersion)
            Log("[shm] header is version %u but this layer is v%u -- another process is out of date, "
                "re-initialising it; update the layer, the helper and the GUI together",
                s.hdr->version.load(), kShmVersion);
        ShmInitDefaults(s.hdr);
    }
    s.lastHeartbeat = s.hdr->heartbeat.load();
    s.firstHeartbeat = s.lastHeartbeat;
    s.path = p;
    Log("[shm] attached %s seq_req=%u seq_resp=%u", p.c_str(),
        s.hdr->seq_req.load(), s.hdr->seq_resp.load());
    return true;
}

static bool ShmNeuralEnabled(ShmMap& s) {
    if (!ShmOpen(s)) return true;
    if (s.hdr->quit.load()) { s.dead = true; return false; }
    const uint32_t ctrl = s.hdr->controlSeq.load();
    if (ctrl != s.lastControlSeq) {
        s.lastControlSeq = ctrl;
        if (s.dead && ::ShmNeuralEnabled(s.hdr)) {
            s.dead = false;
            s.timeouts = 0;
            Log("[shm] control changed, re-enabling");
        }
    }
    const uint32_t hb = s.hdr->heartbeat.load();
    if (hb != s.lastHeartbeat) {
        s.lastHeartbeat = hb;
        // A heartbeat alone is not a reason to try again immediately. The helper ticks it while it
        // sits idle, so a helper that is up but not answering used to re-enable the layer the moment
        // it had given up -- which cost the game another round of full-length waits, over and over.
        // That is the stutter: recover, stall, give up, recover.
        if (s.dead && NowMs() >= s.retryAfterMs && ::ShmNeuralEnabled(s.hdr)) {
            s.dead = false;
            s.timeouts = 0;
            Log("[shm] helper heartbeat, trying again");
        }
    }
    if (s.dead) return false;
    return ::ShmNeuralEnabled(s.hdr);
}

// One round trip: publish the proxy, wait for the model's answer, copy it back.
//
// What crosses is the proxy at the model's own resolution, always R8G8B8A8_UNORM and always
// display-referred, because the encode has already done that work on the GPU. The helper therefore
// never has to know what format the game presents in, and the working scale reduces this copy
// quadratically -- which on this transport is the difference the setting actually buys.
// Take this process's own reference on a dma-buf another process exported: procfs opens a fresh
// descriptor for the same buffer. Same uid and a permissive yama setting are what make the open
// work; where it does not, the caller falls back and nothing else changes.
// Adopt a descriptor the helper exported. The primary route is pidfd_getfd, which duplicates a
// descriptor straight out of the helper's table -- the only way to receive a dma-buf, since those
// live on an anonymous filesystem that cannot be reopened by path. The old route (opening
// /proc/<pid>/fd/<n>) is kept as a fallback for kernels predating pidfd; it works for ordinary
// files even though it cannot see dma-bufs. Both need the same uid and a permissive yama.
static int AdoptPeerFd(uint32_t pid, uint32_t fd) {
    if (!pid || fd == 0 || fd > 1000000u) return -1;
#ifdef __NR_pidfd_open
    const int pidfd = (int)syscall(__NR_pidfd_open, pid, 0);
    if (pidfd >= 0) {
        const int dup = (int)syscall(__NR_pidfd_getfd, pidfd, fd, 0);
        close(pidfd);
        if (dup >= 0) return dup;
    }
#endif
    char p[64];
    snprintf(p, sizeof(p), "/proc/%u/fd/%u", pid, fd);
    return open(p, O_RDWR | O_CLOEXEC);
}

// The exchange is on unless the environment says off; the helper and driver get the final say by
// what they publish.
static bool DmaBufEnabled() {
    static const bool on = [] {
        const char* v = getenv("DLSSNR_DMABUF");
        return !(v && !strcmp(v, "0"));
    }();
    return on;
}

static bool ShmProcessFrame(ShmMap& s, uint32_t w, uint32_t h, size_t bytes, const void* proxy,
                          void* modelOut, bool proxyInRegion, bool answerFromFd, bool hdrEncode) {
    if (s.dead) return false;
    if (!ShmOpen(s)) { s.dead = true; return false; }
    if (w > kMaxW || h > kMaxH || w < kMinW || h < kMinH) return false;
    if (bytes != size_t(w) * h * 4 && bytes != size_t(w) * h * 8) return false;
    if (s.hdr->quit.load()) { s.dead = true; return false; }

    const bool time = TimeEnabled();
    const double t0 = NowMs();
    if (!ShmMapFrames(s, bytes)) { s.dead = true; return false; }
    // When the transport buffer IS this region (the imported case), or the proxy crossed as a
    // dma-buf instead, the GPU already wrote the bytes where they belong and there is nothing to
    // copy.
    if (!proxyInRegion && proxy != (const void*) s.inPixels) std::memcpy(s.inPixels, proxy, bytes);
    const double tCopy = NowMs();
    s.hdr->width.store(w);
    s.hdr->height.store(h);
    s.hdr->format.store(1u);  // RGBA byte order either way; the float path keeps the same swizzle
    // Say what the bytes ARE before announcing them: the helper sizes its read by this, never by
    // what it hopes the layer has switched to. The release fence below covers it like the pixels.
    s.hdr->hdrEncode.store(hdrEncode ? 1u : 0u);
    uint32_t req = s.hdr->seq_req.load() + 1;
    // The release pairs with the helper's acquire on seq_resp: everything this process wrote --
    // the proxy, whether by the GPU into the imported region or by the memcpy above -- is visible
    // to the helper before it sees the new request number. (The GPU's own write is fenced earlier,
    // by leg 1's vkWaitForFences; this fence covers the host-visible ordering across processes.)
    std::atomic_thread_fence(std::memory_order_release);
    s.hdr->seq_req.store(req);

    // How long this frame may wait, which is a question about whether anyone is listening.
    //
    // A live helper needs real time: the model is milliseconds of work and building its feature on the
    // first frame is far more than that. A helper that is not running needs none at all, and the old
    // fixed second-per-frame budget meant a game whose helper was simply not started froze for eight
    // seconds before the layer gave up. That is what this is for.
    // Is anything listening? The helper says so itself, from the moment it attaches until it exits,
    // which is the only signal that stays true while it is busy. Heartbeats do not: it stops ticking
    // them precisely while it is building the model's feature.
    const bool helperPresent = s.hdr->helperState.load() != kHelperStopped;

    // The first frame of a size is not like the others. It makes the helper load the model and build
    // a feature -- measured at 194 ms for a small frame and more for a large one -- against about 4 ms
    // once it is warm. Timing that out and giving up is how a working helper gets abandoned before it
    // has answered once.
    const bool warmingUp = !s.everAnswered;
    const double budgetMs = !helperPresent ? 20.0 : (warmingUp ? 10000.0 : 1000.0);

    // Wait for the helper (fail-open: present the original frame on timeout).
    const double tSignal = NowMs();
    for (;;) {
        if (s.hdr->seq_resp.load() >= req) {
            s.timeouts = 0;
            s.everAnswered = true;
            // The helper's GPU wrote the answer into this region (or the memcpy below reads the
            // staging copy of it); the acquire pairs with the helper's release before seq_resp.
            std::atomic_thread_fence(std::memory_order_acquire);
            // The helper answers even when it could not use the frame. seq_ok says whether the
            // answer is worth composing; when it is not, the game's own frame is what to present.
            // The echo says the answer was made for this raster: another swapchain (the Steam
            // overlay, or this one's predecessor mid-resize) may have had its request answered in
            // the meantime, and seq_resp only counts. Composing that answer here would copy a
            // different number of bytes into these surfaces -- the row-shifted colour garbage this
            // check exists to refuse.
            const bool ok = s.hdr->seq_ok.load() >= req && s.hdr->answeredW.load() == w &&
                            s.hdr->answeredH.load() == h;
            if (!ok) Log("[shm] helper could not use frame %u (ok=%u)", req, s.hdr->seq_ok.load());
            if (ok && !answerFromFd && modelOut != (void*) s.outPixels) std::memcpy(modelOut, s.outPixels, bytes);
            if (time) {
                static int frameNo = 0;
                if (++frameNo % TimeInterval() == 0) {
                    const double tDone = NowMs();
                    Log("[time] shm copy=%.2f signal=%.2f wait=%.2f total=%.2f ms",
                        tCopy - t0, tSignal - tCopy, tDone - tSignal, tDone - t0);
                }
            }
            return ok;
        }
        if (s.hdr->quit.load()) { s.dead = true; return false; }
        const double elapsed = NowMs() - tSignal;
        if (elapsed >= budgetMs) break;
        if (elapsed < 2.0) CpuYield();
        else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // Four rather than eight, and with a pause before the next attempt, so giving up costs a
    // fraction of a second and retrying costs that again only every few seconds.
    if (++s.timeouts >= 4) {
        s.dead = true;
        s.retryAfterMs = NowMs() + 5000.0;
        Log("[shm] no answer in %.0f ms x4 (helper %s); passing frames through, retrying in 5s "
            "(seq_req=%u seq_resp=%u heartbeat=%u)",
            budgetMs, helperPresent ? "is present but silent" : "not running",
            s.hdr->seq_req.load(), s.hdr->seq_resp.load(), s.hdr->heartbeat.load());
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dispatch chains
// ---------------------------------------------------------------------------
struct InstanceChain {
    PFN_vkGetInstanceProcAddr next_gipa = nullptr;

    // The instance-level entry points the composition needs, resolved once. Kept here rather than on
    // the device chain because this is where the VkInstance handle is in scope.
    dlssnr::InstanceTable table;

    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
};

#define DEVICE_FN_LIST(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkGetDeviceQueue2) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkDeviceWaitIdle)

struct SwapchainState {
    std::vector<VkImage> images;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // HdrKind: what this swapchain's format and colour space say the frame carries. The float
    // swapchain holds linear light; a 10-bit one with a PQ colour space holds ST 2084 code.
    uint32_t hdrKind = kHdrNone;
    uint32_t width = 0, height = 0;
    // Two fences, not one. Leg 1's must be waited on before the proxy is handed to the helper --
    // the sequence number is the helper's only ordering signal, and it may not be bumped ahead of
    // the write it announces. Leg 2's needs no wait at all in its own frame: the present follows it
    // on the same queue, so the GPU orders them without the CPU. The wait moves to the start of the
    // next present, where the command buffer and the composed surfaces are reused, which takes a
    // full GPU stall out of the frame it belongs to.
    VkFence fenceLeg1 = VK_NULL_HANDLE;
    VkFence fenceLeg2 = VK_NULL_HANDLE;
    bool leg2Pending = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool ready = false;
    bool passThrough = false;

    // The pass. Owns every surface it needs, including the transport pair -- the shared-memory
    // regions themselves when the driver will import them, host-visible staging when it will not.
    std::unique_ptr<dlssnr::Composition> comp;
};

struct DeviceChain {
    InstanceChain* instance = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice self = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr next_dpa = nullptr;

    // The same entry points again, in the form the composition takes them.
    dlssnr::DeviceTable table;

    // The loader's hook for installing a dispatch table on a dispatchable object a layer creates.
    // Handed to every layer in its own VkLayerDeviceCreateInfo node; see Hook_CreateDevice.
    PFN_vkSetDeviceLoaderData setDeviceLoaderData = nullptr;
#define X(name) PFN_##name name = nullptr;
    DEVICE_FN_LIST(X)
#undef X
    std::atomic<bool> inert{false};
    std::mutex lock;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
    std::unordered_map<VkQueue, uint32_t> queueFamilies;
    ShmMap shm;
    uint64_t framesComposed = 0;
    uint64_t framesPassedThrough = 0;
    // Phase 5: the dma-buf exchange. The export sequences last imported; a new sequence means the
    // image behind the descriptor changed and the reference is taken again.
    uint32_t proxySeqSeen = 0;
    uint32_t answerSeqSeen = 0;
};

static std::unordered_map<VkInstance, InstanceChain> g_instances;
static std::unordered_map<VkPhysicalDevice, InstanceChain*> g_phys;
static std::unordered_map<VkDevice, DeviceChain*> g_devices;
static std::mutex g_stateMutex;

// The one swapchain allowed to drive the neural round trip, chosen as the largest in the process.
//
// The shared-memory channel carries a single raster at a time, but a process can present more than
// one swapchain -- the game window and the Steam overlay, or, mid-resize, the old and new windows at
// once. Feeding all of them through one channel makes the helper rebuild its model on every size
// switch and lets one swapchain be handed another's answer. The largest is the game; the rest present
// raw. The record is global rather than per-device because the overlay builds its own VkDevice.
struct PrimarySwap {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t area = 0;
};
static PrimarySwap g_primary;
// Its own mutex, never nested with dc->lock or g_stateMutex, so the lock order in the present hook
// cannot invert against the device hooks.
static std::mutex g_primaryMutex;

// Adopts a larger swapchain; a present from anything else passes through untouched.
static bool ClaimPrimary(VkDevice device, VkSwapchainKHR swapchain, uint32_t w, uint32_t h) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    const uint64_t area = uint64_t(w) * h;
    if (g_primary.swapchain == swapchain && g_primary.device == device) return true;
    if (g_primary.swapchain != VK_NULL_HANDLE && area <= g_primary.area) return false;
    g_primary.device = device;
    g_primary.swapchain = swapchain;
    g_primary.area = area;
    return true;
}

static void ReleasePrimary(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    if (g_primary.swapchain == swapchain && g_primary.device == device) g_primary = PrimarySwap{};
}

// Where this copy of the layer was loaded from, for the duplicate check below.
static std::string LayerObjectPath() {
    Dl_info info{};
    if (dladdr((const void*)&LayerObjectPath, &info) && info.dli_fname && *info.dli_fname)
        return info.dli_fname;
    return std::string();
}

// True when a *different* copy of this layer is already in the chain.
//
// Local packaging installs an implicit-layer manifest pointing at the build tree while install.sh installs
// another pointing at the install prefix, and the loader honours both: two copies of the layer, two
// present hooks, two full round trips, and a single shared-memory file with two writers racing on
// one sequence number. Only the first copy stays live; the rest declare themselves inert and pass
// everything through, which turns a corrupted picture or a hang into one warning line.
//
// The claim is the object's own path rather than a bare flag, so a second call into the same copy --
// which is legal, the loader may negotiate more than once -- is told apart from a second copy.
static bool DuplicateLayerCopy() {
    static const bool dup = [] {
        const std::string self = LayerObjectPath();
        const char* claimed = getenv("DLSSNR_LAYER_OBJECT");
        if (claimed && *claimed) {
            if (self.empty() || self == claimed) return false;
            Log("[layer] another copy is already loaded from %s; this copy (%s) stays inert. "
                "Remove one of the implicit-layer manifests.", claimed, self.c_str());
            return true;
        }
        if (!self.empty()) setenv("DLSSNR_LAYER_OBJECT", self.c_str(), 0);
        return false;
    }();
    return dup;
}

// One set of keyboards for the process, however many devices the game creates.
static dlssnr::Hotkeys g_hotkeys;

// The key to watch, from the header if the interface has set one and from the environment otherwise,
// so it can be bound in a launch option without the interface being involved.
static uint32_t ToggleKey(const ShmHeader* hdr) {
    static const uint32_t fromEnv = [] {
        const char* v = getenv("DLSSNR_TOGGLE_KEY");
        return v && *v ? dlssnr::KeyCodeFromName(v) : 0u;
    }();
    if (fromEnv) return fromEnv;
    return hdr ? hdr->toggleKey.load() : 0u;
}

// Polled before anything asks whether the pass is enabled, because asking first would make turning it
// off a one-way door: the early return would skip the very code that reads the key to turn it back on.
static void PollHotkeys(DeviceChain* dc) {
    if (!ShmOpen(dc->shm) || !dc->shm.hdr) return;
    const uint32_t key = ToggleKey(dc->shm.hdr);
    if (!key || !g_hotkeys.Pressed(key)) return;

    const bool wasOn = dc->shm.hdr->enabled.load() != 0;
    dc->shm.hdr->enabled.store(wasOn ? 0u : 1u);
    dc->shm.hdr->controlSeq.fetch_add(1);
    Log("[hotkey] %s -> neural rendering %s", dlssnr::KeyNameFromCode(key), wasOn ? "off" : "on");
}

static bool LayerEnabled() {
    static const bool e = [] {
        if (DuplicateLayerCopy()) return false;
        const char* v = getenv("VKLayer_DLSS5");
        if (v && v[0] == '1') return true;
        const char* o = getenv("DLSSNR_ENABLE");
        return o && o[0] == '1';
    }();
    return e;
}

// ---------------------------------------------------------------------------
// Instance hooks
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>((const VkLayerInstanceCreateInfo*)pCreateInfo->pNext);
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo*)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // Documented pattern: keep the link node in pNext (layers below need it)
    // and advance u.pLayerInfo so the next layer resolves its own chain entry.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) return res;

    InstanceChain chain{};
    chain.next_gipa = next_gipa;
    chain.vkDestroyInstance = (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
    chain.vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)next_gipa(*pInstance, "vkEnumeratePhysicalDevices");
    chain.vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)next_gipa(*pInstance, "vkGetPhysicalDeviceProperties");
    chain.vkEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(*pInstance, "vkEnumerateDeviceExtensionProperties");
    chain.vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(*pInstance, "vkGetPhysicalDeviceMemoryProperties");

    chain.table.next_gipa = next_gipa;
    chain.table.Load(*pInstance);

    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_instances[*pInstance] = chain;
    Log("[layer] vkCreateInstance -> %p", (void*)*pInstance);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Hook_DestroyInstance(VkInstance instance,
                                                       const VkAllocationCallbacks* pAllocator) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    auto it = g_instances.find(instance);
    if (it == g_instances.end()) return;
    auto destroy = it->second.vkDestroyInstance;
    InstanceChain* chain = &it->second;
    g_instances.erase(it);
    for (auto pit = g_phys.begin(); pit != g_phys.end();)
        pit = (pit->second == chain) ? g_phys.erase(pit) : std::next(pit);
    if (destroy) destroy(instance, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_EnumeratePhysicalDevices(
    VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pPhysicalDevices) {
    InstanceChain* chain = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) chain = &it->second;
    }
    if (!chain || !chain->vkEnumeratePhysicalDevices) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = chain->vkEnumeratePhysicalDevices(instance, pCount, pPhysicalDevices);
    if (res == VK_SUCCESS && pPhysicalDevices) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        for (uint32_t i = 0; i < *pCount; ++i) g_phys[pPhysicalDevices[i]] = chain;
    }
    return res;
}

// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
static VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto* link = const_cast<VkLayerDeviceCreateInfo*>((const VkLayerDeviceCreateInfo*)pCreateInfo->pNext);
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo*)link->pNext;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_dpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create = (PFN_vkCreateDevice)next_gipa(VK_NULL_HANDLE, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;

    // A second node in the same chain carries vkSetDeviceLoaderData. Every dispatchable object this
    // layer allocates has to be passed through it; see SetLoaderData below for why.
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (const auto* n = (const VkLayerDeviceCreateInfo*)pCreateInfo->pNext; n;
         n = (const VkLayerDeviceCreateInfo*)n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            n->function == VK_LOADER_DATA_CALLBACK) {
            setLoaderData = n->u.pfnSetDeviceLoaderData;
            break;
        }
    }

    InstanceChain* ic = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_phys.find(physicalDevice);
        if (it != g_phys.end()) ic = it->second;
    }

    // VK_EXT_external_memory_host is what lets the transport buffers BE the shared-memory regions,
    // so the proxy and the model's answer never pass through a private staging copy. The two fd
    // extensions do the same job across the process boundary: VK_KHR_external_memory_fd is what
    // vkGetMemoryFdKHR and the fd imports need, and VK_EXT_external_memory_dma_buf names the handle
    // type the images are shared as. They are device extensions and the application decides what
    // the device enables, but a layer may add to that list on the way down -- and does, when the
    // pass is on, the device offers them, and the app did not already enable them. If any of that
    // is false the composition falls back to the next transport down.
    static const char* const kWantExts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                                             VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                             VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME };
    constexpr size_t kWantCount = sizeof(kWantExts) / sizeof(kWantExts[0]);
    const VkDeviceCreateInfo* effective = pCreateInfo;
    VkDeviceCreateInfo modified = *pCreateInfo;
    std::vector<const char*> enabledExts;
    if (LayerEnabled() && ic && ic->vkEnumerateDeviceExtensionProperties) {
        bool have[kWantCount] = {};
        uint32_t n = 0;
        ic->vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n && ic->vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, avail.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < n; ++i)
                for (size_t k = 0; k < kWantCount; ++k)
                    if (!std::strcmp(avail[i].extensionName, kWantExts[k])) have[k] = true;
        }
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            for (size_t k = 0; k < kWantCount; ++k)
                if (!std::strcmp(pCreateInfo->ppEnabledExtensionNames[i], kWantExts[k])) have[k] = false;
        for (size_t k = 0; k < kWantCount; ++k) {
            if (!have[k]) continue;
            if (enabledExts.empty()) {
                enabledExts.reserve(pCreateInfo->enabledExtensionCount + kWantCount);
                for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
                    enabledExts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
            }
            enabledExts.push_back(kWantExts[k]);
        }
        if (!enabledExts.empty()) {
            modified.enabledExtensionCount = uint32_t(enabledExts.size());
            modified.ppEnabledExtensionNames = enabledExts.data();
            effective = &modified;
        }
    }

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = create(physicalDevice, effective, pAllocator, pDevice);
    if (res != VK_SUCCESS) return res;

    DeviceChain* dc = new DeviceChain();
    dc->instance = ic;
    dc->physical = physicalDevice;
    dc->self = *pDevice;
    dc->next_dpa = next_dpa;
    dc->setDeviceLoaderData = setLoaderData;
#define X(name) dc->name = (PFN_##name)next_dpa(*pDevice, #name);
    DEVICE_FN_LIST(X)
#undef X
    dc->table.next_dpa = next_dpa;
    dc->table.Load(*pDevice);
    if (!dc->vkQueuePresentKHR || !dc->vkCreateSwapchainKHR || !ic) dc->inert = true;

    // Neural Rendering is an NGX feature and the helper only ever creates its own device on an
    // NVIDIA GPU, so on anything else there is nothing for this layer to do but cost a round trip.
    // Hybrid machines are the case that matters: an implicit layer is loaded for every device the
    // loader builds, including the integrated one a game may well be running on.
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "?";
    if (ic && ic->vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties props{};
        ic->vkGetPhysicalDeviceProperties(physicalDevice, &props);
        std::snprintf(deviceName, sizeof(deviceName), "%s", props.deviceName);
        if (props.vendorID != 0x10DE) {
            dc->inert = true;
            Log("[layer] inert on non-NVIDIA device (vendor %#x): %s", props.vendorID, deviceName);
        }
    }

    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_devices[*pDevice] = dc;
    Log("[layer] vkCreateDevice -> %p on %s (inert=%d enabled=%d)", (void*)*pDevice, deviceName,
        (int) dc->inert.load(), (int) LayerEnabled());
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Hook_DestroyDevice(VkDevice device,
                                                     const VkAllocationCallbacks* pAllocator) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) { dc = it->second; g_devices.erase(it); }
    }
    if (!dc) return;
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) ReleasePrimary(device, kv.first);
    }
    if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) {
            SwapchainState& sc = kv.second;
            sc.comp.reset();
            if (sc.fenceLeg1) dc->vkDestroyFence(device, sc.fenceLeg1, nullptr);
            if (sc.fenceLeg2) dc->vkDestroyFence(device, sc.fenceLeg2, nullptr);
            if (sc.pool) dc->vkDestroyCommandPool(device, sc.pool, nullptr);
        }
        dc->swapchains.clear();
    }
    if (dc->vkDestroyDevice) dc->vkDestroyDevice(device, pAllocator);
    delete dc;
}

static DeviceChain* FindDevice(VkDevice device) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

static void RememberQueue(DeviceChain* dc, VkQueue queue, uint32_t family) {
    if (!queue) return;
    std::lock_guard<std::mutex> lk(dc->lock);
    dc->queueFamilies[queue] = family;
}

static VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue(VkDevice device, uint32_t family,
                                                      uint32_t index, VkQueue* pQueue) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkGetDeviceQueue) return;
    dc->vkGetDeviceQueue(device, family, index, pQueue);
    RememberQueue(dc, *pQueue, family);
}

// The 1.1 way of asking for a queue, and the only way to reach one created with
// VkDeviceQueueCreateFlags. A game that uses it never registered its queue through the hook above,
// so the present path could not tell which family the queue belonged to and fell back to family
// zero -- which is the family the command pool was then created on, and need not be the queue's.
static VKAPI_ATTR void VKAPI_CALL Hook_GetDeviceQueue2(VkDevice device,
                                                       const VkDeviceQueueInfo2* pQueueInfo,
                                                       VkQueue* pQueue) {
    DeviceChain* dc = FindDevice(device);
    if (!dc || !dc->vkGetDeviceQueue2) return;
    dc->vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueueInfo) RememberQueue(dc, *pQueue, pQueueInfo->queueFamilyIndex);
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
// Which swapchain formats the pass can work in. Every one of them has a UNORM twin the composition
// uses internally; the ten-bit and float entries are new here, and are what lets an HDR game reach
// the model at all -- the encode is exactly the step that turns open-ended light into the kind of
// picture the model was trained on.
static bool SupportedFormat(VkFormat f) {
    return dlssnr::CompositionFormat(f) != VK_FORMAT_UNDEFINED;
}

// What a swapchain's format and colour space together say about the light in the frame.
//
// A float swapchain is the easy case: games hand over linear light and the HDR path divides it by
// the white point and hands the model the result. The ten-bit formats are the ones worth the colour
// space: on a desktop set to HDR10 they carry ST 2084 code -- absolute nits, which is why the old
// display-referred reading of them (tone map as if it were SDR) banding-crushed them to eight bits
// on the way to the model. A ten-bit swapchain in an SDR colour space is just a bit more precision
// on a tone-mapped frame, and stays on the SDR path.
static uint32_t DetectHdrKind(VkFormat f, VkColorSpaceKHR cs) {
    if (f == VK_FORMAT_R16G16B16A16_SFLOAT) return kHdrLinearFp16;
    const bool tenBit = f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
                        f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                        f == VkFormat(1000452000) /* R12G12B12A16_UNORM_PACK32 */;
    const bool pq = cs == VK_COLOR_SPACE_HDR10_ST2084_EXT ||
                    cs == VkColorSpaceKHR(1000459000) /* HDR10_ST2084_COMPATIBLE */;
    if (tenBit && pq) return kHdrPq10;
    // A float swapchain in a linear BT.2020 space is still linear light; nothing else here is HDR.
    return kHdrNone;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) dc = it->second;
    }
    if (!dc || !dc->vkCreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

    VkSwapchainCreateInfoKHR m = *pCreateInfo;
    if (!dc->inert && LayerEnabled() && SupportedFormat(pCreateInfo->imageFormat))
        m.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = dc->vkCreateSwapchainKHR(device, &m, pAllocator, pSwapchain);
    if (res != VK_SUCCESS || dc->inert || !LayerEnabled()) return res;

    uint32_t count = 0;
    dc->vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    dc->vkGetSwapchainImagesKHR(device, *pSwapchain, &count, images.data());

    SwapchainState sc{};
    sc.images = std::move(images);
    sc.format = pCreateInfo->imageFormat;
    sc.hdrKind = DetectHdrKind(sc.format, pCreateInfo->imageColorSpace);
    sc.width = pCreateInfo->imageExtent.width;
    sc.height = pCreateInfo->imageExtent.height;
    const bool tooSmall = sc.width < kMinW || sc.height < kMinH;
    sc.passThrough = !SupportedFormat(sc.format) || sc.width > kMaxW || sc.height > kMaxH || tooSmall;

    std::lock_guard<std::mutex> lk(dc->lock);
    Log("[layer] swapchain %p %ux%u fmt=%d hdr=%u passThrough=%d%s", (void*)*pSwapchain,
        pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
        (int)pCreateInfo->imageFormat, sc.hdrKind, (int)sc.passThrough,
        sc.passThrough ? (!SupportedFormat(sc.format) ? " (unsupported format)"
                          : tooSmall ? " (too small)" : " (too large)") : "");
    dc->swapchains[*pSwapchain] = std::move(sc);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL Hook_DestroySwapchainKHR(VkDevice device,
    VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) dc = it->second;
    }
    if (!dc) return;
    ReleasePrimary(device, swapchain);
    std::unique_lock<std::mutex> lk(dc->lock);
    auto it = dc->swapchains.find(swapchain);
    if (it != dc->swapchains.end()) {
        lk.unlock();
        if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
        lk.lock();
        SwapchainState& sc = it->second;
        sc.comp.reset();
        if (sc.fenceLeg1) dc->vkDestroyFence(device, sc.fenceLeg1, nullptr);
        if (sc.fenceLeg2) dc->vkDestroyFence(device, sc.fenceLeg2, nullptr);
        if (sc.pool) dc->vkDestroyCommandPool(device, sc.pool, nullptr);
        dc->swapchains.erase(it);
    }
    lk.unlock();
    if (dc->vkDestroySwapchainKHR) dc->vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

// ---------------------------------------------------------------------------
// Present-time neural round-trip
// ---------------------------------------------------------------------------
// Give a dispatchable object this layer allocated the dispatch table the loader expects on it.
//
// VkCommandBuffer and VkQueue are dispatchable: their first word points at a dispatch table, and
// every layer below reads it to find its own state for that object. The loader fills that word in
// for objects the application allocates through the trampoline -- but a layer that allocates one by
// calling straight down the chain bypasses the trampoline, so the loader never sees it and the word
// keeps whatever the ICD left there. The loader hands each layer vkSetDeviceLoaderData precisely so
// it can do that fill-in itself, and calling it is mandatory, not advisory.
//
// Skipping it is invisible with no other layer present: the next call goes straight to the driver,
// which does not read the word. Add any second layer -- Steam's overlay, MangoHud, validation -- and
// that layer reads the word, finds the ICD's loader magic instead of a table, and aborts. Validation
// says so out loud: 'The VkDevice dispatch handle was not found and Validation will crash.'
static bool SetLoaderData(DeviceChain* dc, void* object) {
    if (!dc->setDeviceLoaderData) return true;  // no loader in the chain; nothing to fill in
    return dc->setDeviceLoaderData(dc->self, object) == VK_SUCCESS;
}

static bool CreateResources(DeviceChain* dc, SwapchainState& sc, uint32_t family) {
    VkDevice d = dc->self;

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    if (dc->vkCreateCommandPool(d, &cpci, nullptr, &sc.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = sc.pool; cbai.commandBufferCount = 1;
    if (dc->vkAllocateCommandBuffers(d, &cbai, &sc.cb) != VK_SUCCESS) return false;
    if (!SetLoaderData(dc, sc.cb)) {
        Log("[layer] vkSetDeviceLoaderData failed for the present command buffer");
        return false;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (dc->vkCreateFence(d, &fci, nullptr, &sc.fenceLeg1) != VK_SUCCESS) return false;
    if (dc->vkCreateFence(d, &fci, nullptr, &sc.fenceLeg2) != VK_SUCCESS) return false;

    if (!dc->instance) return false;
    sc.comp = std::make_unique<dlssnr::Composition>(&dc->table, &dc->instance->table, d, dc->physical);
    if (!sc.comp->Usable()) {
        Log("[layer] composition unavailable: %s", sc.comp->Reason());
        sc.comp.reset();
        return false;
    }
    return true;
}

// Every Vulkan result on the present path, looked at rather than collapsed into a bool.
//
// A failure here was previously indistinguishable from 'nothing to do': the call returned false, the
// caller presented the original frame, and the next frame tried exactly the same thing again. That is
// the right answer for a transient failure and the wrong one for VK_ERROR_DEVICE_LOST, where the
// device is gone, every subsequent call will fail the same way, and the log fills with it.
//
// Losing the device also latches the layer inert, because after that point the fail-open path is the
// only correct one and it costs nothing to take it directly.
static bool NoteVk(DeviceChain* dc, VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    if (r == VK_ERROR_DEVICE_LOST) {
        if (!dc->inert.exchange(true)) Log("[layer] %s -> DEVICE_LOST; layer inert for this device", what);
        return false;
    }
    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1) < 8) Log("[layer] %s -> %d", what, (int) r);
    return false;
}

// Returns true if the swapchain image now holds the composed frame.
//
// Three steps around one round trip. The pass encodes a proxy of the frame on the GPU, that proxy
// crosses to the helper and comes back as the model's answer, and the pass composes the answer onto
// the frame. The proxy's crossing is fenced on the CPU because the model is in another process and
// there is nothing to wait on but a sequence number; the answer's return is not -- leg 2 and the
// present are ordered by the queue itself, and the fence that covers leg 2 is only waited on at the
// start of the NEXT frame, where the command buffer and the composed surfaces are reused.
//
// The caller's present semaphores are consumed by the first submit, because that submit is the first
// thing to touch the image. They are therefore unsignalled by the time this returns and must not be
// handed to vkQueuePresentKHR again; the caller presents with none.
//
// Every path out leaves the swapchain image in PRESENT_SRC_KHR, including the ones that give up.
static bool ProcessPresent(DeviceChain* dc, SwapchainState& sc, VkQueue queue,
                           VkImage swapchainImage, uint32_t waitCount,
                           const VkSemaphore* waitSemaphores) {
    if (!sc.comp) return false;
    VkDevice d = dc->self;
    VkCommandBuffer cb = sc.cb;
    const bool time = TimeEnabled();
    const double t0 = time ? NowMs() : 0.0;

    // The previous frame's compose, if it is still running, must finish before anything here
    // touches the surfaces it reads or the command buffer it was recorded into. Waiting here rather
    // than at the end of that frame keeps the game thread out of the GPU's way for the whole of the
    // helper's round trip. The capture pair that compose recorded lands with it.
    if (sc.leg2Pending) {
        if (!NoteVk(dc, dc->vkWaitForFences(d, 1, &sc.fenceLeg2, VK_TRUE, UINT64_MAX),
                    "vkWaitForFences(leg2)"))
            return false;
        dc->vkResetFences(d, 1, &sc.fenceLeg2);
        sc.leg2Pending = false;
        sc.comp->WriteCapturedFrame();
    }

    const dlssnr::FrameSettings fs = dlssnr::FrameSettings::Read(dc->shm.hdr);

    // The HDR decision, made once per frame before anything is sized or encoded.
    //
    // hdrActive is what this process intends; proxyFormat is what the helper actually built, and the
    // float encode is only taken when both agree -- a model that refused the float input leaves the
    // frame on the 8-bit path it has always used. hdrActive doubles as the echo the helper reads, so
    // it builds the float images only for a layer that has said it will feed them.
    const uint32_t hdrMode = dc->shm.hdr ? dc->shm.hdr->hdrMode.load() : kHdrAuto;
    const bool hdrActive = hdrMode != kHdrOff && (hdrMode == kHdrForce || sc.hdrKind != kHdrNone);
    const bool hdrProxy = hdrActive && dc->shm.hdr &&
                          dc->shm.hdr->proxyFormat.load() == kProxyRgba16F;
    const uint32_t hdrTransfer = sc.hdrKind == kHdrPq10 ? 1u : 0u;

    const bool linearHdr =
        hdrProxy || dlssnr::ColourIsLinearHdr(sc.format, dc->shm.hdr ? dc->shm.hdr->colourMode.load() : kColourAuto);

    // Point the transport at the shared-memory regions before Prepare sizes anything, so the first
    // frame at a new raster imports the mapping instead of building staging that then has to be
    // thrown away. The regions are mapped at the model's own size, which is what the GPU copies
    // into and out of.
    if (dc->shm.hdr && ShmOpen(dc->shm)) {
        uint32_t mw = 0, mh = 0;
        dlssnr::Composition::ModelExtent(sc.width, sc.height, fs, mw, mh);
        if (ShmMapFrames(dc->shm, size_t(mw) * mh * (hdrProxy ? 8 : 4)))
            sc.comp->SetTransport(dc->shm.inPixels, dc->shm.outPixels, dc->shm.mappedFrameBytes);
        else
            sc.comp->SetTransport(nullptr, nullptr, 0);
    } else {
        sc.comp->SetTransport(nullptr, nullptr, 0);
    }

        // ---- Phase 5: the dma-buf exchange ----
    //
    // The helper owns both images that cross the boundary and names their exported dma-bufs in
    // the header; this process takes its own reference on each through pidfd_getfd (or /proc on older kernels).
    // Everything here is best-effort and restated every frame: a descriptor that cannot be opened
    // or imported leaves that direction on the shared-memory transport, which is the arrangement
    // that shipped before. The flags written below say which surfaces this frame's bytes travel
    // through, and the helper honours them on exactly the frame they were set for.
    sc.comp->SetDmaBuf(DmaBufEnabled());

    if (!sc.comp->Prepare(sc.width, sc.height, sc.format, fs, linearHdr, hdrProxy, hdrTransfer)) {
        Log("[layer] composition cannot run here: %s", sc.comp->Reason());
        return false;
    }

    if (dc->shm.hdr) {
        dc->shm.hdr->hdrDetected.store(sc.hdrKind);
        // The intent, not the format-gated decision: the helper builds the float images only for a
        // layer that has said it will feed them, and that handshake has to start while the proxy is
        // still 8-bit. Publishing HdrProxyActive() here would wait on proxyFormat, which waits on
        // this field, and neither would ever move.
        dc->shm.hdr->hdrActive.store(hdrActive ? 1u : 0u);
    }

    if (sc.comp->DmaBuf() && dc->shm.hdr) {
        ShmHeader* hdr = dc->shm.hdr;
        const uint32_t ps = hdr->proxyExportSeq.load();
        if (ps && ps != dc->proxySeqSeen) {
            const int fd = AdoptPeerFd(hdr->proxyPid.load(), hdr->proxyFd.load());
            if (fd >= 0 && sc.comp->ImportProxy(fd, sc.comp->ModelWidth(), sc.comp->ModelHeight()))
                dc->proxySeqSeen = ps;
        } else if (!ps) {
            dc->proxySeqSeen = 0;
        }
        const uint32_t as = hdr->answerExportSeq.load();
        if (as && as != dc->answerSeqSeen) {
            const int fd = AdoptPeerFd(hdr->answerPid.load(), hdr->answerFd.load());
            if (fd >= 0 && sc.comp->ImportAnswerFd(fd, sc.comp->ModelWidth(), sc.comp->ModelHeight()))
                dc->answerSeqSeen = as;
        } else if (!as) {
            dc->answerSeqSeen = 0;
        }
    }

    // One decision, made before the request goes out: the echo the helper reads and the surfaces
    // this frame writes and composes from are the same decision, not two that must agree. The
    // echo names the export sequence this process holds a reference at, so the helper reads the
    // fd path only for the very image the layer imported -- not a stale one behind a restart.
    const bool answerViaFd = sc.comp->AnswerViaFd();
    sc.comp->SetAnswerViaFd(answerViaFd);
    if (dc->shm.hdr) {
        dc->shm.hdr->layerProxySeq.store(sc.comp->ProxyActive() ? dc->proxySeqSeen : 0u);
        dc->shm.hdr->layerAnswerSeq.store(answerViaFd ? dc->answerSeqSeen : 0u);
    }

    // A capture is asked for by writing a frame count into the header; taking it clears the request,
    // so one press produces one run rather than one per frame for as long as nobody clears it.
    if (dc->shm.hdr) {
        if (const uint32_t frames = dc->shm.hdr->captureRequest.exchange(0); frames > 0)
            sc.comp->RequestCapture(std::min<uint32_t>(frames, 64));
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    // Only the first submit waits: the later one is ordered behind it on the same queue and fenced
    // besides, and a binary semaphore may be waited on once per signal.
    std::vector<VkPipelineStageFlags> waitStages(waitCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitCount ? waitSemaphores : nullptr;
    si.pWaitDstStageMask = waitCount ? waitStages.data() : nullptr;
    const auto dropWaits = [&] {
        si.waitSemaphoreCount = 0;
        si.pWaitSemaphores = nullptr;
        si.pWaitDstStageMask = nullptr;
    };

    const auto endAndSubmit = [&](VkFence fence) {
        if (!NoteVk(dc, dc->vkEndCommandBuffer(cb), "vkEndCommandBuffer")) return false;
        if (!NoteVk(dc, dc->vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) return false;
        return true;
    };
    const auto waitAndReset = [&](VkFence fence) {
        if (!NoteVk(dc, dc->vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
            return false;
        dc->vkResetFences(d, 1, &fence);
        return true;
    };

    // ---- leg 1: the frame the model is shown ----
    if (!NoteVk(dc, dc->vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer")) return false;
    if (!sc.comp->RecordCapture(cb, swapchainImage, fs)) {
        dc->vkEndCommandBuffer(cb);
        return false;
    }
    // This one fence is real: the proxy the helper is about to read is written by these commands,
    // and the sequence number must not outrun the pixels it announces.
    if (!endAndSubmit(sc.fenceLeg1)) return false;
    if (!waitAndReset(sc.fenceLeg1)) return false;
    dropWaits();
    sc.comp->ConsumeMeter();
    const double tCapture = time ? NowMs() : 0.0;

    // ---- the round trip ----
    if (!ShmProcessFrame(dc->shm, sc.comp->ModelWidth(), sc.comp->ModelHeight(), sc.comp->ModelBytes(),
                         sc.comp->ProxyPixels(), sc.comp->ModelPixels(), sc.comp->ProxyActive(),
                         answerViaFd, sc.comp->HdrProxyActive())) {
        // Fail-open. Leg 1 already put the image back in PRESENT_SRC_KHR, so the original frame is
        // what gets presented and nothing else is owed.
        return false;
    }
    sc.comp->MarkModelFrame();
    const double tHelper = time ? NowMs() : 0.0;

    // ---- leg 2: the answer, composed back ----
    if (!NoteVk(dc, dc->vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer")) return false;
    if (!sc.comp->RecordCompose(cb, swapchainImage, fs)) {
        dc->vkEndCommandBuffer(cb);
        return false;
    }
    // No wait. The present that follows runs on this same queue behind these commands, so the image
    // is composed before it is shown without the CPU ever parking here; the fence is collected at
    // the top of the next frame, where the reused surfaces actually need it.
    if (!endAndSubmit(sc.fenceLeg2)) return false;
    sc.leg2Pending = true;
    const double tReturn = time ? NowMs() : 0.0;

    if (dc->shm.hdr) {
        ShmStore64(dc->shm.hdr->layerFramesLo, dc->shm.hdr->layerFramesHi, ++dc->framesComposed);
        dc->shm.hdr->layerWidth.store(sc.width);
        dc->shm.hdr->layerHeight.store(sc.height);
        dc->shm.hdr->layerFormat.store(uint32_t(sc.format));
        dc->shm.hdr->layerCompositionUp.store(1);
        dc->shm.hdr->layerMsBits.store(FloatToBits(float(tReturn - t0)));
        dc->shm.hdr->layerMeasuredWhiteBits.store(FloatToBits(sc.comp->MeasuredWhitePoint()));
        dc->shm.hdr->layerHeartbeat.fetch_add(1);
    }

    if (time) {
        static int frameNo = 0;
        if (++frameNo % TimeInterval() == 0) {
            Log("[time] encode=%.2f helper=%.2f resolve=%.2f total=%.2f ms (model %ux%u)",
                tCapture - t0, tHelper - tCapture, tReturn - tHelper, tReturn - t0,
                sc.comp->ModelWidth(), sc.comp->ModelHeight());
        }
    }
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_QueuePresentKHR(VkQueue queue,
                                                           const VkPresentInfoKHR* pPresentInfo) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        if (g_devices.size() == 1) dc = g_devices.begin()->second;
        else {
            for (auto& kv : g_devices) {
                std::lock_guard<std::mutex> dl(kv.second->lock);
                if (kv.second->queueFamilies.count(queue)) { dc = kv.second; break; }
            }
        }
    }
    if (!dc || !dc->vkQueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;

    // Whether this call's wait semaphores have already been consumed by a submit of ours. They are
    // handed to the first swapchain we actually process; every path after that presents with none,
    // because a semaphore signalled once may only be waited on once. Presenting with them a second
    // time is a wait that never completes -- which is what a second layer in the chain, Steam's
    // overlay among them, turns from a latent bug into a hang.
    bool waitsConsumed = false;

    if (!dc->inert && LayerEnabled()) {
        std::lock_guard<std::mutex> lk(dc->lock);
        PollHotkeys(dc);
        if (!ShmNeuralEnabled(dc->shm)) return dc->vkQueuePresentKHR(queue, pPresentInfo);
        uint32_t family = 0;
        auto qit = dc->queueFamilies.find(queue);
        if (qit != dc->queueFamilies.end()) family = qit->second;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto sit = dc->swapchains.find(pPresentInfo->pSwapchains[i]);
            if (sit == dc->swapchains.end()) continue;
            SwapchainState& sc = sit->second;
            if (sc.passThrough || pPresentInfo->pImageIndices[i] >= sc.images.size()) continue;
            // One swapchain drives the channel; the rest present raw. See ClaimPrimary.
            if (!ClaimPrimary(dc->self, pPresentInfo->pSwapchains[i], sc.width, sc.height)) continue;
            if (!sc.ready && !dc->shm.dead) {
                if (!CreateResources(dc, sc, family)) {
                    Log("[layer] staging resources failed for swapchain %p (%ux%u, family %u); "
                        "passing this swapchain through",
                        (void*)pPresentInfo->pSwapchains[i], sc.width, sc.height, family);
                    sc.passThrough = true;
                    // This swapchain claimed the primary role and just gave it up. Without the
                    // release the claim would sit on a swapchain that never drives the channel,
                    // and no peer of equal or smaller area could take it over.
                    ReleasePrimary(dc->self, pPresentInfo->pSwapchains[i]);
                    continue;
                }
                sc.ready = true;
            }
            if (!sc.ready || dc->shm.dead) {
                ReleasePrimary(dc->self, pPresentInfo->pSwapchains[i]);
                continue;
            }
            const uint32_t waitCount = waitsConsumed ? 0u : pPresentInfo->waitSemaphoreCount;
            waitsConsumed = true;
            const bool composed = ProcessPresent(dc, sc, queue, sc.images[pPresentInfo->pImageIndices[i]],
                                                 waitCount, pPresentInfo->pWaitSemaphores);
            if (!composed) ++dc->framesPassedThrough;
            if (VerboseEnabled()) {
                Log("[present] swapchain=%p image=%u seq=%u composed=%d",
                    (void*)pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i],
                    dc->shm.hdr ? dc->shm.hdr->seq_req.load() : 0u, int(composed));
            }
            // On failure we simply present the original frame (fail-open). The semaphores are still
            // consumed -- the capture submit waits on them before anything can fail -- so the flag
            // stays set and the present below still drops them.
        }
        if (TimeEnabled()) {
            static int frameNo = 0;
            if (++frameNo % TimeInterval() == 0) {
                Log("[layer] composed=%llu passed through=%llu",
                    (unsigned long long)dc->framesComposed,
                    (unsigned long long)dc->framesPassedThrough);
            }
        }
    }

    if (!waitsConsumed) return dc->vkQueuePresentKHR(queue, pPresentInfo);

    // pNext is carried through untouched: present ids, present timing and the rest belong to the
    // caller and none of them are about semaphores.
    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 0;
    pi.pWaitSemaphores = nullptr;
    return dc->vkQueuePresentKHR(queue, &pi);
}

// ---------------------------------------------------------------------------
// Loader entry points
// ---------------------------------------------------------------------------
static PFN_vkVoidFunction LookupHook(const char* n) {
    if (!std::strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)Hook_CreateInstance;
    if (!std::strcmp(n, "vkDestroyInstance")) return (PFN_vkVoidFunction)Hook_DestroyInstance;
    if (!std::strcmp(n, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)Hook_EnumeratePhysicalDevices;
    if (!std::strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction)Hook_CreateDevice;
    if (!std::strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction)Hook_DestroyDevice;
    if (!std::strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue;
    if (!std::strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue2;
    if (!std::strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)Hook_CreateSwapchainKHR;
    if (!std::strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)Hook_DestroySwapchainKHR;
    if (!std::strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)Hook_QueuePresentKHR;
    return nullptr;
}

static PFN_vkVoidFunction LookupDeviceHook(const char* n) {
    if (!std::strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction)Hook_DestroyDevice;
    if (!std::strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue;
    if (!std::strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue2;
    if (!std::strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)Hook_CreateSwapchainKHR;
    if (!std::strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)Hook_DestroySwapchainKHR;
    if (!std::strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)Hook_QueuePresentKHR;
    return nullptr;
}

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 7) v->loaderLayerInterfaceVersion = 7;
    if (v->loaderLayerInterfaceVersion >= 2) {
        v->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        v->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
        v->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    // Once per process, not once per negotiate.
    //
    // The loader re-enumerates the implicit layer directory many times during a single instance
    // creation -- 628 times for one 32-bit vkCreateInstance here, and the same for every other
    // manifest in the directory -- and loads this library on each pass. That is the loader's
    // business, but announcing it each time turned one line into 627 in the user's log. Every other
    // layer stays quiet because none of them log from here.
    static std::once_flag announced;
    std::call_once(announced, [] {
        const char* v = getenv("VKLayer_DLSS5");
        Log("=== %s loaded (VKLayer_DLSS5=%s) ===", VK_LAYER_NAME, v ? v : "(unset)");
    });
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pCount,
                                                                  VkLayerProperties* pProperties) {
    if (!pCount) return VK_SUCCESS;
    if (!pProperties) { *pCount = 1; return VK_SUCCESS; }
    if (*pCount < 1) { *pCount = 1; return VK_INCOMPLETE; }
    std::memset(pProperties, 0, sizeof(*pProperties));
    std::strncpy(pProperties->layerName, VK_LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
    std::strncpy(pProperties->description, "DLSS 5 Neural Rendering injection layer",
                 VK_MAX_DESCRIPTION_SIZE - 1);
    pProperties->specVersion = VK_MAKE_VERSION(1, 3, 0);
    pProperties->implementationVersion = 1;
    *pCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char*, uint32_t* pCount,
                                                                      VkExtensionProperties*) {
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (!std::strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!std::strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!std::strcmp(pName, "vkNegotiateLoaderLayerInterfaceVersion"))
        return (PFN_vkVoidFunction)vkNegotiateLoaderLayerInterfaceVersion;
    if (!std::strcmp(pName, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    if (!std::strcmp(pName, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (auto fn = LookupHook(pName)) return fn;
    if (instance) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_instances.find(instance);
        if (it != g_instances.end() && it->second.next_gipa) return it->second.next_gipa(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (!std::strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (auto fn = LookupDeviceHook(pName)) return fn;
    if (device) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end() && it->second->next_dpa) return it->second->next_dpa(device, pName);
    }
    return nullptr;
}

}  // extern "C"
