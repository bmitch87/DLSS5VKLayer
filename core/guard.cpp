// Ported from standalone_runner/main.cpp:38-129 (verified fail-closed guard).
#include "guard.h"
#include <atomic>
#include "logging.h"
#include <csetjmp>
#include <cstdio>

namespace dlssnr {

thread_local jmp_buf g_guardJmp;
thread_local volatile bool g_guardActive = false;
thread_local volatile DWORD g_guardCode = 0;
thread_local volatile int g_guardHits = 0;

struct ModuleRange {
    const char* name = nullptr;
    uintptr_t base = 0;
    uintptr_t size = 0;
};
static ModuleRange g_ranges[4];

void RegisterModuleRange(const char* name, uintptr_t base, uintptr_t size) {
    for (auto& r : g_ranges) {
        if (!r.name || r.name == name) { r.name = name; r.base = base; r.size = size; return; }
    }
}

static void DescribeRange(uintptr_t addr, const char** outName, unsigned long long* outOffset) {
    for (auto& r : g_ranges) {
        if (r.name && addr >= r.base && addr < r.base + r.size) {
            *outName = r.name; *outOffset = addr - r.base; return;
        }
    }
    *outName = "exe/other"; *outOffset = addr;
}

static std::atomic<bool> g_ngxFaulted{false};

bool NgxFaulted() { return g_ngxFaulted.load(std::memory_order_relaxed); }
void NoteNgxFault() { g_ngxFaulted.store(true, std::memory_order_relaxed); }

static LONG WINAPI GuardVeh(EXCEPTION_POINTERS* ep) {
    if (!g_guardActive) return EXCEPTION_CONTINUE_SEARCH;
    // OutputDebugStringA/W raises these under Wine; never treat as a fault.
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 /*DBG_PRINTEXCEPTION_C*/ || code == 0x4001000A /*DBG_PRINTEXCEPTION_WIDE_C*/)
        return EXCEPTION_CONTINUE_SEARCH;

    g_guardCode = ep->ExceptionRecord->ExceptionCode;
    // One-way and process-wide: this is the fact that outlives the call, the thread and the
    // 24-hit disarm below. Set before anything else can go wrong on the way out.
    NoteNgxFault();
    if (++g_guardHits >= 24) { g_guardActive = false; return EXCEPTION_CONTINUE_SEARCH; }

    auto* rec = ep->ExceptionRecord;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    uintptr_t fault = rec->NumberParameters > 1 ? (uintptr_t)rec->ExceptionInformation[1] : 0;
    const char* ripMod = nullptr; unsigned long long ripOff = 0;
    DescribeRange(rip, &ripMod, &ripOff);

    void* retAddr = nullptr;
    uintptr_t rsp = (uintptr_t)ep->ContextRecord->Rsp;
    if (rsp > 0x10000 && rsp < 0x7fffffffffffULL) retAddr = *(void**)rsp;
    const char* retMod = nullptr; unsigned long long retOff = 0;
    DescribeRange((uintptr_t)retAddr, &retMod, &retOff);

    Log("[veh] code=%#x rip=%p (%s+0x%llx) ret=%p (%s+0x%llx) fault=%p rw=%llu",
        (uint32_t)rec->ExceptionCode, (void*)rip, ripMod, ripOff,
        retAddr, retMod, retOff, (void*)fault,
        rec->NumberParameters > 0 ? (unsigned long long)rec->ExceptionInformation[0] : 0);
    Log("[veh] rdi=%p rsi=%p rdx=%p rcx=%p r8=%p r9=%p r10=%p r11=%p",
        (void*)ep->ContextRecord->Rdi, (void*)ep->ContextRecord->Rsi,
        (void*)ep->ContextRecord->Rdx, (void*)ep->ContextRecord->Rcx,
        (void*)ep->ContextRecord->R8, (void*)ep->ContextRecord->R9,
        (void*)ep->ContextRecord->R10, (void*)ep->ContextRecord->R11);

    g_guardActive = false;
    longjmp(g_guardJmp, 1);
}

void InstallGuard() { AddVectoredExceptionHandler(1, GuardVeh); }

}  // namespace dlssnr