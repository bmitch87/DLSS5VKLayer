#pragma once

// A frame trace that is still readable after the interesting thing has scrolled past.
//
// The logs answer "is it slow now". They cannot answer "what happened forty seconds ago, twice",
// and that is the question every measurement in this project actually wants: the capture metrics,
// the A/B rounds, the GPU timer, the model's cost floor. None of them had a record with a time base
// to write into.
//
// This is that record. Two fixed-capacity rings per process, a CSV written once when the process
// stops, and an offline reader in tools/. Five things make it survive being useful, and each of
// them is here because leaving it out is the failure:
//
//   1. CAPACITY IS IN EVENTS, NOT SECONDS. A ring sized "about ten seconds" holds two seconds at
//      500 fps and a minute in a menu, and the reader has no way to know which. The count is fixed
//      and the span is whatever it was, stated in the file.
//
//   2. THE OVERWRITE COUNT IS IN THE FILE. A ring that wrapped and does not say so reads as a
//      complete session that happened to be short, which is the one misreading that makes every
//      number in it wrong.
//
//   3. WHOLE-SESSION AGGREGATES LIVE OUTSIDE THE RING. Count, total, maximum and "how many were
//      slow" per tag, updated on every event and never overwritten, so a session that filled the
//      ring twenty times over still reports its own shape.
//
//   4. A SECOND RING FOR THE SLOW EVENTS ONLY. The events worth finding are rare, so at any useful
//      capacity the recent ring throws exactly them away first. The slow ring keeps them until it
//      is full of them.
//
//   5. A #clock LINE AND A #complete MARKER. The clock line carries the tick rate, the session's
//      origin and a UTC time taken near it, so a trace can be lined up against the ordinary log.
//      The marker is the last line written; the reader refuses a file without it rather than
//      half-reading a truncated one.
//
// Switched on by DLSSNR_TRACE=1 in the SAME binary, so "is the instrumentation itself the problem"
// is an environment variable rather than a second build. Off, an event costs one predictable branch.
//
// CAVEAT, stated here rather than discovered later: these are CPU wall-clock times. A short GPU
// submission call does not mean the GPU work was short -- that is what the timestamp queries are
// for. And nested events contain one another, so their durations must never be summed.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace dlssnr {

inline uint64_t TraceNowUs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

class FrameTrace {
  public:
    // Sized for about ten minutes of a 60 Hz session at a dozen events a frame, at 32 bytes an
    // event: 8 MiB for the recent ring. DLSSNR_TRACE_EVENTS overrides it; a value that is not a
    // power of two is rounded down to one, because the index wraps with a mask.
    static constexpr uint32_t kDefaultRecent = 1u << 18;  // 262144
    static constexpr uint32_t kSlowCount = 2048;
    static constexpr uint32_t kMaxTags = 48;

    struct Event {
        const char* tag;
        uint64_t startUs;
        uint32_t durUs;
        uint32_t a;
        uint32_t b;
    };

    bool Enabled() const { return _on; }

    // Reads the environment once. Safe to call repeatedly; only the first call does anything.
    //
    // `dirOverride` exists for the helper, which is a PE running under Wine: a Linux directory from
    // the environment is not a path its fopen can reach, so the caller translates it and hands the
    // translated one in. The layer, native, passes nothing.
    void Init(const char* who, const char* dirOverride = nullptr) {
        if (_started.exchange(true)) return;
        const char* on = getenv("DLSSNR_TRACE");
        _on = on && on[0] == '1';
        if (!_on) return;
        _who = who;
        _recentCount = kDefaultRecent;
        if (const char* n = getenv("DLSSNR_TRACE_EVENTS"); n && *n) {
            const unsigned long want = strtoul(n, nullptr, 10);
            uint32_t p = 1024;
            while (p * 2u <= want && p < (1u << 24)) p *= 2u;
            _recentCount = p;
        }
        if (const char* s = getenv("DLSSNR_TRACE_SLOW_US"); s && *s)
            _slowUs = uint32_t(strtoul(s, nullptr, 10));
        _recent = (Event*) std::calloc(_recentCount, sizeof(Event));
        _slow = (Event*) std::calloc(kSlowCount, sizeof(Event));
        if (!_recent || !_slow) { _on = false; return; }
        if (dirOverride && *dirOverride)
            std::snprintf(_dir, sizeof(_dir), "%s", dirOverride);
        _originUs = TraceNowUs();
        _originUnix = uint64_t(std::time(nullptr));
    }

