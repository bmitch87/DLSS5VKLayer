#pragma once
#include <windows.h>
#include <csetjmp>
#include <cstdint>

namespace dlssnr {

// VEH + setjmp exception guard (MinGW has no __try/__except). Fail-closed:
// any exception inside Guarded() longjmps back and returns failValue.
void InstallGuard();
void RegisterModuleRange(const char* name, uintptr_t base, uintptr_t size);

// Has anything inside NGX faulted in this process, ever?
//
// Distinct from g_guardHits, which is per-thread and reset at every Guarded() entry so the
// VEH can disarm itself after a burst. This is process-wide and one-way, because it answers
// a different question with a different lifetime: not "is this call going badly" but "is
// this library still safe to call at all".
//
// Two crash dumps from another project on this model say it is not. After a fault while
// shutting NGX down, the faulting thread still held the SDK's critical section, and a later
// initialisation waited on that lock while the first waited on the backend. Their rule, and
// it costs nothing: after code you cannot debug has faulted, do not call back into it to
// tidy up.
bool NgxFaulted();
void NoteNgxFault();

// Guarded() turns a FAULT into a return value. It does nothing about a HANG: a call that
// never returns never reaches the code that would notice, and the helper then sits inside
// the driver while the layer times out, marks the channel dead and retries every five
// seconds for the rest of the session. The game keeps running -- fail-open works -- but the
// helper is a zombie and nothing says so.
//
// So the call announces itself and a watcher elsewhere decides it has taken too long. The
// call is deliberately NOT moved to a worker thread to be abandoned: it records into our
// command buffer and holds driver state, so a timeout cannot be recovered from in-process
// whatever thread it runs on. Since the honest end state is "stop and ask to be restarted"
// either way, observing costs far less than supervising and ends in the same place.
void NgxCallBegin(const char* what);
void NgxCallEnd();
// 0 when nothing is outstanding, else how long the current call has been running.
unsigned long long NgxCallOutstandingMs();
const char* NgxCallName();

struct NgxCallScope {
    explicit NgxCallScope(const char* what) { NgxCallBegin(what); }
    ~NgxCallScope() { NgxCallEnd(); }
    NgxCallScope(const NgxCallScope&) = delete;
    NgxCallScope& operator=(const NgxCallScope&) = delete;
};

extern thread_local jmp_buf g_guardJmp;
extern thread_local volatile bool g_guardActive;
extern thread_local volatile DWORD g_guardCode;
extern thread_local volatile int g_guardHits;

template <class R, class F>
R Guarded(F&& f, R failValue, DWORD* seh) {
    *seh = 0;
    g_guardHits = 0;
    if (setjmp(g_guardJmp) == 0) {
        g_guardActive = true;
        R result = f();
        g_guardActive = false;
        return result;
    }
    *seh = g_guardCode;
    return failValue;
}

}  // namespace dlssnr