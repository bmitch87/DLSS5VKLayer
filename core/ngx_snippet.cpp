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
static bool ParamSetULL(NVSDK_NGX_Parameter* p, const char* n, unsigned long long v, DWORD* seh) {
    Guarded([&] { p->Set(n, v); return true; }, false, seh);
    return *seh == 0;
}
static bool ParamSetF(NVSDK_NGX_Parameter* p, const char* n, float v, DWORD* seh) {
    Guarded([&] { p->Set(n, v); return true; }, false, seh);
    return *seh == 0;
}
static bool ParamGetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int* v, DWORD* seh) {
    return Guarded([&] { return NVSDK_NGX_SUCCEED(p->Get(n, v)); }, false, seh);
}
static bool ParamGetF(NVSDK_NGX_Parameter* p, const char* n, float* v, DWORD* seh) {
    return Guarded([&] { return NVSDK_NGX_SUCCEED(p->Get(n, v)); }, false, seh);
}

static NVSDK_NGX_Result CallInitExtSafely(FnVkInitExt fn, unsigned long long appId,
    const wchar_t* path, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
    NVSDK_NGX_Version version, DWORD* seh) noexcept {
    return Guarded([&] { return fn(appId, path, instance, pd, device, version, nullptr); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallCreateSafely(FnVkCreateFeature fn, VkCommandBuffer cmd, int feature,
    NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle, DWORD* seh) noexcept {
    return Guarded([&] { return fn(cmd, feature, params, handle); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallEvaluateSafely(FnVkEvaluateFeature fn, VkCommandBuffer cmd,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params, DWORD* seh) noexcept {
    return Guarded([&] { return fn(cmd, handle, params, nullptr); },
                   NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallReleaseSafely(FnVkReleaseFeature fn, NVSDK_NGX_Handle* handle, DWORD* seh) noexcept {
    return Guarded([&] { return fn(handle); }, NVSDK_NGX_Result_FAIL_SEH, seh);
}
static NVSDK_NGX_Result CallShutdownSafely(FnVkShutdown1 fn, VkDevice device, DWORD* seh) noexcept {
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
bool NgxLoadAndInit(NgxSnippet& s, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                    uint32_t width, uint32_t height, VkCommandBuffer recordingCmd,
                    const NgxTuning& tuning) {
    if (s.disabled) return false;
    s.binDir = ResolveBinDir();
    if (s.binDir.empty()) { Log("[ngx] nvngx_dlssnr.dll not found (set DLSSNR_BIN_DIR)"); s.disabled = true; return false; }
    Log("[ngx] bin dir: %ls", s.binDir.c_str());

    s.snippet = LoadLibraryExW((s.binDir + L"\\nvngx_dlssnr.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!s.snippet) { Log("[ngx] LoadLibrary nvngx_dlssnr.dll failed (%lu)", GetLastError()); s.disabled = true; return false; }
    Log("[ngx] nvngx_dlssnr.dll loaded at %p", (void*)s.snippet);
    RegisterPeRange("nvngx_dlssnr.dll", s.snippet);

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
        if (s.core) {
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
        if (!s.params && s.snippet) {
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
        }
    }
    {
        DWORD seh2 = 0;
        bool ok = ParamSetUI(s.params, "__selftest", 0xC0FFEE, &seh2);
        unsigned int back = 0;
        ok = ok && ParamGetUI(s.params, "__selftest", &back, &seh2) && back == 0xC0FFEE;
        Log("[params] round-trip self-test: %s (seh=%#x)", ok ? "PASS" : "FAIL", seh2);
        if (!ok) { s.disabled = true; return false; }
    }

    // Create parameters (extracted_pipeline_notes.md section 4).
    DWORD seh = 0;
    bool ps = true;
    ps &= ParamSetUI(s.params, "DLSSNR.Width", width, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Height", height, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.InputWidth", width, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.InputHeight", height, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.OutputWidth", width, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.OutputHeight", height, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Output.Width", width, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Output.Height", height, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Upscaling", 0u, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.Scale", 1.0f, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.ScalingRatio", 1.0f, &seh);
    ps &= ParamSetULL(s.params, "DLSSNRComputeScalingRatioCallback",
                      (unsigned long long)(void*)&ScalingRatioCallback, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Hint.Render.Preset", 0u, &seh);
    ps &= ParamSetUI(s.params, "Width", width, &seh);
    ps &= ParamSetUI(s.params, "Height", height, &seh);
    ps &= ParamSetUI(s.params, "PerfQualityValue", 3u, &seh);  // Balanced
    ps &= ParamSetUI(s.params, "CreationNodeMask", 1u, &seh);
    ps &= ParamSetUI(s.params, "VisibilityNodeMask", 1u, &seh);
    ps &= ParamSetUI(s.params, "NVSDK_NGX_Parameter_PerfQualityValue", 3u, &seh);
    ps &= ParamSetUI(s.params, "NVSDK_NGX_Parameter_CreationNodeMask", 1u, &seh);
    ps &= ParamSetUI(s.params, "NVSDK_NGX_Parameter_VisibilityNodeMask", 1u, &seh);

    // Create flags: sharpening is applied by the net when the runtime float is
    // nonzero (see NgxSetSharpness); auto-exposure keeps adaptation state in the
    // DLL so it survives normal dynamic lighting without host-side resets.
    unsigned int createFlags = NVSDK_NGX_DLSS_Feature_Flags_DoSharpening |
                               NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const char* hdrEnv = getenv("DLSSNR_HDR");
    const bool wantHdr = s.hdrActive || (hdrEnv && hdrEnv[0] == '1');
    if (wantHdr) createFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    ps &= ParamSetUI(s.params, "Feature_Flags", createFlags, &seh);
    ps &= ParamSetUI(s.params, "NVSDK_NGX_Parameter_Feature_Flags", createFlags, &seh);

    // Non-destructive exposure: identity pre-exposure/exposure-scale (never
    // re-pinned per frame) + SDR tonemapped hint unless the HDR path is asked
    // for and the snippet's feature flags advertise HDR.
    ps &= ParamSetF(s.params, "InPreExposure", 1.0f, &seh);
    ps &= ParamSetF(s.params, "InExposureScale", 1.0f, &seh);
    ps &= ParamSetF(s.params, "NVSDK_NGX_Parameter_PreExposure", 1.0f, &seh);
    ps &= ParamSetF(s.params, "NVSDK_NGX_Parameter_ExposureScale", 1.0f, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.AutoExposure", 1u, &seh);
    Log("[params] create contract set: %s (seh=%#x) flags=%#x hdr=%d",
        ps ? "ok" : "FAILED", seh, createFlags, int(wantHdr));

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

    // Tonemapping hint now that the snippet's feature flags are known: SDR by default (the input is
    // LDR RGBA8); HDR only when both asked for and advertised.
    {
        DWORD seh2 = 0;
        const bool hdrPath = wantHdr && s.hdrCapable;
        ParamSetUI(s.params, "DLSSNR.Hdr", hdrPath ? 1u : 0u, &seh2);
        ParamSetUI(s.params, "DLSSNR.SDR", hdrPath ? 0u : 1u, &seh2);
        Log("[params] tonemap hint: %s (featureFlags=%#x)", hdrPath ? "HDR" : "SDR", s.featureFlags);
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

// The HDR contract, rewritten from s.hdrActive before every create. The create flags and the
// tonemap hint are ordinary string-keyed parameters, so restating them here is exactly what the
// init-time block did once -- and doing it at create is what lets a toggle take effect on the next
// feature build rather than never.
static void ApplyHdrContract(NgxSnippet& s) {
    if (!s.params) return;
    DWORD seh = 0;
    unsigned int flags = NVSDK_NGX_DLSS_Feature_Flags_DoSharpening |
                         NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (s.hdrActive) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    ParamSetUI(s.params, "Feature_Flags", flags, &seh);
    ParamSetUI(s.params, "NVSDK_NGX_Parameter_Feature_Flags", flags, &seh);
    ParamSetUI(s.params, "DLSSNR.Hdr", s.hdrActive ? 1u : 0u, &seh);
    ParamSetUI(s.params, "DLSSNR.SDR", s.hdrActive ? 0u : 1u, &seh);
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
                     const NVSDK_NGX_Resource_VK& depth, uint32_t width, uint32_t height) {
    if (!s.params) return;
    s.resColor = color; s.resOut = out; s.resMV = mv; s.resDepth = depth;
    DWORD seh = 0;
    Guarded([&] {
        const bool hasDepth = s.resDepth.Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE;
        s.params->Set("DLSSNR.Color", &s.resColor);
        s.params->Set("DLSSNR.Output", &s.resOut);
        s.params->Set("DLSSNR.MVec", &s.resMV);
        s.params->Set("DLSSNR.Depth", hasDepth ? &s.resDepth : (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.ControlMask", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.UI", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.UIAlpha", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.Backbuffer", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("DLSSNR.BidirectionalDistortionField", (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("Color", &s.resColor);
        s.params->Set("Output", &s.resOut);
        s.params->Set("Depth", hasDepth ? &s.resDepth : (const NVSDK_NGX_Resource_VK*)nullptr);
        s.params->Set("MotionVectors", &s.resMV);
        s.params->Set("MVec", &s.resMV);
        return true;
    }, false, &seh);

    const char* subrectNames[][4] = {
        { "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight" },
        { "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight" },
        { "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight" },
        { "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight" },
    };
    for (auto& n : subrectNames) {
        ParamSetUI(s.params, n[0], 0, &seh);
        ParamSetUI(s.params, n[1], 0, &seh);
        ParamSetUI(s.params, n[2], width, &seh);
        ParamSetUI(s.params, n[3], height, &seh);
    }
    bool ps = true;
    ps &= ParamSetF(s.params, "DLSSNR.Jitter.Offset.X", 0.0f, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.Jitter.Offset.Y", 0.0f, &seh);
    ps &= ParamSetF(s.params, "JitterOffsetX", 0.0f, &seh);
    ps &= ParamSetF(s.params, "JitterOffsetY", 0.0f, &seh);
    ps &= ParamSetUI(s.params, "Reset", 1u, &seh);
    ps &= ParamSetF(s.params, "Sharpness", 0.0f, &seh);
    ps &= ParamSetUI(s.params, "Width", width, &seh);
    ps &= ParamSetUI(s.params, "Height", height, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.MVecScaleX", 1.0f, &seh);
    ps &= ParamSetF(s.params, "DLSSNR.MVecScaleY", 1.0f, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.DepthInverted", 1u, &seh);
    ps &= ParamSetUI(s.params, "DLSS.Indicator.Invert.X.Axis", 0u, &seh);
    ps &= ParamSetUI(s.params, "DLSS.Indicator.Invert.Y.Axis", 0u, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Enabled", 1u, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.Reset", 1u, &seh);
    ps &= ParamSetUI(s.params, "DLSSNR.UICorrection", 0u, &seh);

    // Style, Intensity, LocalTone, LocalStructure, SkinStructure and UseAutoMask are deliberately
    // absent. They used to be written here, every frame, as constants -- which did two harmful
    // things: it had no effect on the running feature, because the model latches them at creation,
    // and it left the parameter block holding those constants for whatever created a feature next.
    // A feature built at any moment other than immediately after NgxSetCreateTuning therefore got
    // defaults no matter what the user had chosen. They belong to NgxTuning and to create time.
    //
    // UseAutoMask was the clearest case: the constant written here was 0, so the automatic skin mask
    // was forced off regardless of the setting, whose default is on.
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
    const bool ok1 = ParamSetUI(s.params, "DLSSNR.Reset", reset ? 1u : 0u, &seh);
    const bool ok2 = ParamSetUI(s.params, "Reset", reset ? 1u : 0u, &seh);
    if (!logValue) return;
    unsigned int back = 0, back2 = 0;
    ParamGetUI(s.params, "DLSSNR.Reset", &back, &seh);
    ParamGetUI(s.params, "Reset", &back2, &seh);
    Log("[params] DLSSNR.Reset(slot11) requested=%u readback=%u alias=%u ok=%d/%d seh=%#x",
        reset ? 1u : 0u, back, back2, int(ok1), int(ok2), seh);
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

void NgxSetSharpness(NgxSnippet& s, float sharpness) {
    if (!s.params) return;
    DWORD seh = 0;
    // The runtime sharpness float has to reach the DLL on every evaluate dispatch: DoSharpening is
    // enabled at create, and this is the per-frame amount it applies.
    ParamSetF(s.params, "Sharpness", sharpness, &seh);
    if (Verbose()) {
        float back = 0.0f;
        ParamGetF(s.params, "Sharpness", &back, &seh);
        Log("[params] Sharpness=%.4f readback=%.4f (seh=%#x)", sharpness, back, seh);
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
        if (s.ownParams) delete static_cast<OwnParam*>(s.params);
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