// Own NVSDK_NGX_Parameter implementation with the verified non-standard
// 16-slot vtable layout (no virtual destructor on base).
// Ported from standalone_runner/main.cpp:134-246.
#pragma once
#include "ngx_abi.h"
#include "logging.h"
#include <map>
#include <set>
#include <string>

namespace dlssnr {

// Which side is reading, and when. "Set" looks like "in effect", and it is not: a key
// whose name the DLL does not have is written into this map and never asked for again,
// and nothing said so. So every read records the phase it happened in, and DumpUnread()
// reports what was never asked for at all.
//
// The phases are separate because the answer differs by phase: a key read only during
// CreateFeature is latched, one read at every evaluate is live, and that distinction is
// the whole of the latched-vs-live question. Our own diagnostic readbacks are marked
// kNgxPhaseOurs so they cannot make a key look read by the DLL.
//
// An unread-key probe, after a technique reported by DXL (src/core/NgxParameterBag.h:126-146,
// AGPL-3.0 -- read, not copied).
enum : unsigned {
    kNgxPhaseOther = 1u,     // the DLL read it outside create/evaluate (init, teardown)
    kNgxPhaseCreate = 2u,    // inside CreateFeature
    kNgxPhaseEvaluate = 4u,  // inside EvaluateFeature
    kNgxPhaseOurs = 8u,      // our own readback; never evidence about the DLL
};

inline unsigned& NgxCurrentPhase() {
    static unsigned phase = kNgxPhaseOther;
    return phase;
}

// Scope rather than a pair of statements: a guarded NGX call can return through a longjmp
// out of the VEH, and a restore written after the call would not run on that path.
struct NgxPhaseScope {
    unsigned prev;
    explicit NgxPhaseScope(unsigned p) : prev(NgxCurrentPhase()) { NgxCurrentPhase() = p; }
    ~NgxPhaseScope() { NgxCurrentPhase() = prev; }
    NgxPhaseScope(const NgxPhaseScope&) = delete;
    NgxPhaseScope& operator=(const NgxPhaseScope&) = delete;
};

struct OwnParam final : NVSDK_NGX_Parameter {
    struct ParamVal {
        int kind = 0;  // 1=u64/int, 2=float, 3=double, 4=ptr
        mutable unsigned readMask = 0;  // which phases have read this key
        unsigned long long u = 0;
        float f = 0.0f;
        double d = 0.0;
        void* p = nullptr;
    };
    mutable std::map<std::string, ParamVal> m;

