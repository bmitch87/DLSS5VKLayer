// Ported from standalone_runner/main.cpp (verified Feature-18 Vulkan sequence):
//   IAT spoof  :283-365   guarded calls :741-774   param helpers :248-266
//   core init  :892-936   params        :938-1006  snippet init  :1008-1033
//   create     :1035-1061 eval params  :1063-1142  teardown      :1199-1213
#include "ngx_snippet.h"
#include "guard.h"
#include "logging.h"
#include "ngx_param.h"
#include <chrono>
#include <cstring>
#include <vector>

namespace dlssnr {

HMODULE g_layerModule = nullptr;

// ---------------------------------------------------------------------------
// Caller-identity spoof: IAT hook of KERNEL32!GetModuleFileNameW inside the
// snippet/core modules so they see "nvngx.dll" as the caller.
// ---------------------------------------------------------------------------
static DWORD WINAPI SpoofedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) noexcept {
    if (module == g_layerModule) {
        static constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
        constexpr DWORD LEN = ARRAYSIZE(AUTHORIZED_CALLER) - 1;
        if (!filename || !size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        if (size <= LEN) {
            if (size > 1) std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
        return LEN;
    }
    extern decltype(&GetModuleFileNameW) g_realGetModuleFileNameW;
    if (g_realGetModuleFileNameW) return g_realGetModuleFileNameW(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}
decltype(&GetModuleFileNameW) g_realGetModuleFileNameW = nullptr;

struct SpoofState { void** slot = nullptr; decltype(&GetModuleFileNameW) orig = nullptr; };
static SpoofState g_snippetSpoof, g_coreSpoof;

static void** FindImportedFunctionSlot(HMODULE module, const char* functionName) noexcept {
    if (!module || !functionName) return nullptr;
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= nt->OptionalHeader.SizeOfImage)
        return nullptr;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    const auto* descEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress + dir.Size);
    for (; desc < descEnd && desc->Name; ++desc) {
        if (desc->Name >= nt->OptionalHeader.SizeOfImage) continue;
        const char* lib = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(lib, "KERNEL32.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(lib, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* addrThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addrThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            const uint32_t rva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
            if (rva >= nt->OptionalHeader.SizeOfImage) return nullptr;
            const auto* imp = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + rva);
            if (std::strcmp(reinterpret_cast<const char*>(imp->Name), functionName) == 0)
                return reinterpret_cast<void**>(&addrThunk->u1.Function);
        }
    }
    return nullptr;
}

static bool InstallCallerSpoof(HMODULE module, SpoofState& state) {
    state.slot = FindImportedFunctionSlot(module, "GetModuleFileNameW");
    if (!state.slot) { Log("[spoof] module %p has no GetModuleFileNameW import", (void*)module); return false; }
    DWORD old = 0;
    if (!VirtualProtect(state.slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Log("[spoof] VirtualProtect failed (%lu)", GetLastError()); return false;
    }
    state.orig = reinterpret_cast<decltype(state.orig)>(
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(state.slot),
                                   reinterpret_cast<void*>(&SpoofedGetModuleFileNameW)));
    VirtualProtect(state.slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), state.slot, sizeof(void*));
    if (!state.orig) { Log("[spoof] original import was null"); return false; }
    g_realGetModuleFileNameW = state.orig;
    Log("[spoof] GetModuleFileNameW IAT hooked at %p (module %p)", (void*)state.slot, (void*)module);
    return true;
}

static void RemoveCallerSpoof(SpoofState& state) {
    if (!state.slot || !state.orig) return;
    DWORD old = 0;
    if (VirtualProtect(state.slot, sizeof(void*), PAGE_READWRITE, &old)) {
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(state.slot),
                                   reinterpret_cast<void*>(state.orig));
        VirtualProtect(state.slot, sizeof(void*), old, &old);
    }
    state = {};
}

// ---------------------------------------------------------------------------
// Param helpers + guarded NGX calls
// ---------------------------------------------------------------------------
static bool ParamSetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int v, DWORD* seh) {
    Guarded([&] { p->Set(n, v); return true; }, false, seh);
    return *seh == 0;
}
static bool ParamSetF(NVSDK_NGX_Parameter* p, const char* n, float v, DWORD* seh) {
    Guarded([&] { p->Set(n, v); return true; }, false, seh);
    return *seh == 0;
}
// Every readback in this file is a question we are asking ourselves, not one the DLL
// asked. Marking the phase here keeps our own reads out of the evidence about which keys
// the DLL uses, and stops miss() reporting a key we invented (DLSSNR.Available) as one
// the DLL wanted and we lacked.
static bool ParamGetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int* v, DWORD* seh) {
    NgxPhaseScope phase(kNgxPhaseOurs);
    return Guarded([&] { return NVSDK_NGX_SUCCEED(p->Get(n, v)); }, false, seh);
}
static bool ParamGetF(NVSDK_NGX_Parameter* p, const char* n, float* v, DWORD* seh) {
    NgxPhaseScope phase(kNgxPhaseOurs);
    return Guarded([&] { return NVSDK_NGX_SUCCEED(p->Get(n, v)); }, false, seh);
}