    // tag must outlive the trace: a string literal, as with the breadcrumbs.
    void Add(const char* tag, uint64_t startUs, uint64_t endUs, uint32_t a = 0, uint32_t b = 0) {
        if (!_on) return;
        const uint64_t dur = endUs > startUs ? endUs - startUs : 0;
        const uint32_t durUs = dur > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(dur);

        // Outside the rings, so overflowing them costs the shape of the session but not its totals.
        Agg& g = AggFor(tag);
        g.count.fetch_add(1, std::memory_order_relaxed);
        g.totalUs.fetch_add(durUs, std::memory_order_relaxed);
        uint32_t prevMax = g.maxUs.load(std::memory_order_relaxed);
        while (durUs > prevMax &&
               !g.maxUs.compare_exchange_weak(prevMax, durUs, std::memory_order_relaxed)) {}
        if (durUs >= _slowUs) g.slowCount.fetch_add(1, std::memory_order_relaxed);

        Store(_recent, _recentCount, _recentNext, _recentLost, tag, startUs, durUs, a, b);
        if (durUs >= _slowUs)
            Store(_slow, kSlowCount, _slowNext, _slowLost, tag, startUs, durUs, a, b);
    }

    // Writes the CSV. Returns the path written, or nullptr. Safe to call more than once: the second
    // call is a no-op, because a trace written twice is two truncated halves of one session.
    const char* Write() {
        if (!_on || _written.exchange(true)) return nullptr;
        const char* dir = _dir[0] ? _dir : getenv("DLSSNR_TRACE_DIR");
        if (!dir || !*dir) dir = getenv("XDG_STATE_HOME");
        char path[1024];
        if (dir && *dir)
            std::snprintf(path, sizeof(path), "%s/dlssnr-trace-%s-%llu.csv", dir, _who,
                          (unsigned long long) _originUnix);
        else
            std::snprintf(path, sizeof(path), "/tmp/dlssnr-trace-%s-%llu.csv", _who,
                          (unsigned long long) _originUnix);
        FILE* f = std::fopen(path, "w");
        if (!f) return nullptr;

        // The clock line first: tick rate, this session's origin on that clock, and a UTC second
        // taken next to it. Without all three a trace cannot be lined up with the ordinary log.
        std::fprintf(f, "#clock,ticks_per_second,1000000,origin_ticks,%llu,unix_utc,%llu\n",
                     (unsigned long long) _originUs, (unsigned long long) _originUnix);
        std::fprintf(f, "#process,%s\n", _who);
        std::fprintf(f, "#capacity,recent_events,%u,slow_events,%u,slow_threshold_us,%u\n",
                     _recentCount, kSlowCount, _slowUs);
        // Rule 2. A ring that wrapped without saying so reads as a short complete session.
        std::fprintf(f, "#overwritten,recent,%llu,slow,%llu\n",
                     (unsigned long long) _recentLost.load(),
                     (unsigned long long) _slowLost.load());
        std::fprintf(f, "#caveat,cpu wall clock; nested durations contain one another and must not "
                        "be summed\n");

        // Rule 3, before the rings, so a truncated tail still leaves the totals readable.
        std::fprintf(f, "#aggregate,tag,count,total_us,max_us,slow_count\n");
        for (uint32_t i = 0; i < _tagCount.load(); ++i) {
            const Agg& g = _aggs[i];
            const char* tag = _tags[i].load();
            if (!tag) continue;
            std::fprintf(f, "#agg,%s,%llu,%llu,%u,%llu\n", tag,
                         (unsigned long long) g.count.load(),
                         (unsigned long long) g.totalUs.load(), g.maxUs.load(),
                         (unsigned long long) g.slowCount.load());
        }

        std::fprintf(f, "ring,tag,start_us,duration_us,a,b\n");
        Dump(f, "recent", _recent, _recentCount, _recentNext.load());
        Dump(f, "slow", _slow, kSlowCount, _slowNext.load());
        // Rule 5. The analyser refuses a file that does not end here.
        std::fprintf(f, "#complete,1\n");
        std::fclose(f);
        std::snprintf(_path, sizeof(_path), "%s", path);
        return _path;
    }