    NVSDK_NGX_Result miss(const char* n) const {
        // Our own diagnostic readbacks go through ParamGet*, which sets kNgxPhaseOurs.
        // Logging those blamed the DLL for a question we asked ourselves -- every session
        // reported DLSSNR.Available as a missing key, and that key is ours, not the DLL's.
        if (NgxCurrentPhase() == kNgxPhaseOurs) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        static std::set<std::string> logged;
        if (n && logged.insert(n).second)
            Log("[param-miss] DLL queried missing key: '%s'", n);
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

    static const char* PhaseName(unsigned mask) {
        const bool c = (mask & kNgxPhaseCreate) != 0;
        const bool e = (mask & kNgxPhaseEvaluate) != 0;
        const bool o = (mask & kNgxPhaseOther) != 0;
        if (c && e) return o ? "create+evaluate+other" : "create+evaluate";
        if (c) return o ? "create+other" : "create";
        if (e) return o ? "evaluate+other" : "evaluate";
        if (o) return "other";
        return "never";
    }

    // What the DLL never asked for. A key here either has a name this build does not
    // have, or belongs to a path this session never took -- both are worth knowing, and
    // neither is visible from the fact that we set it.
    void DumpUnread() const {
        unsigned unread = 0, total = 0;
        for (const auto& kv : m) {
            ++total;
            if ((kv.second.readMask & ~unsigned(kNgxPhaseOurs)) == 0) {
                ++unread;
                Log("[param-unread] never read by the DLL: '%s'", kv.first.c_str());
            } else {
                Log("[param-read] '%s' read at %s", kv.first.c_str(), PhaseName(kv.second.readMask));
            }
        }
        Log("[param-unread] %u of %u keys were never read by the DLL", unread, total);
    }

    // Slot 0 (0x00)
    void NVSDK_CONV Set(const char* n, void* v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 4; x.p = v; x.u = (unsigned long long)v;
    }
    // Slot 1 (0x08)
    void NVSDK_CONV Set(const char* n, unsigned long long v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 1; x.u = v; x.f = (float)v; x.d = (double)v;
    }
    // Slot 2 (0x10)
    void NVSDK_CONV Set(const char* n, float v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 2; x.f = v; x.d = (double)v; x.u = (unsigned long long)v;
    }
    // Slot 3 (0x18)
    void NVSDK_CONV Set(const char* n, double v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 3; x.d = v; x.f = (float)v; x.u = (unsigned long long)v;
    }
    // Slot 4 (0x20)
    void NVSDK_CONV Set(const char* n, unsigned int v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 1; x.u = (unsigned long long)v; x.f = (float)v; x.d = (double)v;
    }
    // Slot 5 (0x28)
    void NVSDK_CONV Set(const char* n, int v) override {
        if (!n) return;
        auto& x = m[n]; x = {}; x.kind = 1; x.u = (unsigned long long)(unsigned int)v; x.f = (float)v; x.d = (double)v;
    }

    // Slot 6 (0x30)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, double* v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) return miss(n);
        it->second.readMask |= NgxCurrentPhase();
        *v = (it->second.kind == 3) ? it->second.d : (it->second.kind == 2 ? (double)it->second.f : (double)it->second.u);
        if (Verbose()) Log("[param-get:double] '%s' -> %f", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 7 (0x38)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, unsigned long long* v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) return miss(n);
        it->second.readMask |= NgxCurrentPhase();
        *v = it->second.u;
        if (Verbose()) Log("[param-get:ull] '%s' -> %llu", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 8 (0x40)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, void** v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) { *v = nullptr; return miss(n); }
        it->second.readMask |= NgxCurrentPhase();
        *v = it->second.p ? it->second.p : (void*)(uintptr_t)it->second.u;
        if (Verbose()) Log("[param-get:void*] '%s' -> %p", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 9 (0x48)
    NVSDK_NGX_Result NVSDK_CONV GetReserved9(const char*, void*) const override {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    // Slot 10 (0x50)
    NVSDK_NGX_Result NVSDK_CONV GetReserved10(const char*, void*) const override {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    // Slot 11 (0x58)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, int* v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) return miss(n);
        it->second.readMask |= NgxCurrentPhase();
        *v = (int)it->second.u;
        if (Verbose()) Log("[param-get:int] '%s' -> %d", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 12 (0x60)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, unsigned int* v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) return miss(n);
        it->second.readMask |= NgxCurrentPhase();
        *v = (unsigned int)it->second.u;
        if (Verbose()) Log("[param-get:uint] '%s' -> %u", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 13 (0x68)
    NVSDK_NGX_Result NVSDK_CONV GetReserved13(const char*, void*) const override {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    // Slot 14 (0x70)
    NVSDK_NGX_Result NVSDK_CONV Get(const char* n, float* v) const override {
        if (!n || !v) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto it = m.find(n); if (it == m.end()) return miss(n);
        it->second.readMask |= NgxCurrentPhase();
        *v = (it->second.kind == 2) ? it->second.f : (it->second.kind == 3 ? (float)it->second.d : (float)it->second.u);
        if (Verbose()) Log("[param-get:float] '%s' -> %f", n, *v);
        return NVSDK_NGX_Result_Success;
    }
    // Slot 15 (0x78)
    void NVSDK_CONV Reset() override {
        Log("[param-reset]");
        m.clear();
    }
};

inline NVSDK_NGX_Result NVSDK_CONV ScalingRatioCallback(NVSDK_NGX_Parameter* parameters) noexcept {
    if (!parameters) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
}

}  // namespace dlssnr