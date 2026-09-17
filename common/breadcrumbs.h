#pragma once

// A fixed-size ring of where-were-we markers, dropped along the present path and dumped when
// something on it stops answering.
//
// The problem it exists for: a hung session looks the same from every vantage point we have. The
// layer times out waiting for the helper, marks the channel dead and retries; the helper's own
// watchdog fires on a call that never returned; a lost device arrives as a bare VkResult. None of
// them says which of the eight or nine steps in a frame the process was standing on, and from the
// shared-memory side a hung helper and a hung layer are indistinguishable -- both leave seq_req
// ahead of seq_resp and nothing else to read.
//
// So each process drops a crumb at every step it can be standing on, and prints the last kCount of
// them, newest first, with the time since the newest next to each. The step that never got its
// successor is the one that hung, and the gap in front of it says for how long.
//
// Cost, so that "leave it on" is a measurement rather than a hope: one relaxed fetch_add, one
// steady_clock read, and three relaxed stores per crumb. Ten crumbs a frame is well under a
// microsecond, against a frame budget of sixteen thousand. There is no allocation, no lock and no
// syscall on the drop path, so it stays on in release builds -- a diagnostic that has to be
// switched on before the crash it explains is not a diagnostic.
//
// The stage is a `const char* const` to a string literal, stored as a pointer. That keeps the drop
// to a single store and needs no table to be kept in step with an enum; the literals live in the
// binary that reads them, which is the same binary that wrote them, because each process owns its
// own ring. Nothing here crosses the process boundary -- a shared ring would need the strings to
// cross too, and the two sides hang for different reasons in different places anyway.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace dlssnr {

inline uint64_t BreadcrumbNowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Breadcrumbs {
    // A power of two, so the index wraps with a mask rather than a division. Sixty-four is about
    // six frames of the layer's path -- enough that the dump shows the frame that hung and the
    // ones before it looking normal, which is most of the diagnosis.
    static constexpr uint32_t kCount = 64;

    struct Crumb {
        std::atomic<const char*> stage{nullptr};
        std::atomic<uint32_t> detail{0};
        std::atomic<uint64_t> us{0};
    };

    Crumb ring[kCount];
    std::atomic<uint64_t> next{0};

    // `stage` must outlive the ring: pass a string literal, never a buffer.
    void Drop(const char* stage, uint32_t detail = 0) {
        const uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
        Crumb& c = ring[i & (kCount - 1)];
        // Time and detail first, stage last with a release: a reader that sees the stage sees the
        // rest of the entry with it. The reader is this same process in a failure path, so the
        // race is with the other threads presenting on other devices, not with itself.
        c.us.store(BreadcrumbNowUs(), std::memory_order_relaxed);
        c.detail.store(detail, std::memory_order_relaxed);
        c.stage.store(stage, std::memory_order_release);
    }

    // Newest first, with the age of each in milliseconds. `log` takes (const char* stage,
    // uint32_t detail, double ageMs) and is whatever the calling process logs through.
    template <class Log>
    void Dump(Log&& log) const {
        const uint64_t end = next.load(std::memory_order_acquire);
        if (end == 0) return;
        const uint64_t begin = end > kCount ? end - kCount : 0;
        const uint64_t now = BreadcrumbNowUs();
        for (uint64_t i = end; i > begin; --i) {
            const Crumb& c = ring[(i - 1) & (kCount - 1)];
            const char* stage = c.stage.load(std::memory_order_acquire);
            if (!stage) continue;
            const uint64_t us = c.us.load(std::memory_order_relaxed);
            log(stage, c.detail.load(std::memory_order_relaxed),
                double(now - us) / 1000.0);
        }
    }
};

}  // namespace dlssnr
