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

// ---------------------------------------------------------------------------
// Kill switches
// ---------------------------------------------------------------------------
// Four things can be wrong when a session hangs or corrupts: our own GPU work in this process, the
// transport, the helper's evaluate, and the fact that we are hooked into the chain at all. The
// picture cannot separate them -- gPassthrough and gApplyModel are shader-level, and both still
// record the whole pass, cross the boundary and evaluate the model, so they bisect the half we can
// already reason about by looking at the frame.
//
// These two bisect the other half, from the outside in. Each is the smallest early-out that exists
// on the path it cuts, deliberately: a diagnostic with logic of its own is a second thing that can
// be wrong. None of them is a compatibility mode -- each says so on entry and each leaves the
// composition off, so a session that "works" with one set has not worked, it has been skipped.
static bool DryRun() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_DRY_RUN");
        return p && p[0] == '1';
    }();
    return v;
}

static bool NoRoundTrip() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_NO_ROUNDTRIP");
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
                          void* modelOut, bool proxyInRegion, bool answerFromFd, bool hdrEncode,
                          bool frameRepeat) {
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
    // And whether these are the same pixels as last time. Same placement and the same
    // reason: it describes the bytes, so it is published by the statement announcing them.
    s.hdr->frameRepeat.store(frameRepeat ? 1u : 0u);
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
    bool surfaceMaintenance1 = false;

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
    X(vkGetSwapchainImagesKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkQueueSubmit2) X(vkQueueWaitIdle) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkDeviceWaitIdle) \
    X(vkAcquireNextImageKHR) X(vkReleaseSwapchainImagesEXT)

struct SwapchainState {
    std::vector<VkImage> images;
    // The queue this swapchain was last presented on, and a fence for the repaint's own acquire.
    // A repaint has to submit somewhere, and the only queue known to be right for a swapchain is
    // the one the application itself uses.
    VkQueue queue = VK_NULL_HANDLE;
    VkFence repaintFence = VK_NULL_HANDLE;
    // The repaint gave up on this swapchain. Its own switch, not passThrough: passThrough turns the
    // layer off for the swapchain entirely, so using it here stopped the composition for good over a
    // convenience that had merely run out of road.
    bool repaintOff = false;
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
    // Reused by the present path; resizing only occurs when an unusual caller supplies more waits.
    std::vector<VkPipelineStageFlags> waitStages;
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