static NVSDK_NGX_Result CallInitExtSafely(FnVkInitExt fn, unsigned long long appId,
    const wchar_t* path, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
    NVSDK_NGX_Version version, DWORD* seh) noexcept {
    NgxCallScope watch("Init_Ext");
    return Guarded([&] { return fn(appId, path, instance, pd, device, version, nullptr); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallCreateSafely(FnVkCreateFeature fn, VkCommandBuffer cmd, int feature,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle, DWORD* seh) noexcept {
    NgxCallScope watch("CreateFeature");
    NgxPhaseScope phase(kNgxPhaseCreate);
    return Guarded([&] { return fn(cmd, feature, params, handle); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallEvaluateSafely(FnVkEvaluateFeature fn, VkCommandBuffer cmd,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, DWORD* seh) noexcept {
    NgxCallScope watch("EvaluateFeature");
    NgxPhaseScope phase(kNgxPhaseEvaluate);
    return Guarded([&] { return fn(cmd, handle, params, nullptr); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallReleaseSafely(FnVkReleaseFeature fn, NVSDK_NGX_Handle* handle, DWORD* seh) noexcept {
    NgxCallScope watch("ReleaseFeature");
    return Guarded([&] { return fn(handle); }, NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallShutdownSafely(FnVkShutdown1 fn, VkDevice device, DWORD* seh) noexcept {
    NgxCallScope watch("Shutdown1");
    return Guarded([&] { return fn(device); }, NVSDK_NGX_Result_FAIL_SEH, seh);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
static std::wstring ModuleDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_layerModule, path, MAX_PATH);
    std::wstring dir = path;
    auto sep = dir.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? L"." : dir.substr(0, sep);
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ResolveBinDir() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"DLSSNR_BIN_DIR", env, MAX_PATH) > 0 &&
        FileExists(std::wstring(env) + L"\\nvngx_dlssnr.dll"))
        return env;
    std::wstring dir = ModuleDir();
    if (FileExists(dir + L"\\nvngx_dlssnr.dll")) return dir;
    if (FileExists(dir + L"\\binaries\\nvngx_dlssnr.dll")) return dir + L"\\binaries";
    return L"";
}

static void RegisterPeRange(const char* name, HMODULE mod) {
    if (!mod) return;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    RegisterModuleRange(name, (uintptr_t)mod, nt->OptionalHeader.SizeOfImage);
}

// ---------------------------------------------------------------------------
// Load + init (everything up to and including CreateFeature(18))
// ---------------------------------------------------------------------------
// The minimum driver the loaded model says it needs, 0 when it did not say.
double g_modelMinDriver = 0.0;

// Which model is this, exactly?
//
// The load used to log an address and nothing else, so a bug report could not say which
// model produced it -- at a point where several projects have established that different
// builds of nvngx_dlssnr.dll behave differently, and where the same version number has been
// seen on two different builds.
//
// Three things, all cheap, none previously recorded: the PATH it was actually loaded from
// (more than one copy can be in reach under Wine -- the binaries directory, one left beside
// a game, one in the prefix); whether something ELSE had already loaded it before we did;
// and the version resource, which carries NGX-specific fields nothing in this tree read --
// the minimum driver the build expects and the GPU architecture it was built for. Those two
// are the mechanism behind most "it does not work on my card" reports in this niche, and
// they come from the binary rather than from a table we would have to maintain.
static void DescribeSnippet(HMODULE mod) {
    wchar_t pathW[MAX_PATH] = {};
    const DWORD got = g_realGetModuleFileNameW ? g_realGetModuleFileNameW(mod, pathW, MAX_PATH)
                                               : GetModuleFileNameW(mod, pathW, MAX_PATH);
    if (!got) { Log("[ngx] could not resolve the model's path"); return; }
    char path[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof(path) - 1, nullptr, nullptr);
    Log("[ngx] model path: %s", path);

    DWORD ignored = 0;
    const DWORD sz = GetFileVersionInfoSizeW(pathW, &ignored);
    if (!sz) { Log("[ngx] model has no version resource"); return; }
    std::vector<uint8_t> buf(sz);
    if (!GetFileVersionInfoW(pathW, 0, sz, buf.data())) return;

    // The string table is per language+codepage; ask which one this binary has rather than
    // assuming the usual 040904B0.
    struct LangCp { WORD lang, cp; };
    LangCp* lc = nullptr;
    UINT lcBytes = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lc, &lcBytes) ||
        lcBytes < sizeof(LangCp) || !lc)
        return;

    const wchar_t* keys[] = { L"FileVersion", L"NGXMinimumDriverVersion", L"NGXGpuArchitecture",
                              L"NGXApiVersion" };
    for (const wchar_t* key : keys) {
        wchar_t q[128];
        _snwprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%s", lc->lang, lc->cp, key);
        wchar_t* value = nullptr;
        UINT vlen = 0;
        if (!VerQueryValueW(buf.data(), q, (LPVOID*)&value, &vlen) || !value) continue;
        char k[64] = {}, v[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, key, -1, k, sizeof(k) - 1, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, value, -1, v, sizeof(v) - 1, nullptr, nullptr);
        Log("[ngx] model %s = %s", k, v);
        if (!wcscmp(key, L"NGXMinimumDriverVersion")) g_modelMinDriver = atof(v);
    }
}

bool NgxLoadAndInit(NgxSnippet& s, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                    uint32_t width, uint32_t height, VkCommandBuffer recordingCmd,
                    const NgxTuning& tuning) {
    if (s.disabled) return false;
    s.binDir = ResolveBinDir();
    if (s.binDir.empty()) { Log("[ngx] nvngx_dlssnr.dll not found (set DLSSNR_BIN_DIR)"); s.disabled = true; return false; }
    Log("[ngx] bin dir: %ls", s.binDir.c_str());

    // Did something already have it open? Under Wine a game, a previous tool or the runner
    // can have loaded a different copy first, and LoadLibraryEx would then hand us theirs
    // rather than the one we asked for -- which is a different situation from loading it
    // ourselves and is worth telling apart in a report.
    if (HMODULE already = GetModuleHandleW(L"nvngx_dlssnr.dll"))
        Log("[ngx] nvngx_dlssnr.dll was already loaded in this process at %p", (void*)already);

    s.snippet = LoadLibraryExW((s.binDir + L"\\nvngx_dlssnr.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!s.snippet) { Log("[ngx] LoadLibrary nvngx_dlssnr.dll failed (%lu)", GetLastError()); s.disabled = true; return false; }
    Log("[ngx] nvngx_dlssnr.dll loaded at %p", (void*)s.snippet);
    RegisterPeRange("nvngx_dlssnr.dll", s.snippet);
    DescribeSnippet(s.snippet);

    s.initExt = reinterpret_cast<FnVkInitExt>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_Init_Ext"));
    s.initExt2 = reinterpret_cast<FnVkInitExt>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_Init_Ext2"));
    s.initPlain = reinterpret_cast<FnVkInitExt>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_Init"));
    s.createFeature = reinterpret_cast<FnVkCreateFeature>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_CreateFeature"));
    s.evaluateFeature = reinterpret_cast<FnVkEvaluateFeature>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    s.releaseFeature = reinterpret_cast<FnVkReleaseFeature>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    s.shutdown1 = reinterpret_cast<FnVkShutdown1>(GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_Shutdown1"));
    if (!s.createFeature || !s.evaluateFeature || !s.releaseFeature || !s.shutdown1) {
        Log("[ngx] snippet Vulkan exports incomplete (create=%p eval=%p release=%p shutdown=%p)",
            (void*)s.createFeature, (void*)s.evaluateFeature, (void*)s.releaseFeature, (void*)s.shutdown1);
        s.disabled = true; return false;
    }
    Log("[ngx] snippet exports: init=%p create=%p", (void*)s.initExt, (void*)s.createFeature);

    if (!InstallCallerSpoof(s.snippet, g_snippetSpoof)) { s.disabled = true; return false; }

    // NVAPI. The handle is never called into by this code -- nvapi64.dll is loaded only so a snippet
    // that resolves NVAPI by name finds this copy rather than failing -- so when the runner already
    // supplies NVAPI (Proton/DXVK-NVAPI, which the launcher announces with DLSSNR_SKIP_NVAPI) we skip
    // it outright: forcing the vendored nvapi64.dll in there bypasses the DXVK-NVAPI override and
    // faults inside its DllMain. The load is guarded either way, because a bad nvapi64 has to degrade
    // to "no NVAPI", not take the whole helper down -- Guarded() is the only thing standing between a
    // faulting DllMain and an unhandled exception, and the old bare LoadLibraryExW had no such cover.
    wchar_t nvenv[MAX_PATH];
    const bool skipNvapi = GetEnvironmentVariableW(L"DLSSNR_SKIP_NVAPI", nvenv, MAX_PATH) > 0 &&
                           nvenv[0] != L'\0' && nvenv[0] != L'0';
    if (skipNvapi) {
        Log("[ngx] nvapi64.dll load skipped (runner supplies NVAPI)");
    } else {
        DWORD seh2 = 0;
        s.nvapi = Guarded([&] {
            return LoadLibraryExW((s.binDir + L"\\nvapi64.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }, (HMODULE)nullptr, &seh2);
        if (s.nvapi) RegisterPeRange("nvapi64.dll", s.nvapi);
        else Log("[ngx] nvapi64.dll not loaded (seh=%#x); continuing without it", seh2);
    }

    // Core (nvngx.dll): libmgr prerequisite for snippet init; also param allocator. Optional -- the
    // parameter allocator falls back to the snippet's own, then to an in-house implementation. Guarded
    // for the same reason as nvapi64: a faulting DllMain here must degrade, not kill the helper.
    std::wstring corePath = s.binDir + L"\\nvngx.dll";
    if (FileExists(corePath)) {
        DWORD seh2 = 0;
        s.core = Guarded([&] {
            return LoadLibraryExW(corePath.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }, (HMODULE)nullptr, &seh2);
        if (s.core) InstallCallerSpoof(s.core, g_coreSpoof);
        else Log("[core] nvngx.dll load faulted (seh=%#x); continuing without core", seh2);
    }
    if (s.core) {
        const char* projectId = "7c134ab9-9677-4af5-a2b2-bca943350861";
        typedef NVSDK_NGX_Result (NVSDK_CONV* FnCoreInitWithProjectID)(
            const char*, int, const char*, const wchar_t*,
            VkInstance, VkPhysicalDevice, VkDevice);
        typedef NVSDK_NGX_Result (NVSDK_CONV* FnCoreInitExt)(
            unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
            NVSDK_NGX_Version, const NVSDK_NGX_FeatureDiscoveryInfo*);
        auto coreInitProjectID = reinterpret_cast<FnCoreInitWithProjectID>(
            GetProcAddress(s.core, "NVSDK_NGX_VULKAN_Init_with_ProjectID"));
        if (!coreInitProjectID)
            coreInitProjectID = reinterpret_cast<FnCoreInitWithProjectID>(
                GetProcAddress(s.core, "NVSDK_NGX_VULKAN_Init_ProjectID"));
        auto coreInitExt = reinterpret_cast<FnCoreInitExt>(
            GetProcAddress(s.core, "NVSDK_NGX_VULKAN_Init_Ext"));
        bool coreInited = false;
        if (coreInitProjectID) {
            DWORD seh2 = 0;
            NVSDK_NGX_Result r = Guarded([&] {
                return coreInitProjectID(projectId, 3 /*CUSTOM*/, "Magpie-Experimental-0.5.7",
                    s.binDir.c_str(), instance, pd, device);
            }, NVSDK_NGX_Result_FAIL_SEH, &seh2);
            Log("[core] VULKAN_Init_with_ProjectID -> %#x seh=%#x", (uint32_t)r, seh2);
            coreInited = NVSDK_NGX_SUCCEED(r);
        } else {
            Log("[core] no Init_with_ProjectID export");
        }
        if (!coreInited && coreInitExt) {
            DWORD seh2 = 0;
            NVSDK_NGX_Result r = Guarded([&] {
                return coreInitExt(DLSSNR_SIGNED_SNIPPET_APPLICATION_ID, s.binDir.c_str(),
                    instance, pd, device, NVSDK_NGX_Version_API_14, nullptr);
            }, NVSDK_NGX_Result_FAIL_SEH, &seh2);
            Log("[core] VULKAN_Init_Ext -> %#x seh=%#x", (uint32_t)r, seh2);
            coreInited = NVSDK_NGX_SUCCEED(r);
        }
        Log("[core] init %s", coreInited ? "OK" : "FAILED (continuing)");
    } else {
        Log("[core] nvngx.dll not loadable");
    }

    // Parameters: DLL allocator preferred (core -> snippet), own 16-slot vtable
    // implementation only as fallback (notes section 4: blocks must come from the
    // matching API family's allocator).
    {
        DWORD seh2 = 0;
        NVSDK_NGX_Result r = NVSDK_NGX_Result_FAIL_Failure;
        // The unread-key probe only sees what passes through our own container, so a run
        // whose question is "which keys does the DLL actually read" has to take the
        // fallback deliberately rather than by accident. It is a diagnostic, not a mode:
        // the DLL allocator stays preferred everywhere else.
        const bool forceOwn = [] {
            const char* e = getenv("DLSSNR_FORCE_OWNPARAM");
            return e && e[0] == '1';
        }();
        if (forceOwn) Log("[params] DLSSNR_FORCE_OWNPARAM=1: skipping the DLL allocators");
        if (s.core && !forceOwn) {
            auto coreAlloc = reinterpret_cast<FnVkAllocateParameters>(
                GetProcAddress(s.core, "NVSDK_NGX_VULKAN_AllocateParameters"));
            auto coreDestroy = reinterpret_cast<FnVkDestroyParameters>(
                GetProcAddress(s.core, "NVSDK_NGX_VULKAN_DestroyParameters"));
            if (coreAlloc && coreDestroy) {
                r = Guarded([&] { return coreAlloc(&s.params); }, NVSDK_NGX_Result_FAIL_SEH, &seh2);
                Log("[params] core AllocateParameters -> %#x seh=%#x", (uint32_t)r, seh2);
                if (NVSDK_NGX_SUCCEED(r) && s.params) s.paramsDestroy = coreDestroy;
                else s.params = nullptr;
            }
        }
        if (!s.params && s.snippet && !forceOwn) {
            auto snipAlloc = reinterpret_cast<FnVkAllocateParameters>(
                GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_AllocateParameters"));
            auto snipDestroy = reinterpret_cast<FnVkDestroyParameters>(
                GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_DestroyParameters"));
            if (snipAlloc) {
                r = Guarded([&] { return snipAlloc(&s.params); }, NVSDK_NGX_Result_FAIL_SEH, &seh2);
                Log("[params] snippet AllocateParameters -> %#x seh=%#x", (uint32_t)r, seh2);
                if (NVSDK_NGX_SUCCEED(r) && s.params) s.paramsDestroy = snipDestroy;
                else s.params = nullptr;
            } else {
                Log("[params] snippet does not export AllocateParameters");
            }
        }
        if (!s.params) {
            s.params = new OwnParam();
            s.ownParams = true;
            Log("[params] using own NVSDK_NGX_Parameter implementation");
        } else {
            // Worth saying out loud: with a DLL-allocated block we cannot see which keys
            // the DLL reads, so the [param-unread] report below will be absent.
            Log("[params] container is the DLL's; the unread-key probe is unavailable "
                "(set DLSSNR_FORCE_OWNPARAM=1 to run it)");
        }
    }
    {
        DWORD seh2 = 0;
        bool ok = ParamSetUI(s.params, "__selftest", 0xC0FFEE, &seh2);
        unsigned int back = 0;
        ok = ok && ParamGetUI(s.params, "__selftest", &back, &seh2) && back == 0xC0FFEE;

        // The same-type round trip above says nothing about a CROSS-type read, and NGX's
        // own container is loose about types: the snippet may write as one and read as
        // another. Two cases, both of which we ship values for -- a float read back as an
        // integer, and the negative float DLSSNR.SkinStructureStrength defaults to.
        ok = ok && ParamSetF(s.params, "__selftest_f", 2.0f, &seh2);
        ok = ok && ParamGetUI(s.params, "__selftest_f", &back, &seh2) && back == 2u;
        float backF = 0.0f;
        ok = ok && ParamSetF(s.params, "__selftest_neg", -1.0f, &seh2);
        ok = ok && ParamGetF(s.params, "__selftest_neg", &backF, &seh2) && backF == -1.0f;

        Log("[params] round-trip self-test: %s (seh=%#x)", ok ? "PASS" : "FAIL", seh2);
        if (!ok) { s.disabled = true; return false; }
    }

    // Create parameters.
    //
    // This block used to be twice this size. The keys that are gone were never read by the
    // DLL -- established two ways that agree: no such literal exists in nvngx_dlssnr.dll's
    // string table, and a live session with the parameter container instrumented
    // (DLSSNR_FORCE_OWNPARAM=1, see OwnParam::DumpUnread) reported 38 of 83 keys never
    // asked for. Removed: DLSSNR.{InputWidth,InputHeight,OutputWidth,OutputHeight,
    // Output.Width,Output.Height,Upscaling,Scale,AutoExposure,Hdr,SDR,Jitter.Offset.X,
    // Jitter.Offset.Y}, the six literal NVSDK_NGX_Parameter_* spellings, the undotted
    // aliases (Color/Output/Depth/MVec/MotionVectors/Reset/Width/Height/JitterOffset*),
    // Feature_Flags, Sharpness, InPreExposure, InExposureScale and PerfQualityValue.
    //
    // They were not free. Each one reads to a maintainer as "we told the model this", and
    // two of them were load-bearing in comments elsewhere that were therefore wrong.
    //
    // What actually pins this pass to 1:1 is DLSSNR.ScalingRatio, which IS read, at create
    // and again at every evaluate. DLSSNR.Upscaling never existed; the comment that said it
    // was what kept the model from upscaling was describing a key the DLL does not have.
    //
    // DLSSNRComputeScalingRatioCallback is also gone: it is a real name in the binary, but
    // the probe shows it is never called for, because ScalingRatio is supplied directly.
    // PerfQualityValue is a real name too, with its own error strings in the DLL, and was
    // still never read on this path -- it belongs to a scaling-ratio route we do not take.
    DWORD seh = 0;
    bool ps = true;
    ps &= ParamSetUI(s.params, "DLSSNR.Width", width, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Height", height, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.ScalingRatio", 1.0f, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Hint.Render.Preset", 0u, &seh);
    ps &= ParamSetUI(s.params, "CreationNodeMask", 1u, &seh);
    ps &= ParamSetUI(s.params, "VisibilityNodeMask", 1u, &seh);

    // No Feature_Flags, and this is the correction that matters most in this file.
    //
    // DoSharpening, AutoExposure and IsHDR were computed here and written to Feature_Flags
    // and its macro-name twin. Neither key exists in the DLL and the probe confirms neither
    // is ever read, so none of those three flags has ever reached the model -- by this
    // route or any other, because everything the model receives goes through the parameter
    // block. There is no flags key among the 61 DLSSNR.* names either.
    //
    // So whatever the HDR path achieves, it achieves through what the layer does to the
    // pixels and through the resource formats we bind, not through a signal the model was
    // given. ApplyHdrContract still re-derives hdrActive because the rest of the helper
    // keys off it -- the crossing image formats, the proxy width, the rebuild -- and that
    // part is real. What is not real is the idea that the model was told.
    //
    // The same goes for exposure: InPreExposure, InExposureScale and DLSSNR.AutoExposure
    // were all unread. A scan of the binary finds no exposure parameter under any name,
    // while it does contain a CUDA kernel called cuda_capture_output_exposure_scale_kernel
    // -- the model scales its output by an exposure it computes internally, with nothing to
    // steer it. The only exposure control anyone has over this model is what the proxy
    // looks like, which is the white-point meter's job.
    const char* hdrEnv = getenv("DLSSNR_HDR");
    const bool wantHdr = s.hdrActive || (hdrEnv && hdrEnv[0] == '1');
    Log("[params] create contract set: %s (seh=%#x) hdr=%d", ps ? "ok" : "FAILED", seh,
        int(wantHdr));

    // Snippet Init_Ext: (appId, path, instance, pd, device, version, featureInfo=nullptr)
    NVSDK_NGX_Result initResult = NVSDK_NGX_Result_FAIL_NotInitialized;
    for (NVSDK_NGX_Version ver : { NVSDK_NGX_Version_API_14, NVSDK_NGX_Version_API_13 }) {
        if (s.initExt) {
            initResult = CallInitExtSafely(s.initExt, DLSSNR_SIGNED_SNIPPET_APPLICATION_ID,
                s.binDir.c_str(), instance, pd, device, ver, &seh);
            Log("[ngx] VULKAN_Init_Ext(ver=0x%x) -> %#x seh=%#x", ver, (uint32_t)initResult, seh);
            if (NVSDK_NGX_SUCCEED(initResult)) break;
        }
        if (!NVSDK_NGX_SUCCEED(initResult) && s.initExt2) {
            initResult = CallInitExtSafely(s.initExt2, DLSSNR_SIGNED_SNIPPET_APPLICATION_ID,
                s.binDir.c_str(), instance, pd, device, ver, &seh);
            Log("[ngx] VULKAN_Init_Ext2(ver=0x%x) -> %#x seh=%#x", ver, (uint32_t)initResult, seh);
            if (NVSDK_NGX_SUCCEED(initResult)) break;
        }
        if (!NVSDK_NGX_SUCCEED(initResult) && s.initPlain) {
            initResult = CallInitExtSafely(s.initPlain, DLSSNR_SIGNED_SNIPPET_APPLICATION_ID,
                s.binDir.c_str(), instance, pd, device, ver, &seh);
            Log("[ngx] VULKAN_Init(ver=0x%x) -> %#x seh=%#x", ver, (uint32_t)initResult, seh);
            if (NVSDK_NGX_SUCCEED(initResult)) break;
        }
    }
    if (!NVSDK_NGX_SUCCEED(initResult)) {
        Log("[ngx] snippet init failed, disabling layer");
        s.disabled = true;
        return false;
    }

    // The snippet exports GetScratchBufferSize and we have never called it -- it was the one
    // declared export nothing resolved. Another project listed "CreateFeature may need
    // GetScratchBufferSize satisfied first" among the theories it could not test; we can, so
    // this asks and logs the answer rather than leaving it open. Our creates already succeed,
    // so a non-zero requirement here would be a surprise worth seeing, and a zero retires the
    // question. The number also belongs in any per-pass VRAM accounting.
    {
        auto scratch = reinterpret_cast<FnVkGetScratchBufferSize>(
            GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_GetScratchBufferSize"));
        if (scratch) {
            DWORD seh2 = 0;
            size_t bytes = 0;
            NVSDK_NGX_Result r = Guarded([&] { return scratch(FEATURE_DLSSNR, s.params, &bytes); },
                                         NVSDK_NGX_Result_FAIL_SEH, &seh2);
            Log("[ngx] GetScratchBufferSize(18) -> %#x seh=%#x bytes=%llu", (uint32_t)r, seh2,
                (unsigned long long)bytes);
        } else {
            Log("[ngx] snippet does not export GetScratchBufferSize");
        }
    }

// Public Vulkan NGX contract: query Feature-18 requirements before create.
    {
        auto reqs2 = reinterpret_cast<FnVkGetFeatureReqs2>(
            GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_GetFeatureRequirements"));
        if (reqs2) {
            DWORD seh2 = 0;
            NVSDK_NGX_FeatureRequirements fr{};
            NVSDK_NGX_Result r = Guarded([&] { return reqs2(instance, pd, &fr); },
                                         NVSDK_NGX_Result_FAIL_SEH, &seh2);
            Log("[reqs] GetFeatureRequirements -> %#x seh=%#x ver=%u.%u flags=%#x minGPU=%u inGPU=%u cs=%u.%u",
                (uint32_t)r, seh2, fr.Version.Major, fr.Version.Minor, fr.FeatureFlags,
                fr.MinGPUMode, fr.InGPUMode, fr.MinCSMajorVersion, fr.MinCSMinorVersion);
            if (NVSDK_NGX_SUCCEED(r)) {
                s.featureFlags = fr.FeatureFlags;
                s.hdrCapable = (fr.FeatureFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0u;
                s.hdrActive = wantHdr && s.hdrCapable;
            }
        }
    }

    // The tonemap hint that used to be written here -- DLSSNR.Hdr / DLSSNR.SDR -- is gone:
    // neither key is in the DLL and neither was ever read. The decision itself still
    // matters to us, so it is still computed and logged; it just is not sent anywhere.
    {
        const bool hdrPath = wantHdr && s.hdrCapable;
        Log("[params] hdr path: %s (featureFlags=%#x) -- not sent, the model has no such key",
            hdrPath ? "HDR" : "SDR", s.featureFlags);
    }

    // Last, so neither the create contract above nor the tonemap hint can overwrite it. Its preset
    // write in particular used to land after everything the caller chose.
    NgxSetCreateTuning(s, tuning);

    bool created = NgxCreatePass(s, 0, width, height, recordingCmd);
    if (!created && s.snippet && s.params) {
        auto reqs = reinterpret_cast<FnVkGetFeatureRequirements>(
            GetProcAddress(s.snippet, "NVSDK_NGX_VULKAN_GetFeatureRequirements"));
        if (reqs) {
            DWORD seh3 = 0;
            NVSDK_NGX_Result r = Guarded([&] { return reqs(instance, pd, s.params); },
                                         NVSDK_NGX_Result_FAIL_SEH, &seh3);
            Log("[diag] GetFeatureRequirements -> %#x seh=%#x", (uint32_t)r, seh3);
            unsigned int avail = 0;
            ParamGetUI(s.params, "DLSSNR.Available", &avail, &seh3);
            Log("[diag] DLSSNR.Available=%u", avail);
        }
    }
    return created;
}

void NgxSetCreateTuning(NgxSnippet& s, const NgxTuning& t) {
    if (!s.params) return;
    DWORD seh = 0;
    bool ok = true;
    ok &= ParamSetUI(s.params, "DLSSNR.Hint.Render.Preset", t.preset, &seh);
    ok &= ParamSetUI(s.params, "DLSSNR.Style", t.style, &seh);
    ok &= ParamSetF(s.params, "DLSSNR.Intensity", t.intensity, &seh);
    ok &= ParamSetF(s.params, "DLSSNR.LocalToneStrength", t.localTone, &seh);
    ok &= ParamSetF(s.params, "DLSSNR.LocalStructureStrength", t.localStructure, &seh);
    ok &= ParamSetF(s.params, "DLSSNR.SkinStructureStrength", t.skinStructure, &seh);
    ok &= ParamSetUI(s.params, "DLSSNR.UseAutoMask", t.autoMask, &seh);
    if (!ok) Log("[params] create tuning FAILED (seh=%#x)", seh);
    else
        Log("[params] create tuning: preset=%u style=%u intensity=%.2f tone=%.2f structure=%.2f "
            "skin=%.2f automask=%u",
            t.preset, t.style, t.intensity, t.localTone, t.localStructure, t.skinStructure, t.autoMask);
}

// No FreeMemOnReleaseFeature here, and that is a measured answer rather than an omission.
//
// NGX is documented to pool a feature's memory across an ordinary ReleaseFeature so the next
// create is cheap, and the published escape is to ask for it back with the
// NVSDK_NGX_Parameter_FreeMemOnReleaseFeature parameter -- which would matter to us, because
// MaintainPasses releases the tail of the chain when the pass count drops and expects the
// memory back. But the key does not exist in this model: no such string appears in
// nvngx_dlssnr.dll, and it is not among the 61 DLSSNR.* names either. Setting it would be a
// control with nothing listening.
//
// Whether the memory actually comes back is now observable instead: MaintainPasses samples
// the device-local heap around every build, so a shrink that frees nothing would show as a
// price that never drops.
void NgxReleasePass(NgxSnippet& s, uint32_t pass, VkDevice device) {
    if (pass >= kMaxPasses || !s.features[pass] || !s.releaseFeature) return;
    // Never free under the GPU. The helper submits and fences every evaluate, so waiting on the
    // device here is enough and is cheaper to reason about than parking the handle for N frames.
    (void) device;
    DWORD seh = 0;
    NVSDK_NGX_Result r = CallReleaseSafely(s.releaseFeature, s.features[pass], &seh);
    Log("[ngx] ReleaseFeature pass %u -> %#x seh=%#x", pass, (uint32_t)r, seh);
    s.features[pass] = nullptr;
}

void NgxReleaseAllPasses(NgxSnippet& s, VkDevice device) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) NgxReleasePass(s, i, device);
    s.featureCount = 0;
    s.ready = false;
}

void NgxSetHdr(NgxSnippet& s, bool want) {
    // Raw: the caller decides, the create either succeeds or the helper falls back. Clamping to
    // hdrCapable here would silently swallow the request before init has learned the capability.
    s.hdrActive = want;
}

// There is no HDR contract to write, and finding that out is what this function is now for.
//
// It used to set Feature_Flags (with IsHDR), its macro-name twin, DLSSNR.Hdr and DLSSNR.SDR
// before every create, on the reasoning that restating them at create is what lets an HDR
// toggle take effect on the next feature build rather than never. The reasoning was sound
// and the keys were not: none of those four exists in nvngx_dlssnr.dll, and an instrumented
// session confirms none is ever read. The model has never been told whether the frame is
// HDR.
//
// Kept as a named no-op rather than deleted, because the call sites read as a contract and
// somebody will otherwise re-add one. What actually carries HDR is on our side of the
// boundary: the layer's encode, the crossing image formats, and hdrActive driving the
// rebuild. Those are real and unaffected.
static void ApplyHdrContract(NgxSnippet& s) {
    (void)s;
}

bool NgxCreatePass(NgxSnippet& s, uint32_t pass, uint32_t width, uint32_t height,
                   VkCommandBuffer recordingCmd) {
    if (s.disabled || !s.params || pass >= kMaxPasses) return false;
    // The layer already refuses to send these, but this is the process that touches the GPU, so it
    // is the one that has to be safe against any client: at 1x1 the model builds happily and the
    // first submit hangs the channel (Xid 109), which kills the game as well as this helper.
    if (width < kMinW || height < kMinH) {
        Log("[ngx] refusing feature at %ux%u: below the %ux%u floor", width, height, kMinW, kMinH);
        return false;
    }
    if (s.features[pass]) return true;

    ApplyHdrContract(s);

    DWORD seh = 0;
    const auto t0 = std::chrono::steady_clock::now();
    NVSDK_NGX_Result createResult = CallCreateSafely(s.createFeature, recordingCmd,
        FEATURE_DLSSNR, s.params, &s.features[pass], &seh);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    Log("[ngx] VULKAN_CreateFeature(18) pass %u -> %#x seh=%#x handle=%p size=%ux%u in %.0f ms",
        pass, (uint32_t)createResult, seh, (void*)s.features[pass], width, height, ms);
    if (!NVSDK_NGX_SUCCEED(createResult) || !s.features[pass]) {
        s.features[pass] = nullptr;
        // Only the first pass failing is fatal; a later one failing simply caps the chain, which is
        // what a memory ceiling looks like and is not a reason to lose the pass altogether.
        // A failed HDR create is not a dead snippet -- it is the model refusing float input, and
        // the helper answers by rebuilding at 8-bit. Only an SDR create failure is fatal.
        if (pass == 0 && !s.hdrActive) s.disabled = true;
        return false;
    }
    s.featureW = width;
    s.featureH = height;
    if (pass + 1 > s.featureCount) s.featureCount = pass + 1;
    s.ready = true;
    if (pass == 0)
        Log("[ngx] STATUS: Feature=18 created=true path=vulkan-snippet size=%ux%u", width, height);
    return true;
}

// ---------------------------------------------------------------------------
// Evaluate parameters (extracted_pipeline_notes.md section 5)
// ---------------------------------------------------------------------------
void NgxSetResources(NgxSnippet& s, const NVSDK_NGX_Resource_VK& color,
                     const NVSDK_NGX_Resource_VK& out, const NVSDK_NGX_Resource_VK& mv,
                     const NVSDK_NGX_Resource_VK& depth, uint32_t width, uint32_t height,
                     uint32_t mvecWidth, uint32_t mvecHeight,
                     const NVSDK_NGX_Resource_VK* controlMask) {
    if (!s.params) return;
    s.resColor = color; s.resOut = out; s.resMV = mv; s.resDepth = depth;
    if (controlMask) s.resControlMask = *controlMask;
    DWORD seh = 0;
    Guarded([&] {
        const bool hasDepth = s.resDepth.Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE;
        s.params->Set("DLSSNR.Color", &s.resColor);
        s.params->Set("DLSSNR.Output", &s.resOut);
        s.params->Set("DLSSNR.MVec", &s.resMV);
        s.params->Set("DLSSNR.Depth", hasDepth ? &s.resDepth : (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.ControlMask",
                      controlMask ? &s.resControlMask : (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.UI", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.UIAlpha", (const NVSDK_NGX_Resource_VK*)nullptr);
        // DLSSNR.Backbuffer is one of the 61 real keys and the DLL asks for it at every evaluate.
        // We have nothing to put there that is not already DLSSNR.Color -- our "backbuffer" IS the
        // frame the model is being shown -- so the probe is to hand it the output surface and see
        // whether anything changes. One line, so that "we pass null" is a measured choice rather
        // than an assumption. The answer on this model is that it changes nothing; see the log.
        static const bool bindBackbuffer = [] {
            const char* p = getenv("DLSSNR_BIND_BACKBUFFER");
            return p && p[0] == '1';
        }();
        s.params->Set("DLSSNR.Backbuffer",
                      bindBackbuffer ? &s.resOut : (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.BidirectionalDistortionField", (const NVSDK_NGX_Resource_VK*)nullptr);
        // The undotted aliases (Color, Output, Depth, MotionVectors, MVec) are gone: the
        // probe reports them never read, and the DLSSNR.* names above are the ones the model
        // asks for. The five nulls stay -- the DLL asks for all five every evaluate, and
        // "no resource" is the answer it is meant to get.
        //
        // It also asks for the four subrect keys of each of those five, which we do not set;
        // that is harmless while the resources are null, and it confirms that the DLL infers
        // no subrect from a resource. Anything that ever binds one of them owes it a subrect.
        return true;
    }, false, &seh);

    const char* subrectNames[][4] = {
        { "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
        { "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
        { "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
        { "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" },
    };
    // The MVec subrect is the one that is not simply the frame. It was set to the full
    // raster unconditionally, which was true while the motion field was always full-size and
    // always entirely written -- and stops being true the moment any of it is produced at a
    // reduced size or zeroed by a confidence gate. Telling the model that every pixel of the
    // field is a measurement when part of it is not is exactly what the subrect parameters
    // exist to prevent.
    for (size_t i = 0; i < sizeof(subrectNames) / sizeof(subrectNames[0]); ++i) {
        const bool isMVec = i == 2;
        const uint32_t sw = isMVec ? mvecWidth : width;
        const uint32_t sh = isMVec ? mvecHeight : height;
        const char* const* n = subrectNames[i];
        ParamSetUI(s.params, n[0], 0, &seh);
        ParamSetUI(s.params, n[1], 0, &seh);
        ParamSetUI(s.params, n[2], sw, &seh);
        ParamSetUI(s.params, n[3], sh, &seh);
    }
    // Gone from this block, all confirmed never read: the dotted and undotted jitter keys
    // (this pass has no jitter anyway), the undotted Reset/Width/Height aliases whose
    // DLSSNR.* twins are the ones the model asks for, and Sharpness -- see NgxSetSharpness.
    bool ps = true;
    ps &= ParamSetF(s.params, "DLSSNR.MVecScaleX", 1.0f, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.MVecScaleY", 1.0f, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.DepthInverted", 1u, &seh);
    ps &= ParamSetUI(s.params, "DLSS.Indicator.Invert.X.Axis", 0u, &seh);
    ps &= ParamSetUI(s.params, "DLSS.Indicator.Invert.Y.Axis", 0u, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Enabled", 1u, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Reset", 1u, &seh);

    // Style, Intensity, LocalTone, LocalStructure, SkinStructure and UseAutoMask are absent
    // HERE and written per pass by NgxSetEvaluateTuning instead. The distinction is the
    // whole point and the old comment in this place had it backwards.
    //
    // They used to be written here as CONSTANTS, every frame, which was a real bug: it left
    // the parameter block holding those constants for whatever created a feature next, so a
    // feature built at any moment other than immediately after NgxSetCreateTuning got
    // defaults whatever the user had chosen. UseAutoMask was the clearest case -- the
    // constant was 0, forcing the automatic skin mask off regardless of the setting.
    //
    // Removing the constants was right. The conclusion bolted onto it -- "the model latches
    // them at creation, so writing them at evaluate does nothing at all" -- was wrong, and
    // it was stated as settled fact here and in core/ngx_snippet.h for a long time. An
    // instrumented session says otherwise: DLSSNR.{Intensity,LocalToneStrength,
    // LocalStructureStrength,SkinStructureStrength,Style,UseAutoMask} are read at EVALUATE,
    // every frame. The only one of the seven read at create is DLSSNR.Hint.Render.Preset.
    //
    // That inverts the consequence. Writing them only at create does not make them latched;
    // it means every pass evaluates with whatever the most recently built feature left in
    // the block -- so in a multi-pass chain with per-pass overrides, every pass ran with
    // some other pass's tuning. The fix is not to put constants back, it is to write each
    // pass's own resolved values immediately before that pass's evaluate.
    // Read back once per change rather than once per pass per frame: this runs on every evaluate in
    // a multipass chain, and the readback is a diagnostic, not a step.
    unsigned int autoMask = 0;
    float mvecScaleX = 0.0f, mvecScaleY = 0.0f;
    ParamGetUI(s.params, "DLSSNR.UseAutoMask", &autoMask, &seh);
    ParamGetF(s.params, "DLSSNR.MVecScaleX", &mvecScaleX, &seh);
    ParamGetF(s.params, "DLSSNR.MVecScaleY", &mvecScaleY, &seh);
    const bool hasDepthNow = s.resDepth.Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE;
    if (Verbose() || !ps || autoMask != s.loggedAutoMask || mvecScaleX != s.loggedMVecScaleX ||
        mvecScaleY != s.loggedMVecScaleY || hasDepthNow != s.loggedDepthBound) {
        s.loggedAutoMask = autoMask;
        s.loggedMVecScaleX = mvecScaleX;
        s.loggedMVecScaleY = mvecScaleY;
        s.loggedDepthBound = hasDepthNow;
        Log("[params] evaluate contract set: %s (seh=%#x) UseAutoMask=%u MVecScaleX=%.6f MVecScaleY=%.6f depth=%s",
            ps ? "ok" : "FAILED", seh, autoMask, mvecScaleX, mvecScaleY,
            hasDepthNow ? "bound" : "null");
    }
}

void NgxSetReset(NgxSnippet& s, bool reset, bool logValue) {
    if (!s.params) return;
    DWORD seh = 0;
    // One key, not two: the undotted "Reset" alias was written beside this one and the probe
    // reports it never read. DLSSNR.Reset is the name the model asks for, at every evaluate.
    const bool ok1 = ParamSetUI(s.params, "DLSSNR.Reset", reset ? 1u : 0u, &seh);
    if (!logValue) return;
    unsigned int back = 0;
    ParamGetUI(s.params, "DLSSNR.Reset", &back, &seh);
    Log("[params] DLSSNR.Reset requested=%u readback=%u ok=%d seh=%#x",
        reset ? 1u : 0u, back, int(ok1), seh);
}

void NgxSetMotionScale(NgxSnippet& s, float scaleX, float scaleY) {
    if (!s.params) return;
    DWORD seh = 0;
    ParamSetF(s.params, "DLSSNR.MVecScaleX", scaleX, &seh);
    ParamSetF(s.params, "DLSSNR.MVecScaleY", scaleY, &seh);
    float x = 0.0f, y = 0.0f;
    ParamGetF(s.params, "DLSSNR.MVecScaleX", &x, &seh);
    ParamGetF(s.params, "DLSSNR.MVecScaleY", &y, &seh);
    Log("[params] MVecScaleX=%.6f MVecScaleY=%.6f (seh=%#x)", x, y, seh);
}

// The six values the model reads at every evaluate, written per pass from that pass's own
// resolved tuning. Never constants: writing constants here is the bug the comment in
// NgxSetResources describes, and the difference is that these are the caller's values.
void NgxSetEvaluateTuning(NgxSnippet& s, const NgxTuning& t) {
    if (s.disabled || !s.params) return;
    DWORD seh = 0;
    ParamSetUI(s.params, "DLSSNR.Style", t.style, &seh);
    ParamSetF(s.params, "DLSSNR.Intensity", t.intensity, &seh);
    ParamSetF(s.params, "DLSSNR.LocalToneStrength", t.localTone, &seh);
    ParamSetF(s.params, "DLSSNR.LocalStructureStrength", t.localStructure, &seh);
    ParamSetF(s.params, "DLSSNR.SkinStructureStrength", t.skinStructure, &seh);
    ParamSetUI(s.params, "DLSSNR.UseAutoMask", t.autoMask, &seh);
    // Was a constant 0 written from NgxSetResources -- the same shape as the UseAutoMask constant
    // that turned out to be forcing the skin mask off regardless of the setting. The key is real
    // (the string is in the DLL) and the parameter probe shows it being read at every evaluate, so
    // the constant was a control with a user on the other end of it and nobody able to reach it.
    ParamSetUI(s.params, "DLSSNR.UICorrection", t.uiCorrection, &seh);
    // Read back and reported when it moves, so "I ticked the box" and "the model was told" are two
    // observable facts rather than one assumption. Only on a change: this runs at every evaluate.
    if (t.uiCorrection != s.loggedUiCorrection) {
        unsigned int back = 0xFFFFFFFFu;
        ParamGetUI(s.params, "DLSSNR.UICorrection", &back, &seh);
        s.loggedUiCorrection = t.uiCorrection;
        Log("[params] UICorrection=%u (read back %u)", t.uiCorrection, back);
    }
}

// There is no sharpness parameter in this model, so this does nothing and says so once.
//
// It used to write "Sharpness" every evaluate, and the comment called it "the one strength
// the model reads at evaluate" -- the value the GUI's per-pass sharpness slider feeds. Two
// independent checks say the key does not exist: no string containing "sharp" appears
// anywhere in nvngx_dlssnr.dll or nvngx.dll, in ASCII or UTF-16, and an instrumented
// session never sees it read. The create flag it was paired with, DoSharpening, went the
// same way -- Feature_Flags is not a key this model has either.
//
// Kept as a function rather than deleted so the one caller stays readable and so this
// explanation sits where someone will look for it. The slider that feeds it is a dead
// control and should be removed or relabelled; that is a GUI change and a protocol
// question, not this file's to make.
void NgxSetSharpness(NgxSnippet& s, float sharpness) {
    if (!s.params) return;
    (void)sharpness;
    static bool said = false;
    if (!said) {
        said = true;
        Log("[params] sharpness is not a parameter of this model; the control has no effect");
    }
}

bool NgxEvaluatePass(NgxSnippet& s, uint32_t pass, VkCommandBuffer recordingCmd) {
    if (s.disabled || !s.ready || pass >= kMaxPasses || !s.features[pass]) return false;
    DWORD seh = 0;
    NVSDK_NGX_Result r =
        CallEvaluateSafely(s.evaluateFeature, recordingCmd, s.features[pass], s.params, &seh);
    if (!NVSDK_NGX_SUCCEED(r)) {
        Log("[ngx] VULKAN_EvaluateFeature -> %#x seh=%#x (disabling)", (uint32_t)r, seh);
        s.disabled = true;
        return false;
    }
    return true;
}

// Teardown order per verified runner: Release -> Shutdown1 -> DestroyParameters
// -> restore IAT -> FreeLibrary.
void NgxTeardown(NgxSnippet& s, VkDevice device) {
    DWORD seh = 0;
    // After a fault inside NGX, do not go back in to tidy up.
    //
    // The sequence below -- ReleaseFeature per pass, Shutdown1, DestroyParameters,
    // FreeLibrary -- is exactly the one another project's two crash dumps indict: a fault
    // during shutdown left the SDK's critical section held, and the next initialisation
    // deadlocked against it. And this function is reached precisely BECAUSE a guarded call
    // faulted, since that is what latches `disabled` and breaks the main loop.
    //
    // We are in a much better position than a process the user is still using: the snippet
    // lives in a helper that owns nothing the game needs, the layer already treats an
    // absent helper as the ordinary case, and the OS unmaps a dying process correctly. So
    // the honest answer is to stop rather than to unwind.
    //
    // The IAT hooks are restored either way, and first: they are in OUR address space, and
    // leaving SpoofedGetModuleFileNameW installed over a dead original is worse than
    // leaving a DLL loaded.
    if (NgxFaulted()) {
        Log("[ngx] teardown skipped: NGX faulted in this process. Releasing nothing and "
            "unloading nothing; restoring our own hooks only.");
        RemoveCallerSpoof(g_snippetSpoof);
        RemoveCallerSpoof(g_coreSpoof);
        s.ready = false;
        return;
    }
    const char* fullEnv = getenv("DLSSNR_NGX_FULL_TEARDOWN");
    const bool full = !fullEnv || fullEnv[0] != '0';
    if (!full) {
        Log("[ngx] teardown skipped by DLSSNR_NGX_FULL_TEARDOWN=0");
        RemoveCallerSpoof(g_snippetSpoof);
        RemoveCallerSpoof(g_coreSpoof);
        s.ready = false;
        return;
    }
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
        if (!s.features[i] || !s.releaseFeature) continue;
        NVSDK_NGX_Result r = CallReleaseSafely(s.releaseFeature, s.features[i], &seh);
        Log("[ngx] ReleaseFeature pass %u -> %#x seh=%#x", i, (uint32_t)r, seh);
        s.features[i] = nullptr;
    }
    s.featureCount = 0;
    if (s.shutdown1) {
        NVSDK_NGX_Result r = CallShutdownSafely(s.shutdown1, device, &seh);
        Log("[ngx] snippet Shutdown1 -> %#x seh=%#x", (uint32_t)r, seh);
    }
    if (s.params) {
        if (s.ownParams) {
            // What the DLL never asked for. "Set" looks like "in effect" and is not, and
            // this is the only place in the tree that can tell the difference.
            static_cast<OwnParam*>(s.params)->DumpUnread();
            delete static_cast<OwnParam*>(s.params);
        }
        else if (s.paramsDestroy) {
            NVSDK_NGX_Result r = Guarded([&] { return s.paramsDestroy(s.params); },
                                         NVSDK_NGX_Result_FAIL_SEH, &seh);
            Log("[ngx] DestroyParameters -> %#x seh=%#x", (uint32_t)r, seh);
        }
        s.params = nullptr;
        s.paramsDestroy = nullptr;
    }
    RemoveCallerSpoof(g_snippetSpoof);
    RemoveCallerSpoof(g_coreSpoof);
    if (s.core) { FreeLibrary(s.core); s.core = nullptr; }
    if (s.snippet) { FreeLibrary(s.snippet); s.snippet = nullptr; }
    s.ready = false;
}

}  // namespace dlssnr