    // For the paths that end the process without unwinding -- the hang watchdog above all. A trace
    // written only on a clean stop loses exactly the session it was needed for.
    void WriteIfEnabled() { if (_on) Write(); }

  private:
    struct Agg {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> totalUs{0};
        std::atomic<uint32_t> maxUs{0};
        std::atomic<uint64_t> slowCount{0};
    };

    // Keyed by the tag POINTER, so the lookup is a pointer compare over a handful of entries rather
    // than a string hash on the hot path. Every tag is a literal, so identical text from one call
    // site is one pointer.
    Agg& AggFor(const char* tag) {
        const uint32_t n = _tagCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (_tags[i].load(std::memory_order_relaxed) == tag) return _aggs[i];
        for (uint32_t i = 0; i < kMaxTags; ++i) {
            const char* expect = nullptr;
            if (_tags[i].compare_exchange_strong(expect, tag, std::memory_order_acq_rel)) {
                uint32_t seen = _tagCount.load(std::memory_order_relaxed);
                while (seen < i + 1 &&
                       !_tagCount.compare_exchange_weak(seen, i + 1, std::memory_order_release)) {}
                return _aggs[i];
            }
            if (expect == tag) return _aggs[i];
        }
        return _overflowAgg;
    }

    static void Store(Event* ring, uint32_t count, std::atomic<uint64_t>& next,
                      std::atomic<uint64_t>& lost, const char* tag, uint64_t startUs,
                      uint32_t durUs, uint32_t a, uint32_t b) {
        const uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= count) lost.fetch_add(1, std::memory_order_relaxed);
        Event& e = ring[i & (count - 1)];
        e.tag = tag;
        e.startUs = startUs;
        e.durUs = durUs;
        e.a = a;
        e.b = b;
    }

    void Dump(FILE* f, const char* name, const Event* ring, uint32_t count, uint64_t next) const {
        const uint64_t begin = next > count ? next - count : 0;
        for (uint64_t i = begin; i < next; ++i) {
            const Event& e = ring[i & (count - 1)];
            if (!e.tag) continue;
            std::fprintf(f, "%s,%s,%llu,%u,%u,%u\n", name, e.tag,
                         (unsigned long long) (e.startUs - _originUs), e.durUs, e.a, e.b);
        }
    }

    bool _on = false;
    const char* _who = "process";
    std::atomic<bool> _started{false};
    std::atomic<bool> _written{false};
    Event* _recent = nullptr;
    Event* _slow = nullptr;
    uint32_t _recentCount = 0;
    uint32_t _slowUs = 50000;  // 50 ms, their threshold; DLSSNR_TRACE_SLOW_US overrides
    std::atomic<uint64_t> _recentNext{0}, _slowNext{0};
    std::atomic<uint64_t> _recentLost{0}, _slowLost{0};
    std::atomic<const char*> _tags[kMaxTags]{};
    Agg _aggs[kMaxTags];
    Agg _overflowAgg;
    std::atomic<uint32_t> _tagCount{0};
    char _dir[512] = {0};
    uint64_t _originUs = 0;
    uint64_t _originUnix = 0;
    char _path[1024] = {0};
};

// One event, timed by its own scope. Nothing is recorded when the trace is off beyond the branch in
// Add, so these can sit on the present path unconditionally.
class TraceScope {
  public:
    TraceScope(FrameTrace& t, const char* tag, uint32_t a = 0, uint32_t b = 0)
        : _t(t), _tag(tag), _a(a), _b(b), _start(t.Enabled() ? TraceNowUs() : 0) {}
    ~TraceScope() { if (_t.Enabled()) _t.Add(_tag, _start, TraceNowUs(), _a, _b); }
    void Note(uint32_t a, uint32_t b = 0) { _a = a; _b = b; }
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

  private:
    FrameTrace& _t;
    const char* _tag;
    uint32_t _a, _b;
    uint64_t _start;
};

}  // namespace dlssnr