    // Composing a still frame again when the settings move under it. See the thread in
    // Hook_CreateDevice.
    std::thread repaintThread;
    std::atomic<bool> repaintRun{true};
    // Raised by the swapchain lifecycle, which cannot decline to take the lock the way a present
    // can. Creating or destroying a swapchain -- which is what going fullscreen does -- has to wait
    // for whoever holds it, and the repaint can be holding it across a transport rebuild, and that
    // rebuild waits for the device to go idle. The same cycle the present path had, on a path that
    // has no fail-open answer. So the repaint stands aside the moment this is up.
    std::atomic<bool> lifecycleWaiting{false};
    std::atomic<double> lastPresentMs{0.0};
    uint32_t repaintSeenCtrl = 0;
    uint32_t repaintSeenTuning = 0;
    // A window to keep asking in, after a change the helper has to rebuild for.
    //
    // A model setting is latched when the feature is created, so the helper debounces and rebuilds;
    // a repaint that runs in the meantime composes with the model still loaded and answers with the
    // old look. The change is real and the redraw is real, which is what made this hard to see: the
    // frame counter moves and the picture does not.
    //
    // One follow-up is not enough either, because the helper rebuilds one pass at a time and spaces
    // them -- "pass 2 retuned; rebuilding it (spacing 250 ms)", then the next -- so a four-pass
    // rebuild runs well past a second. So this asks again every so often until the rebuild has had
    // time to finish, and the last of those carries the finished model.
    double repaintUntilMs = 0.0;
    double repaintNextMs = 0.0;
    bool repaintWasEnabled = true;   // so switching the effect off is itself a change to answer
    bool releaseImages = false;      // VK_EXT_swapchain_maintenance1 was enabled
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

// Whether this swapchain is the one driving the channel, asked without claiming it.
static bool IsPrimary(VkDevice device, VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lk(g_primaryMutex);
    return g_primary.swapchain == swapchain && g_primary.device == device;
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
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        if (!std::strcmp(pCreateInfo->ppEnabledExtensionNames[i],
                         VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
            chain.surfaceMaintenance1 = true;
            break;
        }
    }
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
// Both declared here for the idle repaint, which draws a frame the application did not ask for.
static bool ProcessPresent(DeviceChain* dc, SwapchainState& sc, VkQueue queue,
                           VkImage swapchainImage, uint32_t waitCount, const VkSemaphore* pWaits,
                           bool repaint, bool* waitsConsumed);

// Put the captured frame back, for when the effect is switched off while the picture is still.
//
// No model and no round trip: the composed result is replaced by the frame it was made from, which
// is what the application drew and what it would draw again if it were running. Without this,
// switching the effect off while paused leaves the edit on screen until something else redraws.
static bool RestorePresent(DeviceChain* dc, SwapchainState& sc, VkImage swapchainImage) {
    if (!sc.comp || !sc.comp->HasCapturedFrame()) return false;
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dc->vkBeginCommandBuffer(sc.cb, &bi) != VK_SUCCESS) return false;
    if (!sc.comp->RecordRestore(sc.cb, swapchainImage)) {
        dc->vkEndCommandBuffer(sc.cb);
        return false;
    }
    if (dc->vkEndCommandBuffer(sc.cb) != VK_SUCCESS) return false;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc.cb;
    if (dc->vkQueueSubmit(sc.queue, 1, &si, sc.fenceLeg2) != VK_SUCCESS) return false;
    // Waited for here rather than left to the next frame: there may not be a next frame, and the
    // command buffer is reused by the repaint after this one.
    dc->vkWaitForFences(dc->self, 1, &sc.fenceLeg2, VK_TRUE, 500ull * 1000ull * 1000ull);
    dc->vkResetFences(dc->self, 1, &sc.fenceLeg2);
    sc.leg2Pending = false;
    {
        static std::once_flag said;
        std::call_once(said, [] { Log("[layer] idle repaint: put the captured frame back"); });
    }
    return true;
}
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
                                              VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                              // The repaint's, for handing an image back unpresented.
                                              VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME };
    constexpr size_t kWantCount = sizeof(kWantExts) / sizeof(kWantExts[0]);
    constexpr size_t kExternalMemoryFd = 1;
    const VkDeviceCreateInfo* effective = pCreateInfo;
    bool addedSwapMaint = false;
    VkDeviceCreateInfo modified = *pCreateInfo;
    std::vector<const char*> enabledExts;
    if (LayerEnabled() && ic && ic->vkEnumerateDeviceExtensionProperties) {
        bool available[kWantCount] = {};
        bool enabled[kWantCount] = {};
        uint32_t n = 0;
        ic->vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> avail(n);
        if (n && ic->vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, avail.data()) == VK_SUCCESS) {
            for (uint32_t i = 0; i < n; ++i)
                for (size_t k = 0; k < kWantCount; ++k)
                    if (!std::strcmp(avail[i].extensionName, kWantExts[k])) available[k] = true;
        }
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            for (size_t k = 0; k < kWantCount; ++k)
                    if (!std::strcmp(pCreateInfo->ppEnabledExtensionNames[i], kWantExts[k])) enabled[k] = true;
        for (size_t k = 0; k < kWantCount; ++k) {
            if (!ic->surfaceMaintenance1 &&
                !std::strcmp(kWantExts[k], VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
                continue;
            if (!available[k] || enabled[k]) continue;
            // VK_EXT_external_memory_dma_buf requires this device extension.
            if (!std::strcmp(kWantExts[k], VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                !available[kExternalMemoryFd] && !enabled[kExternalMemoryFd])
                continue;
            if (enabledExts.empty()) {
                enabledExts.reserve(pCreateInfo->enabledExtensionCount + kWantCount);
                for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
                    enabledExts.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
            }
            enabledExts.push_back(kWantExts[k]);
        }
        for (const char* e : enabledExts)
            if (!std::strcmp(e, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) addedSwapMaint = true;
        if (!enabledExts.empty()) {
            modified.enabledExtensionCount = uint32_t(enabledExts.size());
            modified.ppEnabledExtensionNames = enabledExts.data();
            effective = &modified;
        }
    }
    // The extension does nothing unless its feature is asked for.
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapMaint{};
    if (addedSwapMaint) {
        swapMaint.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        swapMaint.swapchainMaintenance1 = VK_TRUE;
        swapMaint.pNext = const_cast<void*>(modified.pNext);
        modified.pNext = &swapMaint;
        effective = &modified;
    }

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto* const nextLayerInfo = link->u.pLayerInfo;
    VkResult res = create(physicalDevice, effective, pAllocator, pDevice);
    if (res != VK_SUCCESS && effective != pCreateInfo) {
        // Nothing added here is worth failing a device creation over.
        Log("[layer] vkCreateDevice refused the layer's additions (%d); retrying with the game's list",
            (int) res);
        link->u.pLayerInfo = nextLayerInfo;
        addedSwapMaint = false;
        res = create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    }
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
    dc->releaseImages = addedSwapMaint && dc->vkReleaseSwapchainImagesEXT != nullptr;
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

    // Compose a still frame again when the settings move under it.
    //
    // A paused player presents nothing, so until now a settings change had no frame to land on and
    // the picture kept whatever look it had when playback stopped. The same is true of a window that
    // is occluded or alt-tabbed away from: anything that stops presenting stops being edited.
    //
    // What makes this safe to do at all is that it never reads the screen back. Composing again
    // means re-running what sits downstream of the captured frame -- the encode, the model and the
    // resolve -- over the frame already held, which is exactly what holdFrame means and why
    // RecordCapture says a setting changed while held "is answered on the same picture". The first
    // version of this handed the acquired image to the ordinary path, which begins by reading the
    // swapchain; on a repaint that holds the previous composed output, so the edits stacked.
    //
    // Every condition is settled before an image is taken. An acquired image is released by
    // presenting it and by nothing else, so taking one and then discovering there is nothing to draw
    // costs the application an image -- which is what left mpv with a blank window when the layer
    // was enabled and no helper was running.
    //
    // DLSSNR_IDLE_REPAINT=0 turns it off.
    static const bool repaintWanted = [] {
        const char* v = getenv("DLSSNR_IDLE_REPAINT");
        return !(v && v[0] == '0');
    }();
    // Not started under DRY_RUN. This thread composes on its own schedule, off the application's
    // thread entirely, so a switch that only cuts the present path would leave it running -- and
    // the one place the layer draws without being asked is the last place a bisection should find
    // a surprise.
    if (repaintWanted && !dc->inert && !DryRun()) {
        dc->repaintThread = std::thread([dc] {
            while (dc->repaintRun.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                if (!dc->repaintRun.load(std::memory_order_relaxed)) break;
                if (dc->inert.load() || !dc->shm.hdr || dc->shm.dead) continue;

                // Idle: long enough that a player between frames is not mistaken for one that has
                // stopped, short enough that a still picture answers a slider while the hand is
                // still on it.
                if (NowMs() - dc->lastPresentMs.load(std::memory_order_relaxed) < 250.0) continue;

                // Tried, never waited for. The application holds this lock across its own present,
                // and under FIFO that call blocks until the display is ready -- a thread parked
                // there could not be joined when the device is destroyed. A held lock means the
                // application is presenting, which is when a repaint has no business running.
                if (dc->lifecycleWaiting.load(std::memory_order_relaxed)) continue;
                std::unique_lock<std::mutex> lk(dc->lock, std::try_to_lock);
                if (!lk.owns_lock()) continue;
                // Checked again with the lock in hand: a swapchain being created or destroyed cannot
                // fail open the way a present can, so it must never find this thread in the middle of
                // a compose.
                if (dc->lifecycleWaiting.load(std::memory_order_relaxed)) continue;
                if (!dc->repaintRun.load(std::memory_order_relaxed)) break;

                // Switching the effect off is itself something to answer: the composed result is
                // what is sitting in the swapchain, and nothing will overwrite it until the
                // application draws again.
                const bool on = ShmNeuralEnabled(dc->shm);
                const uint32_t ctrl = dc->shm.hdr->controlSeq.load();
                const uint32_t tune = dc->shm.hdr->tuningSeq.load();
                const bool changed = ctrl != dc->repaintSeenCtrl || tune != dc->repaintSeenTuning ||
                                     on != dc->repaintWasEnabled;

                // A change to something the model latches at creation arms a second look. The helper
                // waits for the setting to stop moving and then rebuilds, so the answer worth
                // showing arrives after the rebuild, not when the slider was touched. Re-armed on
                // every tuning change, so dragging a slider asks once at the end rather than at each
                // tick.
                const double now2 = NowMs();
                if (changed && tune != dc->repaintSeenTuning) {
                    // Long enough for every pass to be rebuilt in turn, plus room for the last
                    // create itself, which the log measures at around a tenth of a second.
                    const double perPass = double(dc->shm.hdr->rebuildSettleMs.load()) + 250.0;
                    const uint32_t passes = dc->shm.hdr->passes.load();
                    dc->repaintUntilMs = now2 + perPass * double(passes ? passes : 1u) + 800.0;
                }
                const bool inWindow = now2 < dc->repaintUntilMs && now2 >= dc->repaintNextMs;
                if (!changed && !inWindow) continue;
                // Spaced, so a rebuild window costs a handful of round trips rather than one every
                // sixty milliseconds. Nothing here is on the application's thread, but each repaint
                // is a full trip through the model and there is no point running them back to back.
                dc->repaintNextMs = now2 + 400.0;

                SwapchainState* sc = nullptr;
                VkSwapchainKHR swapchain = VK_NULL_HANDLE;
                for (auto& kv : dc->swapchains) {
                    if (!IsPrimary(dc->self, kv.first)) continue;
                    sc = &kv.second;
                    swapchain = kv.first;
                    break;
                }
                if (!sc || !sc->ready || !sc->comp || !sc->queue || sc->passThrough ||
                    sc->repaintOff) continue;
                // Nothing captured means nothing to compose again, and an image taken now would be
                // an image taken for nothing.
                if (!sc->comp->HasCapturedFrame()) continue;
                // And the round trip is only worth starting if the helper is actually answering.
                if (on && (dc->shm.dead || !dc->shm.hdr->modelUp.load())) continue;

                // Checked once more with the lock held: the application may have presented between
                // the test above and here.
                if (NowMs() - dc->lastPresentMs.load(std::memory_order_relaxed) < 250.0) continue;

                // A fence, waited on here, rather than a semaphore: nothing is left signalled for a
                // later acquire to trip over. A real timeout, because an image is released when the
                // presentation engine finishes displaying one and with the application stopped there
                // is no present coming to prompt that -- asking at an instant of this thread's
                // choosing and giving up returns nothing, every time. Bounded, because the
                // application may resume while this waits.
                if (sc->repaintFence == VK_NULL_HANDLE) {
                    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                    if (dc->vkCreateFence(dc->self, &fci, nullptr, &sc->repaintFence) != VK_SUCCESS)
                        continue;
                }
                uint32_t index = 0;
                // Long enough for a window nobody is looking at, short enough not to be felt.
                //
                // An image is released when the presentation engine finishes displaying one, and a
                // window that is occluded or behind another is composited rarely -- so fifty
                // milliseconds asks at an instant of this thread's choosing and gives up, which is
                // the case this feature exists for. Waiting is safe now that the application's
                // present no longer waits for this lock but passes the frame through instead; the
                // cost is no longer a stall but the compositions missed while it waits, so an
                // application resuming mid-acquire goes unedited for this long and no longer.
                const VkResult acq = dc->vkAcquireNextImageKHR(dc->self, swapchain,
                                                               150ull * 1000ull * 1000ull,
                                                               VK_NULL_HANDLE, sc->repaintFence,
                                                               &index);
                if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
                    static std::atomic<uint32_t> n{0};
                    const uint32_t k = n.fetch_add(1);
                    if (k < 3 || k == 20 || k == 100)
                        Log("[layer] idle repaint: acquire gave %d (attempt %u)", (int) acq, k + 1);
                    continue;
                }
                if (dc->vkWaitForFences(dc->self, 1, &sc->repaintFence, VK_TRUE,
                                        500ull * 1000ull * 1000ull) != VK_SUCCESS) {
                    // Walking away here leaves the image acquired for good, and an acquired image is
                    // released by presenting it and by nothing else -- so the application is one
                    // image poorer for the rest of its life, and a swapchain that runs out is an
                    // application whose own acquire never returns. It cannot be presented either,
                    // since the acquire has not completed, so the fence is left alone for the driver
                    // to finish with and this swapchain simply stops being repainted.
                    Log("[layer] idle repaint: the acquired image never became ready; giving up on "
                        "this swapchain rather than presenting an image that is not ready");
                    sc->repaintFence = VK_NULL_HANDLE;   // leaked deliberately; it is still in use
                    sc->repaintOff = true;
                    continue;
                }
                dc->vkResetFences(dc->self, 1, &sc->repaintFence);
                if (index >= sc->images.size()) {
                    // Cannot happen, and handled anyway: the image is acquired and has to go back.
                    if (dc->releaseImages) {
                        VkReleaseSwapchainImagesInfoEXT rel{
                            VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT };
                        rel.swapchain = swapchain;
                        rel.imageIndexCount = 1;
                        rel.pImageIndices = &index;
                        dc->vkReleaseSwapchainImagesEXT(dc->self, &rel);
                    }
                    continue;
                }

                const bool drew =
                    on ? ProcessPresent(dc, *sc, sc->queue, sc->images[index], 0, nullptr,
                                         /*repaint=*/true, nullptr)
                       : RestorePresent(dc, *sc, sc->images[index]);
                if (drew) {
                    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                    pi.swapchainCount = 1;
                    pi.pSwapchains = &swapchain;
                    pi.pImageIndices = &index;
                    dc->vkQueuePresentKHR(sc->queue, &pi);
                    dc->repaintSeenCtrl = ctrl;
                    dc->repaintSeenTuning = tune;
                    dc->repaintWasEnabled = on;
                } else if (dc->releaseImages) {
                    // Handed back rather than dropped. An acquired image is released by presenting
                    // it and by nothing else, so one that cannot be drawn has to be returned or the
                    // swapchain is an image poorer for good.
                    VkReleaseSwapchainImagesInfoEXT rel{
                        VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT };
                    rel.swapchain = swapchain;
                    rel.imageIndexCount = 1;
                    rel.pImageIndices = &index;
                    dc->vkReleaseSwapchainImagesEXT(dc->self, &rel);
                } else {
                    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                    pi.swapchainCount = 1;
                    pi.pSwapchains = &swapchain;
                    pi.pImageIndices = &index;
                    dc->vkQueuePresentKHR(sc->queue, &pi);
                    Log("[layer] idle repaint: nothing drawn and no way to return the image; "
                        "not repainting again");
                    break;
                }
            }
        });
    }
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
    // Before anything it touches is destroyed, and outside the lock it takes.
    if (dc->repaintThread.joinable()) {
        dc->repaintRun.store(false);
        dc->repaintThread.join();
    }
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) ReleasePrimary(device, kv.first);
    }
    // Say the layer has gone. A reader that finds a pid here checks it is alive, so a crash is
    // caught too, but an orderly exit should not need anyone to go looking.
    if (dc->shm.hdr && dc->shm.hdr->layerPid.load() == uint32_t(getpid())) {
        dc->shm.hdr->layerPid.store(0);
        dc->shm.hdr->layerCompositionUp.store(0);
    }
    if (dc->vkDeviceWaitIdle) dc->vkDeviceWaitIdle(device);
    {
        std::lock_guard<std::mutex> lk(dc->lock);
        for (auto& kv : dc->swapchains) {
            SwapchainState& sc = kv.second;
            sc.comp.reset();
            if (sc.fenceLeg1) dc->vkDestroyFence(device, sc.fenceLeg1, nullptr);
            if (sc.fenceLeg2) dc->vkDestroyFence(device, sc.fenceLeg2, nullptr);
            if (sc.repaintFence) dc->vkDestroyFence(device, sc.repaintFence, nullptr);
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

// Announces itself before it blocks, so the repaint can get out of the way. See lifecycleWaiting.
struct LifecycleGuard {
    DeviceChain* dc;
    explicit LifecycleGuard(DeviceChain* d) : dc(d) { if (dc) dc->lifecycleWaiting.store(true); }
    ~LifecycleGuard() { if (dc) dc->lifecycleWaiting.store(false); }
};

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

    LifecycleGuard lifecycle(dc);
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
    LifecycleGuard lifecycle(dc);
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
        if (sc.repaintFence) dc->vkDestroyFence(device, sc.repaintFence, nullptr);
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
    if (reported.fetch_add(1) < 8) {
        if (r == VK_TIMEOUT) Log("[layer] %s -> TIMEOUT", what);
        else Log("[layer] %s -> %d", what, (int) r);
    }
    return false;
}

// Long enough that a slow part running a long pass chain at 4K is never cut off, short
// enough that a driver which has stopped without losing the device does not park the
// game's present thread for the rest of the session. A lost device already returns
// VK_ERROR_DEVICE_LOST from the wait rather than hanging, so this covers the other case:
// a stall with no result to report, which NoteVk could never see because the call never
// returned.
static constexpr uint64_t kFenceBudgetNs = 5ull * 1000ull * 1000ull * 1000ull;

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
                            const VkSemaphore* waitSemaphores, bool repaint,
                            bool* waitsConsumed) {
    if (waitsConsumed) *waitsConsumed = false;
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
        // Bounded: on a timeout leg2Pending stays set, so the next present waits again
        // rather than touching surfaces that compose may still be reading. Giving up for
        // one frame is the fail-open path -- the game's own image is presented -- and the
        // wait is retried until it succeeds or the device is lost.
        if (!NoteVk(dc, dc->vkWaitForFences(d, 1, &sc.fenceLeg2, VK_TRUE, kFenceBudgetNs),
                    "vkWaitForFences(leg2)"))
            return false;
        dc->vkResetFences(d, 1, &sc.fenceLeg2);
        sc.leg2Pending = false;
        sc.comp->WriteCapturedFrame();
    }

    dlssnr::FrameSettings fs = dlssnr::FrameSettings::Read(dc->shm.hdr);
    // A repaint holds the captured frame, whatever the setting says.
    //
    // Held, the frame is not read back from the swapchain while the encode, the model and the resolve
    // all run again -- which is the whole of what composing a still picture again means. Without it
    // this path reads the swapchain, and on a repaint the swapchain holds the previous composed
    // output: each redraw would compose on top of the last and the edits would stack.
    if (repaint) fs.holdFrame = 1;

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
    // Both dma-buf directions are off under NO_ROUNDTRIP: the surfaces they name are the
    // helper's, and the whole point of the switch is that the helper is not in this frame.
    // That puts the proxy and the answer back in host memory, where the substitution below
    // is one memcpy rather than a second GPU path written only for a diagnostic.
    sc.comp->SetDmaBuf(DmaBufEnabled() && !NoRoundTrip());

    if (!sc.comp->Prepare(sc.width, sc.height, sc.format, fs, linearHdr, hdrProxy, hdrTransfer)) {
        Log("[layer] composition cannot run here: %s", sc.comp->Reason());
        return false;
    }

    // A repaint has no picture of its own to fall back on.
    //
    // Prepare rebuilds when the model's raster changes, and a rebuild that cannot keep the captured
    // frame -- a real change of size or working format -- leaves nothing to compose again. The pass
    // would then read the frame from the swapchain, and on a repaint that image holds the previous
    // composed output, so the edit would land on top of the edit. Better to draw nothing and wait for
    // the application to produce a frame of its own.
    if (repaint && !sc.comp->HasCapturedFrame()) {
        static std::once_flag said;
        std::call_once(said, [] {
            Log("[layer] idle repaint: the rebuild could not keep the captured frame; waiting for a "
                "real one rather than composing the screen back onto itself");
        });
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
    if (sc.waitStages.size() < waitCount)
        sc.waitStages.resize(waitCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
    si.waitSemaphoreCount = waitCount;
    si.pWaitSemaphores = waitCount ? waitSemaphores : nullptr;
    si.pWaitDstStageMask = waitCount ? sc.waitStages.data() : nullptr;
    const auto dropWaits = [&] {
        si.waitSemaphoreCount = 0;
        si.pWaitSemaphores = nullptr;
        si.pWaitDstStageMask = nullptr;
    };

    const auto endAndSubmit = [&](VkFence fence) {
        if (!NoteVk(dc, dc->vkEndCommandBuffer(cb), "vkEndCommandBuffer")) return false;
        if (!NoteVk(dc, dc->vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) return false;
        if (waitsConsumed && si.waitSemaphoreCount != 0) *waitsConsumed = true;
        return true;
    };
    const auto waitAndReset = [&](VkFence fence) {
        const VkResult r = dc->vkWaitForFences(d, 1, &fence, VK_TRUE, kFenceBudgetNs);
        if (r == VK_TIMEOUT) {
            // Unlike leg 2's wait, this one cannot just be retried next frame: the command
            // buffer it covers is the one this function re-begins, and beginning a buffer
            // that is still executing is undefined. So the swapchain stands down for good
            // and stops touching that buffer, which lets the in-flight work drain on its
            // own. Pass-through is the fail-open state the rest of this file already uses.
            if (!sc.passThrough) {
                sc.passThrough = true;  // the caller releases the primary claim on seeing this
                Log("[layer] leg 1 fence did not signal within %llu ms; this swapchain is "
                    "now pass-through", (unsigned long long)(kFenceBudgetNs / 1000000ull));
            }
            return false;
        }
        if (!NoteVk(dc, r, "vkWaitForFences")) return false;
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
    if (NoRoundTrip()) {
        // The middle rung. Leg 1 recorded, submitted and fenced; leg 2 will record, submit and
        // compose. Between them, nothing is published, nothing is waited for, and no other process
        // is involved -- the answer is the proxy itself, so the composition runs over a model that
        // returned the frame unchanged.
        //
        // What survives this is ours: our two legs, the surfaces, the barriers, the compose shader.
        // What does not survive it is the transport or the helper, and DLSSNR_SKIP_EVALUATE tells
        // those two apart.
        static std::once_flag said;
        std::call_once(said, [] {
            Log("[layer] DLSSNR_NO_ROUNDTRIP=1: composing from the proxy. The helper is not being "
                "asked for anything and the model is not running.");
        });
        void* answer = sc.comp->ModelPixels();
        const void* proxy = sc.comp->ProxyPixels();
        if (!answer || !proxy) return false;
        if (answer != proxy) std::memcpy(answer, proxy, sc.comp->ModelBytes());
    } else if (!ShmProcessFrame(dc->shm, sc.comp->ModelWidth(), sc.comp->ModelHeight(), sc.comp->ModelBytes(),
                         sc.comp->ProxyPixels(), sc.comp->ModelPixels(), sc.comp->ProxyActive(),
                         answerViaFd, sc.comp->HdrProxyActive(),
                         /*frameRepeat=*/repaint || fs.holdFrame != 0)) {
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
        dc->shm.hdr->layerPid.store(uint32_t(getpid()));
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
    dc->lastPresentMs.store(NowMs(), std::memory_order_relaxed);

    // Whether this call's wait semaphores have already been consumed by a submit of ours. They are
    // handed to the first swapchain we actually process; every path after that presents with none,
    // because a semaphore signalled once may only be waited on once. Presenting with them a second
    // time is a wait that never completes -- which is what a second layer in the chain, Steam's
    // overlay among them, turns from a latent bug into a hang.
    bool waitsConsumed = false;

    // Held from here to the present at the end, alongside Hook_QueueSubmit{,2}. Vulkan requires
    // external synchronization for every operation on a queue; the repaint uses the application's
    // queue, so its acquire/submit/present must not overlap Dolphin's GPU submission thread.
    std::unique_lock<std::mutex> lk;
    if (!dc->inert && LayerEnabled()) {
        lk = std::unique_lock<std::mutex>(dc->lock);
        PollHotkeys(dc);
        if (!ShmNeuralEnabled(dc->shm)) return dc->vkQueuePresentKHR(queue, pPresentInfo);
        if (DryRun()) {
            // The bottom rung. The hooks are installed, the mapping is attached, the hotkeys are
            // polled -- and nothing else happens. No command buffer, no submit, no transport, no
            // helper.
            //
            // If the symptom survives this, it is not ours: what remains is the loader, the hook
            // itself, and whatever else is in the chain. Nothing under this file's control runs.
            static std::once_flag said;
            std::call_once(said, [] {
                Log("[layer] DLSSNR_DRY_RUN=1: presenting the game's own frames. No capture, no "
                    "compose, no round trip. Nothing is being enhanced.");
            });
            if (dc->shm.hdr) {
                dc->shm.hdr->layerPid.store(uint32_t(getpid()));
                dc->shm.hdr->layerCompositionUp.store(0);
                dc->shm.hdr->layerHeartbeat.fetch_add(1);
            }
            return dc->vkQueuePresentKHR(queue, pPresentInfo);
        }
        uint32_t family = 0;
        auto qit = dc->queueFamilies.find(queue);
        if (qit != dc->queueFamilies.end()) family = qit->second;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
            auto sit = dc->swapchains.find(pPresentInfo->pSwapchains[i]);
            if (sit == dc->swapchains.end()) continue;
            SwapchainState& sc = sit->second;
            if (sc.passThrough || pPresentInfo->pImageIndices[i] >= sc.images.size()) continue;
            // One swapchain drives the channel; the rest present raw. See ClaimPrimary.
            if (!ClaimPrimary(dc->self, pPresentInfo->pSwapchains[i], sc.width, sc.height)) {
                static std::atomic<uint32_t> n{0};
                const uint32_t k = n.fetch_add(1);
                if (k < 3) Log("[layer] present: swapchain %p is not primary", (void*)pPresentInfo->pSwapchains[i]);
                continue;
            }
            sc.queue = queue;
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
            bool submittedWaits = false;
            const bool composed = ProcessPresent(dc, sc, queue, sc.images[pPresentInfo->pImageIndices[i]],
                                                 waitCount, pPresentInfo->pWaitSemaphores,
                                                 /*repaint=*/false, &submittedWaits);
            waitsConsumed = waitsConsumed || submittedWaits;
            // ProcessPresent stands a swapchain down for good if its leg 1 fence never
            // signalled. Same reasoning as the CreateResources failure above: a claim
            // left on a swapchain that no longer drives the channel stops any peer from
            // taking it over.
            if (sc.passThrough) ReleasePrimary(dc->self, pPresentInfo->pSwapchains[i]);
            if (!composed) ++dc->framesPassedThrough;
            if (VerboseEnabled()) {
                Log("[present] swapchain=%p image=%u seq=%u composed=%d",
                    (void*)pPresentInfo->pSwapchains[i], pPresentInfo->pImageIndices[i],
                    dc->shm.hdr ? dc->shm.hdr->seq_req.load() : 0u, int(composed));
            }
            // On failure before the capture submit, leave the application's waits attached to the
            // original present. Once our first submit accepted them they have been consumed.
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

static DeviceChain* DeviceForQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    if (g_devices.size() == 1) return g_devices.begin()->second;
    for (auto& kv : g_devices) {
        std::lock_guard<std::mutex> dl(kv.second->lock);
        if (kv.second->queueFamilies.count(queue)) return kv.second;
    }
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit(VkQueue queue, uint32_t submitCount,
                                                        const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                         const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_QueueWaitIdle(VkQueue queue) {
    DeviceChain* dc = DeviceForQueue(queue);
    if (!dc || !dc->vkQueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkQueueWaitIdle(queue);
}

static VKAPI_ATTR VkResult VKAPI_CALL Hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                uint64_t timeout, VkSemaphore semaphore,
                                                                VkFence fence, uint32_t* pImageIndex) {
    DeviceChain* dc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) dc = it->second;
    }
    if (!dc || !dc->vkAcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;
    std::lock_guard<std::mutex> lk(dc->lock);
    return dc->vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
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
    if (!std::strcmp(n, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)Hook_AcquireNextImageKHR;
    if (!std::strcmp(n, "vkQueueSubmit")) return (PFN_vkVoidFunction)Hook_QueueSubmit;
    if (!std::strcmp(n, "vkQueueSubmit2")) return (PFN_vkVoidFunction)Hook_QueueSubmit2;
    if (!std::strcmp(n, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)Hook_QueueWaitIdle;
    if (!std::strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)Hook_QueuePresentKHR;
    return nullptr;
}

static PFN_vkVoidFunction LookupDeviceHook(const char* n) {
    if (!std::strcmp(n, "vkDestroyDevice")) return (PFN_vkVoidFunction)Hook_DestroyDevice;
    if (!std::strcmp(n, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue;
    if (!std::strcmp(n, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)Hook_GetDeviceQueue2;
    if (!std::strcmp(n, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)Hook_CreateSwapchainKHR;
    if (!std::strcmp(n, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)Hook_DestroySwapchainKHR;
    if (!std::strcmp(n, "vkAcquireNextImageKHR")) return (PFN_vkVoidFunction)Hook_AcquireNextImageKHR;
    if (!std::strcmp(n, "vkQueueSubmit")) return (PFN_vkVoidFunction)Hook_QueueSubmit;
    if (!std::strcmp(n, "vkQueueSubmit2")) return (PFN_vkVoidFunction)Hook_QueueSubmit2;
    if (!std::strcmp(n, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)Hook_QueueWaitIdle;
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
        // Said here as well as at the point of use, because the point of use is inside the present
        // path and a reader who scrolled past the first frame would never see it. A session run
        // under one of these is not a session that worked.
        if (DryRun() || NoRoundTrip())
            Log("=== kill switches active:%s%s -- this session is NOT enhancing anything ===",
                DryRun() ? " DLSSNR_DRY_RUN" : "", NoRoundTrip() ? " DLSSNR_NO_ROUNDTRIP" : "");
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
