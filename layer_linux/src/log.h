#pragma once
// One log sink for the whole layer.
//
// Split out of layer.cpp because the composition needs it too, and fixed while moving: the previous
// implementation allocated a fresh std::mutex on every single line and leaked it, which at present
// rates is a leak per frame.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace dlssnr {

inline std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

inline FILE* LogSink() {
    static FILE* f = [] {
        const char* p = getenv("DLSSNR_LOG");
        return p && *p ? fopen(p, "a") : stderr;
    }();
    return f ? f : stderr;
}

inline void Log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::lock_guard<std::mutex> lk(LogMutex());
    fprintf(LogSink(), "[dlssnr-layer] %s\n", buf);
    fflush(LogSink());
}

inline bool Verbose() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_VERBOSE");
        return p && p[0] == '1';
    }();
    return v;
}

inline bool TimeEnabled() {
    static const bool v = [] {
        const char* p = getenv("DLSSNR_TIME");
        return p && p[0] == '1';
    }();
    return v;
}

inline int TimeInterval() {
    static const int v = [] {
        const char* p = getenv("DLSSNR_TIME_EVERY");
        return p && *p ? atoi(p) : 30;
    }();
    return v > 0 ? v : 30;
}

inline double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace dlssnr
