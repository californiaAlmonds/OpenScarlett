// OpenScarlett Sprint 9 + Sprint 10 Verification Suite:
//   1. kernelx.dll Xbox 7-core / 14-thread CPU affinity & ideal processor remapping
//   2. winrt_xs.dll WinRT app lifecycle (CoreApplication, CoreWindow, CoreDispatcher,
//      ApplicationView, DisplayInformation, GamingDeviceInformation) + IReferenceTracker refusal
//   3. xgameruntime + xsapi + xcurl non-blocking offline auth, XSTS/JWT, XStore & XTaskQueue boot
//
// Strictly portable (no hardcoded E:\ paths; uses std::filesystem::temp_directory_path()).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/kernel/kernelx_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/winrt/winrt_abi.h"
#include "openscarlett/xsapi/xsapi_abi.h"
#include "os_check.h"

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
void SetEnvVar(const char* name, const char* value) {
    ::SetEnvironmentVariableA(name, value);
}
#else
void SetEnvVar(const char* name, const char* value) {
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}
#endif

std::string Base64UrlDecode(const std::string& input) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
        if (c >= '0' && c <= '9') return 52 + (c - '0');
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::string out;
    int buf = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) return {};
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

bool ValidateBase64UrlChars(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// ===========================================================================
// Suite 1: kernelx Xbox 7-Core / 14-Thread CPU Affinity Remapping
// ===========================================================================
void TestKernelXAffinityRemapping() {
    std::printf("[Suite 1] kernelx Xbox 7-core / 14-thread CPU affinity remapping...\n");
#if defined(_WIN32)
    HMODULE h_kx = ::GetModuleHandleW(L"kernelx.dll");
    if (!h_kx) {
        h_kx = ::LoadLibraryW(L"kernelx.dll");
    }
    OS_REQUIRE(h_kx != nullptr);

    using FnRemapMask = uint64_t(__stdcall*)(uint64_t, uint64_t);
    using FnRemapIdeal = uint32_t(__stdcall*)(uint32_t, uint32_t);
    using FnSetAff = uint64_t(__stdcall*)(void*, uint64_t);
    using FnSetIdeal = uint32_t(__stdcall*)(void*, uint32_t);
    using FnSetIdealEx = int32_t(__stdcall*)(void*, const PROCESSOR_NUMBER*, PROCESSOR_NUMBER*);
    using FnSetGroupAff = int32_t(__stdcall*)(void*, const GROUP_AFFINITY*, GROUP_AFFINITY*);
    using FnGetProcAff = int32_t(__stdcall*)(void*, uint64_t*, uint64_t*);
    using FnSetProcAff = int32_t(__stdcall*)(void*, uint64_t);
    using FnGetSysInfo = void(__stdcall*)(SYSTEM_INFO*);
    using FnGetCurProc = uint32_t(__stdcall*)(void);

    auto p_remap_mask = reinterpret_cast<FnRemapMask>(::GetProcAddress(h_kx, "OpenScarlettRemapXboxAffinityMask"));
    auto p_remap_ideal = reinterpret_cast<FnRemapIdeal>(::GetProcAddress(h_kx, "OpenScarlettRemapXboxIdealProcessor"));
    auto p_set_aff = reinterpret_cast<FnSetAff>(::GetProcAddress(h_kx, "KernelXSetThreadAffinityMask"));
    auto p_set_ideal = reinterpret_cast<FnSetIdeal>(::GetProcAddress(h_kx, "KernelXSetThreadIdealProcessor"));
    auto p_set_ideal_ex = reinterpret_cast<FnSetIdealEx>(::GetProcAddress(h_kx, "KernelXSetThreadIdealProcessorEx"));
    auto p_set_group = reinterpret_cast<FnSetGroupAff>(::GetProcAddress(h_kx, "KernelXSetThreadGroupAffinity"));
    auto p_get_proc_aff = reinterpret_cast<FnGetProcAff>(::GetProcAddress(h_kx, "KernelXGetProcessAffinityMask"));
    auto p_set_proc_aff = reinterpret_cast<FnSetProcAff>(::GetProcAddress(h_kx, "KernelXSetProcessAffinityMask"));
    auto p_get_sys_info = reinterpret_cast<FnGetSysInfo>(::GetProcAddress(h_kx, "KernelXGetSystemInfo"));
    auto p_get_cur_proc = reinterpret_cast<FnGetCurProc>(::GetProcAddress(h_kx, "KernelXGetCurrentProcessorNumber"));

    OS_REQUIRE(p_remap_mask != nullptr);
    OS_REQUIRE(p_remap_ideal != nullptr);
    OS_REQUIRE(p_set_aff != nullptr);
    OS_REQUIRE(p_set_ideal != nullptr);
    OS_REQUIRE(p_set_ideal_ex != nullptr);
    OS_REQUIRE(p_set_group != nullptr);
    OS_REQUIRE(p_get_proc_aff != nullptr);
    OS_REQUIRE(p_set_proc_aff != nullptr);
    OS_REQUIRE(p_get_sys_info != nullptr);
    OS_REQUIRE(p_get_cur_proc != nullptr);

    // 1A. Pure deterministic remapping across 4-core, 6-core, 8-core, and 16-core hosts
    struct HostCase {
        uint32_t cores;
        uint64_t mask;
    };
    const HostCase cases[] = {
        {4u, 0x000Full},
        {6u, 0x003Full},
        {8u, 0x00FFull},
        {16u, 0xFFFFull},
    };
    for (const auto& hc : cases) {
        const uint64_t all_mapped = p_remap_mask(0x3FFFull, hc.mask);
        const uint64_t expected_all = (hc.cores >= 14u) ? 0x3FFFull : hc.mask;
        OS_REQUIRE(all_mapped == expected_all);
        OS_REQUIRE((all_mapped & ~hc.mask) == 0ull);

        for (uint32_t b = 0u; b < 14u; ++b) {
            const uint64_t single = p_remap_mask(1ull << b, hc.mask);
            const uint64_t expected = 1ull << (b % hc.cores);
            OS_REQUIRE(single == expected);
            OS_REQUIRE(p_remap_ideal(b, hc.cores) == (b % hc.cores));
        }
    }

    // Distinct core preservation on 8-core host: Xbox core 0 + core 12 -> host core 0 + core 4
    OS_REQUIRE(p_remap_mask((1ull << 0u) | (1ull << 12u), 0xFFull) == ((1ull << 0u) | (1ull << 4u)));
    // Non-contiguous host mask (bits 1, 3, 5, 7)
    for (uint32_t b = 0u; b < 14u; ++b) {
        const uint64_t m = p_remap_mask(1ull << b, 0xAAull);
        OS_REQUIRE(m != 0ull);
        OS_REQUIRE((m & ~0xAAull) == 0ull);
    }
    // Zero guardrails
    OS_REQUIRE(p_remap_mask(0ull, 0xFFull) == 0ull);
    OS_REQUIRE(p_remap_mask(0x3FFFull, 0ull) == 0ull);
    OS_REQUIRE(p_remap_ideal(5u, 0u) == 0u);

    // 1B. Live thread & system calls on current thread
    HANDLE cur_thread = ::GetCurrentThread();
    HANDLE cur_proc = ::GetCurrentProcess();

    const uint64_t orig_aff = p_set_aff(cur_thread, 0x3FFFull);
    OS_REQUIRE(orig_aff != 0ull);
    for (uint32_t b = 0u; b < 14u; ++b) {
        const uint64_t prev = p_set_aff(cur_thread, 1ull << b);
        OS_REQUIRE(prev != 0ull);
    }
    OS_REQUIRE(p_set_aff(cur_thread, orig_aff) != 0ull);

    // Live SetThreadIdealProcessor across Xbox logical cores 0..13
    const uint32_t orig_ideal = p_set_ideal(cur_thread, 0u);
    OS_REQUIRE(orig_ideal != static_cast<uint32_t>(-1));
    for (uint32_t b = 0u; b < 14u; ++b) {
        const uint32_t prev = p_set_ideal(cur_thread, b);
        OS_REQUIRE(prev != static_cast<uint32_t>(-1));
    }
    const uint32_t queried_ideal = p_set_ideal(cur_thread, MAXIMUM_PROCESSORS);
    OS_REQUIRE(queried_ideal == 13u);

    // Live SetThreadIdealProcessorEx & SetThreadGroupAffinity
    PROCESSOR_NUMBER req_pn{};
    req_pn.Group = 0;
    req_pn.Number = 11;
    PROCESSOR_NUMBER prev_pn{};
    OS_REQUIRE(p_set_ideal_ex(cur_thread, &req_pn, &prev_pn) == 1);
    OS_REQUIRE(prev_pn.Number == 13);
    OS_REQUIRE(p_set_ideal(cur_thread, orig_ideal) != static_cast<uint32_t>(-1));

    GROUP_AFFINITY req_ga{};
    req_ga.Mask = static_cast<KAFFINITY>(1ull << 13u);
    req_ga.Group = 0;
    GROUP_AFFINITY prev_ga{};
    OS_REQUIRE(p_set_group(cur_thread, &req_ga, &prev_ga) == 1);
    OS_REQUIRE(prev_ga.Mask != 0u);
    OS_REQUIRE(p_set_aff(cur_thread, 0x3FFFull) != 0ull);

    // GetSystemInfo / GetProcessAffinityMask / GetCurrentProcessorNumber
    SYSTEM_INFO si{};
    p_get_sys_info(&si);
    OS_REQUIRE(si.dwNumberOfProcessors == 14u);
    OS_REQUIRE(si.dwActiveProcessorMask == 0x3FFFu);
    OS_REQUIRE(p_get_cur_proc() < 14u);

    uint64_t pm = 0, sm = 0;
    OS_REQUIRE(p_get_proc_aff(cur_proc, &pm, &sm) == 1);
    OS_REQUIRE(sm == 0x3FFFull);
    OS_REQUIRE(p_set_proc_aff(cur_proc, 0x00FFull) == 1);
    OS_REQUIRE(p_get_proc_aff(cur_proc, &pm, &sm) == 1);
    OS_REQUIRE(pm == 0x00FFull);
    OS_REQUIRE(p_set_proc_aff(cur_proc, 0x3FFFull) == 1);

    // Verify unprefixed Win32 alias exports exist on MSVC
#if defined(_MSC_VER)
    OS_REQUIRE(::GetProcAddress(h_kx, "SetThreadAffinityMask") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_kx, "SetThreadIdealProcessor") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_kx, "SetThreadIdealProcessorEx") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_kx, "SetThreadGroupAffinity") != nullptr);
    OS_REQUIRE(::GetProcAddress(h_kx, "GetSystemInfo") != nullptr);
#endif
#endif
    std::printf("  PASS: kernelx Xbox 7-core / 14-thread CPU affinity remapping\n");
}

// ===========================================================================
// Suite 2: winrt_xs Activation, Lifecycle Events & IReferenceTracker Refusal
// ===========================================================================
constexpr SX_GUID kIidIReferenceTracker = {
    0x11D3B13Au, 0x180Eu, 0x4789u, {0xA8, 0xBE, 0x77, 0x12, 0x88, 0x28, 0x93, 0xE6}};
constexpr SX_GUID kIidIReferenceTrackerTarget = {
    0x64BD43F8u, 0xBFEEu, 0x4EC4u, {0xB7, 0xEB, 0x29, 0x35, 0x15, 0x8D, 0xAE, 0x21}};
constexpr SX_GUID kIidICoreApplication = {
    0x0AACF7A4u, 0x5E1Du, 0x49DFu, {0x80, 0x34, 0xFB, 0x6A, 0x68, 0xBC, 0x5E, 0xD1}};
constexpr SX_GUID kIidICoreApplication2 = {
    0x998681FBu, 0x1AB6u, 0x4B7Fu, {0xBE, 0x4A, 0x9A, 0x06, 0x45, 0x22, 0x4C, 0x04}};
constexpr SX_GUID kIidICoreApplicationView = {
    0x638BB2DBu, 0x451Du, 0x4661u, {0xB0, 0x99, 0x41, 0x4F, 0x34, 0xFF, 0xB9, 0xF1}};
constexpr SX_GUID kIidICoreWindowStatic = {
    0x4D239005u, 0x3C2Au, 0x41B1u, {0x90, 0x22, 0x53, 0x6B, 0xB9, 0xCF, 0x93, 0xB1}};
constexpr SX_GUID kIidICoreWindow = {
    0x79B9D5F2u, 0x879Eu, 0x4B89u, {0xB7, 0x98, 0x79, 0xE4, 0x75, 0x98, 0x03, 0x0C}};
constexpr SX_GUID kIidICoreDispatcher = {
    0x60DB2FA8u, 0xB705u, 0x4FDEu, {0xA7, 0xD6, 0xEB, 0xBB, 0x18, 0x91, 0xD3, 0x9E}};
constexpr SX_GUID kIidIApplicationViewStatics2 = {
    0xAF338AE5u, 0xCF64u, 0x423Cu, {0x85, 0xE5, 0xF3, 0xE7, 0x24, 0x48, 0xFB, 0x23}};
constexpr SX_GUID kIidIApplicationView = {
    0xD222D519u, 0x4361u, 0x451Eu, {0x96, 0xC4, 0x60, 0xF4, 0xF9, 0x74, 0x2D, 0xB0}};
constexpr SX_GUID kIidIApplicationView2 = {
    0xE876B196u, 0xA545u, 0x40DCu, {0xB5, 0x94, 0x45, 0x0C, 0xBA, 0x68, 0xCC, 0x00}};
constexpr SX_GUID kIidIApplicationView3 = {
    0x903C9CE5u, 0x793Au, 0x4FDFu, {0xA2, 0xB2, 0xAF, 0x1A, 0xC2, 0x1E, 0x31, 0x08}};
constexpr SX_GUID kIidIDisplayInformationStatics = {
    0xC6A02A6Cu, 0xD452u, 0x44DCu, {0xBA, 0x07, 0x96, 0xF3, 0xC6, 0xAD, 0xF9, 0xD1}};
constexpr SX_GUID kIidIDisplayInformation = {
    0xBED112AEu, 0xADC3u, 0x4DC9u, {0xAE, 0x65, 0x85, 0x1F, 0x4D, 0x7D, 0x47, 0x99}};
constexpr SX_GUID kIidIDisplayInformation2 = {
    0x4DCD0021u, 0xFAD1u, 0x4B8Eu, {0x8E, 0xDF, 0x77, 0x58, 0x87, 0xB8, 0xBF, 0x19}};
constexpr SX_GUID kIidIGamingDeviceInformationStatics = {
    0x3F0D9C56u, 0x0A0Bu, 0x4B99u, {0x90, 0x72, 0x7C, 0x8F, 0x90, 0x4A, 0x7A, 0x31}};

struct TestEventToken { int64_t value; };
struct TestRect { float X, Y, Width, Height; };
struct TestPoint { float X, Y; };
struct TestSize { float Width, Height; };
struct TestDateTime { int64_t UniversalTime; };
struct TestGamingDeviceModelInfo { uint32_t vendorId; uint32_t deviceId; };

// Minimal test COM vtables matching WinRT ABI
struct T_IDeferral;
struct T_IDeferralVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IDeferral*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IDeferral*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IDeferral*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IDeferral*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IDeferral*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IDeferral*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* Complete)(T_IDeferral*);
};
struct T_IDeferral { const T_IDeferralVtbl* lpVtbl; };

struct T_ISuspendingOp;
struct T_ISuspendingOpVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ISuspendingOp*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ISuspendingOp*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ISuspendingOp*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ISuspendingOp*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ISuspendingOp*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ISuspendingOp*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetDeferral)(T_ISuspendingOp*, T_IDeferral**);
    int32_t(SCARLETTX_WINRT_CALL* get_Deadline)(T_ISuspendingOp*, TestDateTime*);
};
struct T_ISuspendingOp { const T_ISuspendingOpVtbl* lpVtbl; };

struct T_ISuspendingArgs;
struct T_ISuspendingArgsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ISuspendingArgs*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ISuspendingArgs*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ISuspendingArgs*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ISuspendingArgs*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ISuspendingArgs*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ISuspendingArgs*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_SuspendingOperation)(T_ISuspendingArgs*, T_ISuspendingOp**);
};
struct T_ISuspendingArgs { const T_ISuspendingArgsVtbl* lpVtbl; };

struct T_IBgArgs;
struct T_IBgArgsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IBgArgs*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IBgArgs*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IBgArgs*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IBgArgs*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IBgArgs*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IBgArgs*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetDeferral)(T_IBgArgs*, T_IDeferral**);
};
struct T_IBgArgs { const T_IBgArgsVtbl* lpVtbl; };

struct T_IEventHandler;
struct T_IEventHandlerVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IEventHandler*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IEventHandler*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IEventHandler*);
    int32_t(SCARLETTX_WINRT_CALL* Invoke)(T_IEventHandler*, void* sender, void* args);
};
struct T_IEventHandler { const T_IEventHandlerVtbl* lpVtbl; };

struct TestEventHandlerObj {
    const T_IEventHandlerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<int32_t> call_count{0};
    bool expect_suspending_deferral = false;
    bool expect_bg_deferral = false;
    bool deferral_completed = false;
};

int32_t SCARLETTX_WINRT_CALL Teh_QI(T_IEventHandler* self, const SX_GUID* riid, void** ppv) {
    if (!ppv) return SX_WINRT_E_POINTER;
    *ppv = nullptr;
    if (riid && std::memcmp(riid, &SX_IID_IUnknown, sizeof(SX_GUID)) == 0) {
        reinterpret_cast<TestEventHandlerObj*>(self)->ref_count.fetch_add(1);
        *ppv = self;
        return SX_WINRT_S_OK;
    }
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Teh_AddRef(T_IEventHandler* self) {
    return reinterpret_cast<TestEventHandlerObj*>(self)->ref_count.fetch_add(1) + 1;
}
uint32_t SCARLETTX_WINRT_CALL Teh_Release(T_IEventHandler* self) {
    auto* obj = reinterpret_cast<TestEventHandlerObj*>(self);
    return obj->ref_count.fetch_sub(1) - 1;
}
int32_t SCARLETTX_WINRT_CALL Teh_Invoke(T_IEventHandler* self, void*, void* args) {
    auto* obj = reinterpret_cast<TestEventHandlerObj*>(self);
    obj->call_count.fetch_add(1);
    if (obj->expect_suspending_deferral && args) {
        auto* sargs = reinterpret_cast<T_ISuspendingArgs*>(args);
        T_ISuspendingOp* op = nullptr;
        if (sargs->lpVtbl->get_SuspendingOperation(sargs, &op) == SX_WINRT_S_OK && op) {
            T_IDeferral* def = nullptr;
            if (op->lpVtbl->GetDeferral(op, &def) == SX_WINRT_S_OK && def) {
                if (def->lpVtbl->Complete(def) == SX_WINRT_S_OK) {
                    obj->deferral_completed = true;
                }
                def->lpVtbl->Release(def);
            }
            op->lpVtbl->Release(op);
        }
    }
    if (obj->expect_bg_deferral && args) {
        auto* bargs = reinterpret_cast<T_IBgArgs*>(args);
        T_IDeferral* def = nullptr;
        if (bargs->lpVtbl->GetDeferral(bargs, &def) == SX_WINRT_S_OK && def) {
            if (def->lpVtbl->Complete(def) == SX_WINRT_S_OK) {
                obj->deferral_completed = true;
            }
            def->lpVtbl->Release(def);
        }
    }
    return SX_WINRT_S_OK;
}
const T_IEventHandlerVtbl kTestEventHandlerVtbl = {
    &Teh_QI, &Teh_AddRef, &Teh_Release, &Teh_Invoke,
};

struct T_IDispatchedHandler;
struct T_IDispatchedHandlerVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IDispatchedHandler*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IDispatchedHandler*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IDispatchedHandler*);
    int32_t(SCARLETTX_WINRT_CALL* Invoke)(T_IDispatchedHandler*);
};
struct T_IDispatchedHandler { const T_IDispatchedHandlerVtbl* lpVtbl; };
struct TestDispatchedObj {
    const T_IDispatchedHandlerVtbl* lpVtbl;
    int32_t invoked = 0;
};
int32_t SCARLETTX_WINRT_CALL Tdh_QI(T_IDispatchedHandler*, const SX_GUID*, void** ppv) {
    if (ppv) *ppv = nullptr;
    return SX_WINRT_E_NOINTERFACE;
}
uint32_t SCARLETTX_WINRT_CALL Tdh_AddRef(T_IDispatchedHandler*) { return 1; }
uint32_t SCARLETTX_WINRT_CALL Tdh_Release(T_IDispatchedHandler*) { return 1; }
int32_t SCARLETTX_WINRT_CALL Tdh_Invoke(T_IDispatchedHandler* self) {
    reinterpret_cast<TestDispatchedObj*>(self)->invoked++;
    return SX_WINRT_S_OK;
}
const T_IDispatchedHandlerVtbl kTestDispatchedVtbl = {
    &Tdh_QI, &Tdh_AddRef, &Tdh_Release, &Tdh_Invoke,
};

struct T_ICoreDispatcher;
struct T_ICoreDispatcherVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreDispatcher*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreDispatcher*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreDispatcher*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreDispatcher*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreDispatcher*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreDispatcher*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_HasThreadAccess)(T_ICoreDispatcher*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* ProcessEvents)(T_ICoreDispatcher*, uint32_t);
    int32_t(SCARLETTX_WINRT_CALL* RunAsync)(T_ICoreDispatcher*, int32_t, T_IDispatchedHandler*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* RunIdleAsync)(T_ICoreDispatcher*, T_IDispatchedHandler*, SX_IInspectable**);
};
struct T_ICoreDispatcher { const T_ICoreDispatcherVtbl* lpVtbl; };

struct T_ICoreWindow;
struct T_ICoreWindowVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreWindow*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreWindow*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreWindow*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreWindow*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreWindow*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_AutomationHostProvider)(T_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* get_Bounds)(T_ICoreWindow*, TestRect*);
    int32_t(SCARLETTX_WINRT_CALL* get_CustomProperties)(T_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* get_Dispatcher)(T_ICoreWindow*, T_ICoreDispatcher**);
    int32_t(SCARLETTX_WINRT_CALL* get_FlowDirection)(T_ICoreWindow*, int32_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_FlowDirection)(T_ICoreWindow*, int32_t);
    int32_t(SCARLETTX_WINRT_CALL* get_IsInputEnabled)(T_ICoreWindow*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* put_IsInputEnabled)(T_ICoreWindow*, uint8_t);
    int32_t(SCARLETTX_WINRT_CALL* get_PointerCursor)(T_ICoreWindow*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* put_PointerCursor)(T_ICoreWindow*, SX_IInspectable*);
    int32_t(SCARLETTX_WINRT_CALL* get_PointerPosition)(T_ICoreWindow*, TestPoint*);
    int32_t(SCARLETTX_WINRT_CALL* get_Visible)(T_ICoreWindow*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* Activate)(T_ICoreWindow*);
    int32_t(SCARLETTX_WINRT_CALL* Close)(T_ICoreWindow*);
    void* pad_keys_capture[4];
    int32_t(SCARLETTX_WINRT_CALL* add_Activated)(T_ICoreWindow*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Activated)(T_ICoreWindow*, TestEventToken);
    void* pad_events[30];
    int32_t(SCARLETTX_WINRT_CALL* add_VisibilityChanged)(T_ICoreWindow*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_VisibilityChanged)(T_ICoreWindow*, TestEventToken);
};
struct T_ICoreWindow { const T_ICoreWindowVtbl* lpVtbl; };

struct T_ICoreWindowStatic;
struct T_ICoreWindowStaticVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreWindowStatic*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreWindowStatic*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreWindowStatic*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreWindowStatic*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreWindowStatic*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreWindowStatic*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentThread)(T_ICoreWindowStatic*, T_ICoreWindow**);
};
struct T_ICoreWindowStatic { const T_ICoreWindowStaticVtbl* lpVtbl; };

struct T_ICoreAppView;
struct T_ICoreAppViewVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreAppView*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreAppView*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreAppView*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreAppView*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreAppView*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreAppView*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_CoreWindow)(T_ICoreAppView*, T_ICoreWindow**);
    int32_t(SCARLETTX_WINRT_CALL* add_Activated)(T_ICoreAppView*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Activated)(T_ICoreAppView*, TestEventToken);
    int32_t(SCARLETTX_WINRT_CALL* get_IsMain)(T_ICoreAppView*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_IsHosted)(T_ICoreAppView*, uint8_t*);
};
struct T_ICoreAppView { const T_ICoreAppViewVtbl* lpVtbl; };

struct T_ICoreApplication;
struct T_ICoreApplicationVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreApplication*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreApplication*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreApplication*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreApplication*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreApplication*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreApplication*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_Id)(T_ICoreApplication*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* add_Suspending)(T_ICoreApplication*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Suspending)(T_ICoreApplication*, TestEventToken);
    int32_t(SCARLETTX_WINRT_CALL* add_Resuming)(T_ICoreApplication*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_Resuming)(T_ICoreApplication*, TestEventToken);
    int32_t(SCARLETTX_WINRT_CALL* get_Properties)(T_ICoreApplication*, SX_IInspectable**);
    int32_t(SCARLETTX_WINRT_CALL* GetCurrentView)(T_ICoreApplication*, T_ICoreAppView**);
};
struct T_ICoreApplication { const T_ICoreApplicationVtbl* lpVtbl; };

struct T_ICoreApplication2;
struct T_ICoreApplication2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_ICoreApplication2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_ICoreApplication2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_ICoreApplication2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_ICoreApplication2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_ICoreApplication2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_ICoreApplication2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* add_BackgroundActivated)(T_ICoreApplication2*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_BackgroundActivated)(T_ICoreApplication2*, TestEventToken);
    int32_t(SCARLETTX_WINRT_CALL* add_LeavingBackground)(T_ICoreApplication2*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_LeavingBackground)(T_ICoreApplication2*, TestEventToken);
    int32_t(SCARLETTX_WINRT_CALL* add_EnteredBackground)(T_ICoreApplication2*, T_IEventHandler*, TestEventToken*);
    int32_t(SCARLETTX_WINRT_CALL* remove_EnteredBackground)(T_ICoreApplication2*, TestEventToken);
};
struct T_ICoreApplication2 { const T_ICoreApplication2Vtbl* lpVtbl; };

struct T_IApplicationView;
struct T_IApplicationViewVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IApplicationView*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IApplicationView*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IApplicationView*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IApplicationView*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IApplicationView*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IApplicationView*, SX_TrustLevel*);
    void* pad_props[7];
    int32_t(SCARLETTX_WINRT_CALL* put_Title)(T_IApplicationView*, SX_HSTRING);
    int32_t(SCARLETTX_WINRT_CALL* get_Title)(T_IApplicationView*, SX_HSTRING*);
};
struct T_IApplicationView { const T_IApplicationViewVtbl* lpVtbl; };

struct T_IApplicationView2;
struct T_IApplicationView2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IApplicationView2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IApplicationView2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IApplicationView2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IApplicationView2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IApplicationView2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IApplicationView2*, SX_TrustLevel*);
    void* pad[2];
    int32_t(SCARLETTX_WINRT_CALL* get_VisibleBounds)(T_IApplicationView2*, TestRect*);
};
struct T_IApplicationView2 { const T_IApplicationView2Vtbl* lpVtbl; };

struct T_IApplicationView3;
struct T_IApplicationView3Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IApplicationView3*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IApplicationView3*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IApplicationView3*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IApplicationView3*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IApplicationView3*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IApplicationView3*, SX_TrustLevel*);
    void* pad[3];
    int32_t(SCARLETTX_WINRT_CALL* get_IsFullScreenMode)(T_IApplicationView3*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* TryEnterFullScreenMode)(T_IApplicationView3*, uint8_t*);
    int32_t(SCARLETTX_WINRT_CALL* ExitFullScreenMode)(T_IApplicationView3*);
};
struct T_IApplicationView3 { const T_IApplicationView3Vtbl* lpVtbl; };

struct T_IApplicationViewStatics2;
struct T_IApplicationViewStatics2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IApplicationViewStatics2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IApplicationViewStatics2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IApplicationViewStatics2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IApplicationViewStatics2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IApplicationViewStatics2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IApplicationViewStatics2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentView)(T_IApplicationViewStatics2*, T_IApplicationView**);
};
struct T_IApplicationViewStatics2 { const T_IApplicationViewStatics2Vtbl* lpVtbl; };

struct T_IDisplayInformation;
struct T_IDisplayInformationVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IDisplayInformation*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IDisplayInformation*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IDisplayInformation*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IDisplayInformation*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IDisplayInformation*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IDisplayInformation*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_CurrentOrientation)(T_IDisplayInformation*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_NativeOrientation)(T_IDisplayInformation*, uint32_t*);
    void* pad_evt[2];
    int32_t(SCARLETTX_WINRT_CALL* get_ResolutionScale)(T_IDisplayInformation*, uint32_t*);
    int32_t(SCARLETTX_WINRT_CALL* get_LogicalDpi)(T_IDisplayInformation*, float*);
};
struct T_IDisplayInformation { const T_IDisplayInformationVtbl* lpVtbl; };

struct T_IDisplayInformation2;
struct T_IDisplayInformation2Vtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IDisplayInformation2*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IDisplayInformation2*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IDisplayInformation2*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IDisplayInformation2*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IDisplayInformation2*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IDisplayInformation2*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_RawPixelsPerViewPixel)(T_IDisplayInformation2*, double*);
};
struct T_IDisplayInformation2 { const T_IDisplayInformation2Vtbl* lpVtbl; };

struct T_IDisplayInformationStatics;
struct T_IDisplayInformationStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IDisplayInformationStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IDisplayInformationStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IDisplayInformationStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IDisplayInformationStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IDisplayInformationStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IDisplayInformationStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* GetForCurrentView)(T_IDisplayInformationStatics*, T_IDisplayInformation**);
};
struct T_IDisplayInformationStatics { const T_IDisplayInformationStaticsVtbl* lpVtbl; };

struct T_IGamingDeviceInfoStatics;
struct T_IGamingDeviceInfoStaticsVtbl {
    int32_t(SCARLETTX_WINRT_CALL* QueryInterface)(T_IGamingDeviceInfoStatics*, const SX_GUID*, void**);
    uint32_t(SCARLETTX_WINRT_CALL* AddRef)(T_IGamingDeviceInfoStatics*);
    uint32_t(SCARLETTX_WINRT_CALL* Release)(T_IGamingDeviceInfoStatics*);
    int32_t(SCARLETTX_WINRT_CALL* GetIids)(T_IGamingDeviceInfoStatics*, uint32_t*, SX_GUID**);
    int32_t(SCARLETTX_WINRT_CALL* GetRuntimeClassName)(T_IGamingDeviceInfoStatics*, SX_HSTRING*);
    int32_t(SCARLETTX_WINRT_CALL* GetTrustLevel)(T_IGamingDeviceInfoStatics*, SX_TrustLevel*);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceModelInformation)(T_IGamingDeviceInfoStatics*, TestGamingDeviceModelInfo*);
    int32_t(SCARLETTX_WINRT_CALL* get_DeviceFamily)(T_IGamingDeviceInfoStatics*, SX_HSTRING*);
};
struct T_IGamingDeviceInfoStatics { const T_IGamingDeviceInfoStaticsVtbl* lpVtbl; };

void AssertRefusesReferenceTracker(SX_IUnknown* unk) {
    OS_REQUIRE(unk != nullptr);
    void* ppv = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEFu));
    OS_REQUIRE(unk->lpVtbl->QueryInterface(unk, &kIidIReferenceTracker, &ppv) == SX_WINRT_E_NOINTERFACE);
    OS_REQUIRE(ppv == nullptr);

    ppv = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEFu));
    OS_REQUIRE(unk->lpVtbl->QueryInterface(unk, &kIidIReferenceTrackerTarget, &ppv) == SX_WINRT_E_NOINTERFACE);
    OS_REQUIRE(ppv == nullptr);
}

SX_HSTRING MakeHString(const std::u16string& s) {
    SX_HSTRING h = nullptr;
    OS_REQUIRE(WindowsCreateString(
                   reinterpret_cast<const uint16_t*>(s.data()),
                   static_cast<uint32_t>(s.size()), &h) == SX_WINRT_S_OK);
    return h;
}

void TestWinRtAppLifecycle() {
    std::printf("[Suite 2] winrt_xs lifecycle, CoreWindow/AppView/DisplayInfo & IReferenceTracker refusal...\n");
    OS_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == SX_WINRT_S_OK);

#if defined(_WIN32)
    HMODULE h_wrt = ::GetModuleHandleW(L"winrt_xs.dll");
    if (!h_wrt) {
        h_wrt = ::LoadLibraryW(L"winrt_xs.dll");
    }
    OS_REQUIRE(h_wrt != nullptr);

    using FnTriggerEvent = int32_t(*)(uint32_t);
    using FnGetConstrained = int32_t(*)(void);
    using FnGetGamingInfo = int32_t(__stdcall*)(TestGamingDeviceModelInfo*);

    auto p_trigger = reinterpret_cast<FnTriggerEvent>(::GetProcAddress(h_wrt, "OpenScarlettTriggerWinRtLifecycleEvent"));
    auto p_get_constrained = reinterpret_cast<FnGetConstrained>(::GetProcAddress(h_wrt, "OpenScarlettGetWinRtConstrainedMode"));
    auto p_get_gaming_info = reinterpret_cast<FnGetGamingInfo>(::GetProcAddress(h_wrt, "GetGamingDeviceModelInformation"));
    OS_REQUIRE(p_trigger != nullptr);
    OS_REQUIRE(p_get_constrained != nullptr);
    OS_REQUIRE(p_get_gaming_info != nullptr);
#endif

    // 2A. CoreApplication + CoreApplication2 + CoreApplicationView
    SX_HSTRING h_ca = MakeHString(u"Windows.ApplicationModel.Core.CoreApplication");
    T_ICoreApplication* core_app = nullptr;
    OS_REQUIRE(RoGetActivationFactory(h_ca, &kIidICoreApplication, reinterpret_cast<void**>(&core_app)) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(core_app));

    T_ICoreApplication2* core_app2 = nullptr;
    OS_REQUIRE(core_app->lpVtbl->QueryInterface(core_app, &kIidICoreApplication2, reinterpret_cast<void**>(&core_app2)) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app2 != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(core_app2));

    T_ICoreAppView* app_view = nullptr;
    OS_REQUIRE(core_app->lpVtbl->GetCurrentView(core_app, &app_view) == SX_WINRT_S_OK);
    OS_REQUIRE(app_view != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(app_view));
    uint8_t is_main = 0, is_hosted = 1;
    OS_REQUIRE(app_view->lpVtbl->get_IsMain(app_view, &is_main) == SX_WINRT_S_OK && is_main == 1u);
    OS_REQUIRE(app_view->lpVtbl->get_IsHosted(app_view, &is_hosted) == SX_WINRT_S_OK && is_hosted == 0u);

    // 2B. CoreWindow + CoreDispatcher
    SX_HSTRING h_cw = MakeHString(u"Windows.UI.Core.CoreWindow");
    T_ICoreWindowStatic* cw_statics = nullptr;
    OS_REQUIRE(RoGetActivationFactory(h_cw, &kIidICoreWindowStatic, reinterpret_cast<void**>(&cw_statics)) == SX_WINRT_S_OK);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(cw_statics));

    T_ICoreWindow* core_win = nullptr;
    OS_REQUIRE(cw_statics->lpVtbl->GetForCurrentThread(cw_statics, &core_win) == SX_WINRT_S_OK && core_win != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(core_win));

    TestRect bounds{};
    OS_REQUIRE(core_win->lpVtbl->get_Bounds(core_win, &bounds) == SX_WINRT_S_OK);
    OS_REQUIRE(bounds.Width >= 1280.0f && bounds.Height >= 720.0f);
    OS_REQUIRE(core_win->lpVtbl->Activate(core_win) == SX_WINRT_S_OK);
    uint8_t visible = 0;
    OS_REQUIRE(core_win->lpVtbl->get_Visible(core_win, &visible) == SX_WINRT_S_OK && visible == 1u);

    T_ICoreDispatcher* disp = nullptr;
    OS_REQUIRE(core_win->lpVtbl->get_Dispatcher(core_win, &disp) == SX_WINRT_S_OK && disp != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(disp));
    uint8_t has_access = 0;
    OS_REQUIRE(disp->lpVtbl->get_HasThreadAccess(disp, &has_access) == SX_WINRT_S_OK && has_access == 1u);
    OS_REQUIRE(disp->lpVtbl->ProcessEvents(disp, 3u) == SX_WINRT_S_OK);
    TestDispatchedObj disp_cb{&kTestDispatchedVtbl, 0};
    SX_IInspectable* run_act = nullptr;
    OS_REQUIRE(disp->lpVtbl->RunAsync(disp, 0, reinterpret_cast<T_IDispatchedHandler*>(&disp_cb), &run_act) == SX_WINRT_S_OK);
    OS_REQUIRE(disp_cb.invoked == 1);
    if (run_act) run_act->lpVtbl->Release(run_act);

    // 2C. Suspend -> Resume -> Constrained Mode -> Unconstrained Mode lifecycle delivery
    TestEventHandlerObj h_susp{&kTestEventHandlerVtbl};
    h_susp.expect_suspending_deferral = true;
    TestEventHandlerObj h_res{&kTestEventHandlerVtbl};
    TestEventHandlerObj h_ebg{&kTestEventHandlerVtbl};
    h_ebg.expect_bg_deferral = true;
    TestEventHandlerObj h_lbg{&kTestEventHandlerVtbl};
    h_lbg.expect_bg_deferral = true;
    TestEventHandlerObj h_vis{&kTestEventHandlerVtbl};

    TestEventToken tok_susp{}, tok_res{}, tok_ebg{}, tok_lbg{}, tok_vis{};
    OS_REQUIRE(core_app->lpVtbl->add_Suspending(core_app, reinterpret_cast<T_IEventHandler*>(&h_susp), &tok_susp) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app->lpVtbl->add_Resuming(core_app, reinterpret_cast<T_IEventHandler*>(&h_res), &tok_res) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app2->lpVtbl->add_EnteredBackground(core_app2, reinterpret_cast<T_IEventHandler*>(&h_ebg), &tok_ebg) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app2->lpVtbl->add_LeavingBackground(core_app2, reinterpret_cast<T_IEventHandler*>(&h_lbg), &tok_lbg) == SX_WINRT_S_OK);
    OS_REQUIRE(core_win->lpVtbl->add_VisibilityChanged(core_win, reinterpret_cast<T_IEventHandler*>(&h_vis), &tok_vis) == SX_WINRT_S_OK);

#if defined(_WIN32)
    OS_REQUIRE(p_trigger(0u) >= 1);  // Suspending
    OS_REQUIRE(h_susp.call_count.load() == 1);
    OS_REQUIRE(h_susp.deferral_completed);

    OS_REQUIRE(p_trigger(1u) >= 1);  // Resuming
    OS_REQUIRE(h_res.call_count.load() == 1);

    OS_REQUIRE(p_trigger(4u) >= 2);  // ConstrainedModeEntered
    OS_REQUIRE(p_get_constrained() == 1);
    OS_REQUIRE(h_ebg.call_count.load() == 1);
    OS_REQUIRE(h_ebg.deferral_completed);
    OS_REQUIRE(h_vis.call_count.load() == 1);
    OS_REQUIRE(core_win->lpVtbl->get_Visible(core_win, &visible) == SX_WINRT_S_OK && visible == 0u);

    OS_REQUIRE(p_trigger(5u) >= 3);  // ConstrainedModeExited
    OS_REQUIRE(p_get_constrained() == 0);
    OS_REQUIRE(h_lbg.call_count.load() == 1);
    OS_REQUIRE(h_lbg.deferral_completed);
    OS_REQUIRE(h_res.call_count.load() == 2);
    OS_REQUIRE(h_vis.call_count.load() == 2);
    OS_REQUIRE(core_win->lpVtbl->get_Visible(core_win, &visible) == SX_WINRT_S_OK && visible == 1u);
#endif

    OS_REQUIRE(core_app->lpVtbl->remove_Suspending(core_app, tok_susp) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app->lpVtbl->remove_Resuming(core_app, tok_res) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app2->lpVtbl->remove_EnteredBackground(core_app2, tok_ebg) == SX_WINRT_S_OK);
    OS_REQUIRE(core_app2->lpVtbl->remove_LeavingBackground(core_app2, tok_lbg) == SX_WINRT_S_OK);
    OS_REQUIRE(core_win->lpVtbl->remove_VisibilityChanged(core_win, tok_vis) == SX_WINRT_S_OK);

    // 2D. ApplicationView (GetForCurrentView, Title, VisibleBounds, TryEnterFullScreenMode)
    SX_HSTRING h_av = MakeHString(u"Windows.UI.ViewManagement.ApplicationView");
    T_IApplicationViewStatics2* av_statics = nullptr;
    OS_REQUIRE(RoGetActivationFactory(h_av, &kIidIApplicationViewStatics2, reinterpret_cast<void**>(&av_statics)) == SX_WINRT_S_OK);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(av_statics));

    T_IApplicationView* av1 = nullptr;
    OS_REQUIRE(av_statics->lpVtbl->GetForCurrentView(av_statics, &av1) == SX_WINRT_S_OK && av1 != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(av1));

    SX_HSTRING new_title = MakeHString(u"Scarlett Engine Test");
    OS_REQUIRE(av1->lpVtbl->put_Title(av1, new_title) == SX_WINRT_S_OK);
    SX_HSTRING got_title = nullptr;
    OS_REQUIRE(av1->lpVtbl->get_Title(av1, &got_title) == SX_WINRT_S_OK);
    int32_t cmp = -1;
    OS_REQUIRE(WindowsCompareStringOrdinal(new_title, got_title, &cmp) == SX_WINRT_S_OK && cmp == 0);
    WindowsDeleteString(new_title);
    WindowsDeleteString(got_title);

    T_IApplicationView2* av2 = nullptr;
    OS_REQUIRE(av1->lpVtbl->QueryInterface(av1, &kIidIApplicationView2, reinterpret_cast<void**>(&av2)) == SX_WINRT_S_OK);
    TestRect vbounds{};
    OS_REQUIRE(av2->lpVtbl->get_VisibleBounds(av2, &vbounds) == SX_WINRT_S_OK);
    OS_REQUIRE(vbounds.Width >= 1280.0f && vbounds.Height >= 720.0f);

    T_IApplicationView3* av3 = nullptr;
    OS_REQUIRE(av1->lpVtbl->QueryInterface(av1, &kIidIApplicationView3, reinterpret_cast<void**>(&av3)) == SX_WINRT_S_OK);
    uint8_t entered_fs = 0, is_fs = 0;
    OS_REQUIRE(av3->lpVtbl->TryEnterFullScreenMode(av3, &entered_fs) == SX_WINRT_S_OK && entered_fs == 1u);
    OS_REQUIRE(av3->lpVtbl->get_IsFullScreenMode(av3, &is_fs) == SX_WINRT_S_OK && is_fs == 1u);
    OS_REQUIRE(av3->lpVtbl->ExitFullScreenMode(av3) == SX_WINRT_S_OK);
    OS_REQUIRE(av3->lpVtbl->get_IsFullScreenMode(av3, &is_fs) == SX_WINRT_S_OK && is_fs == 0u);

    // 2E. DisplayInformation
    SX_HSTRING h_di = MakeHString(u"Windows.Graphics.Display.DisplayInformation");
    T_IDisplayInformationStatics* di_statics = nullptr;
    OS_REQUIRE(RoGetActivationFactory(h_di, &kIidIDisplayInformationStatics, reinterpret_cast<void**>(&di_statics)) == SX_WINRT_S_OK);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(di_statics));

    T_IDisplayInformation* di1 = nullptr;
    OS_REQUIRE(di_statics->lpVtbl->GetForCurrentView(di_statics, &di1) == SX_WINRT_S_OK && di1 != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(di1));
    float dpi = 0.0f;
    uint32_t scale = 0u, orient = 0u;
    OS_REQUIRE(di1->lpVtbl->get_LogicalDpi(di1, &dpi) == SX_WINRT_S_OK && dpi == 96.0f);
    OS_REQUIRE(di1->lpVtbl->get_ResolutionScale(di1, &scale) == SX_WINRT_S_OK && scale == 100u);
    OS_REQUIRE(di1->lpVtbl->get_CurrentOrientation(di1, &orient) == SX_WINRT_S_OK && orient == 1u);

    T_IDisplayInformation2* di2 = nullptr;
    OS_REQUIRE(di1->lpVtbl->QueryInterface(di1, &kIidIDisplayInformation2, reinterpret_cast<void**>(&di2)) == SX_WINRT_S_OK);
    double rppvp = 0.0;
    OS_REQUIRE(di2->lpVtbl->get_RawPixelsPerViewPixel(di2, &rppvp) == SX_WINRT_S_OK && rppvp == 1.0);

    // 2F. GamingDeviceInformation + GamesEnumeration + GetGamingDeviceModelInformation
    SX_HSTRING h_gdi = MakeHString(u"Windows.System.Profile.GamingDeviceInformation");
    T_IGamingDeviceInfoStatics* gdi_statics = nullptr;
    OS_REQUIRE(RoGetActivationFactory(h_gdi, &kIidIGamingDeviceInformationStatics, reinterpret_cast<void**>(&gdi_statics)) == SX_WINRT_S_OK);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(gdi_statics));
    TestGamingDeviceModelInfo ginfo{};
    OS_REQUIRE(gdi_statics->lpVtbl->get_DeviceModelInformation(gdi_statics, &ginfo) == SX_WINRT_S_OK);
    OS_REQUIRE(ginfo.vendorId == 0xC2EC03D3u);
    OS_REQUIRE(ginfo.deviceId == 0x2F7A3DFFu || ginfo.deviceId == 0x1D27FABBu);
#if defined(_WIN32)
    TestGamingDeviceModelInfo ginfo_c{};
    OS_REQUIRE(p_get_gaming_info(&ginfo_c) == SX_WINRT_S_OK);
    OS_REQUIRE(ginfo_c.vendorId == ginfo.vendorId && ginfo_c.deviceId == ginfo.deviceId);
#endif
    SX_HSTRING h_ge = MakeHString(u"Windows.Gaming.Preview.GamesEnumeration.GameList");
    void* ge_inst = nullptr;
    OS_REQUIRE(RoActivateInstance(h_ge, &ge_inst) == SX_WINRT_S_OK && ge_inst != nullptr);
    AssertRefusesReferenceTracker(reinterpret_cast<SX_IUnknown*>(ge_inst));
    reinterpret_cast<SX_IUnknown*>(ge_inst)->lpVtbl->Release(reinterpret_cast<SX_IUnknown*>(ge_inst));

    // Clean teardown
    gdi_statics->lpVtbl->Release(gdi_statics);
    di2->lpVtbl->Release(di2);
    di1->lpVtbl->Release(di1);
    di_statics->lpVtbl->Release(di_statics);
    av3->lpVtbl->Release(av3);
    av2->lpVtbl->Release(av2);
    av1->lpVtbl->Release(av1);
    av_statics->lpVtbl->Release(av_statics);
    disp->lpVtbl->Release(disp);
    core_win->lpVtbl->Release(core_win);
    cw_statics->lpVtbl->Release(cw_statics);
    app_view->lpVtbl->Release(app_view);
    core_app2->lpVtbl->Release(core_app2);
    core_app->lpVtbl->Release(core_app);

    WindowsDeleteString(h_ge);
    WindowsDeleteString(h_gdi);
    WindowsDeleteString(h_di);
    WindowsDeleteString(h_av);
    WindowsDeleteString(h_cw);
    WindowsDeleteString(h_ca);
    RoUninitialize();
    std::printf("  PASS: winrt_xs lifecycle, CoreWindow/AppView/DisplayInfo & IReferenceTracker refusal\n");
}

// ===========================================================================
// Suite 3: Non-Blocking Offline Auth, XSTS/JWT, XStore, XSAPI & XCurl Boot
// ===========================================================================
struct PortCounterCtx {
    std::atomic<int32_t> count{0};
    std::vector<int32_t> order;
};

void OPENSCARLETT_GDK_CALL PortCountCb(void* ctx, bool canceled) {
    if (canceled) return;
    auto* c = static_cast<PortCounterCtx*>(ctx);
    c->count.fetch_add(1, std::memory_order_acq_rel);
}

struct SerializedItemCtx {
    PortCounterCtx* shared;
    int32_t seq;
};

void OPENSCARLETT_GDK_CALL SerializedCb(void* ctx, bool canceled) {
    auto* item = static_cast<SerializedItemCtx*>(ctx);
    if (!canceled && item && item->shared) {
        item->shared->order.push_back(item->seq);
        item->shared->count.fetch_add(1, std::memory_order_acq_rel);
    }
}

void OPENSCARLETT_GDK_CALL AsyncFlagCb(XAsyncBlock* ab) {
    auto* flag = static_cast<std::atomic<bool>*>(ab->context);
    flag->store(true, std::memory_order_release);
}

size_t OS_SHIM_CDECL CurlCaptureWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

void ValidateJwtThreeSegments(const std::string& jwt) {
    const size_t dot1 = jwt.find('.');
    OS_REQUIRE(dot1 != std::string::npos);
    const size_t dot2 = jwt.find('.', dot1 + 1u);
    OS_REQUIRE(dot2 != std::string::npos);
    OS_REQUIRE(jwt.find('.', dot2 + 1u) == std::string::npos);

    const std::string seg_hdr = jwt.substr(0, dot1);
    const std::string seg_pay = jwt.substr(dot1 + 1u, dot2 - dot1 - 1u);
    const std::string seg_sig = jwt.substr(dot2 + 1u);
    OS_REQUIRE(ValidateBase64UrlChars(seg_hdr));
    OS_REQUIRE(ValidateBase64UrlChars(seg_pay));
    OS_REQUIRE(ValidateBase64UrlChars(seg_sig));

    const std::string hdr_json = Base64UrlDecode(seg_hdr);
    const std::string pay_json = Base64UrlDecode(seg_pay);
    OS_REQUIRE(hdr_json.find("\"alg\":\"ES256\"") != std::string::npos);
    OS_REQUIRE(hdr_json.find("\"typ\":\"JWT\"") != std::string::npos);
    OS_REQUIRE(pay_json.find("\"uhs\":\"0000000000000001\"") != std::string::npos);
    OS_REQUIRE(pay_json.find("ScarlettPlayer") != std::string::npos);
}

void TestOfflineAuthStoreAndNetworkBoot() {
    std::printf("[Suite 3] xgameruntime + xsapi + xcurl full non-blocking offline boot sequence...\n");
    const auto t0 = std::chrono::steady_clock::now();

    const fs::path temp_save_root =
        fs::temp_directory_path() / "openscarlett_test_os_winrt_offline_auth";
    std::error_code ec;
    fs::remove_all(temp_save_root, ec);
    fs::create_directories(temp_save_root, ec);

    SetEnvVar("OPENSCARLETT_SAVE_ROOT", temp_save_root.string().c_str());
    SetEnvVar("OPENSCARLETT_XUID", nullptr);
    SetEnvVar("OPENSCARLETT_GAMERTAG", nullptr);
    SetEnvVar("OPENSCARLETT_OFFLINE", "1");

    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // 3A. Verify XTaskQueue across ALL 4 dispatch modes (Manual, Immediate, ThreadPool, SerializedThreadPool)
    {
        // Manual Work + Manual Completion
        XTaskQueueHandle q_man = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &q_man) == XGR_S_OK && q_man != nullptr);
        PortCounterCtx work_cnt{}, comp_cnt{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q_man, XTaskQueuePort_Work, &work_cnt, &PortCountCb) == XGR_S_OK);
        OS_REQUIRE(XTaskQueueSubmitCallback(q_man, XTaskQueuePort_Completion, &comp_cnt, &PortCountCb) == XGR_S_OK);
        OS_REQUIRE(work_cnt.count.load() == 0 && comp_cnt.count.load() == 0);
        OS_REQUIRE(XTaskQueueDispatch(q_man, XTaskQueuePort_Work, 0));
        OS_REQUIRE(work_cnt.count.load() == 1 && comp_cnt.count.load() == 0);
        OS_REQUIRE(XTaskQueueDispatch(q_man, XTaskQueuePort_Completion, 0));
        OS_REQUIRE(comp_cnt.count.load() == 1);
        XTaskQueueCloseHandle(q_man);

        // Immediate Work + Immediate Completion
        XTaskQueueHandle q_imm = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Immediate,
                       XTaskQueueDispatchMode_Immediate,
                       &q_imm) == XGR_S_OK && q_imm != nullptr);
        PortCounterCtx imm_cnt{};
        OS_REQUIRE(XTaskQueueSubmitCallback(q_imm, XTaskQueuePort_Work, &imm_cnt, &PortCountCb) == XGR_S_OK);
        OS_REQUIRE(XTaskQueueSubmitCallback(q_imm, XTaskQueuePort_Completion, &imm_cnt, &PortCountCb) == XGR_S_OK);
        OS_REQUIRE(imm_cnt.count.load() == 2);
        XTaskQueueCloseHandle(q_imm);

        // SerializedThreadPool strict FIFO ordering
        XTaskQueueHandle q_ser = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_SerializedThreadPool,
                       XTaskQueueDispatchMode_SerializedThreadPool,
                       &q_ser) == XGR_S_OK && q_ser != nullptr);
        PortCounterCtx ser_shared{};
        std::vector<SerializedItemCtx> items(16);
        for (int32_t i = 0; i < 16; ++i) {
            items[static_cast<size_t>(i)] = SerializedItemCtx{&ser_shared, i};
            OS_REQUIRE(XTaskQueueSubmitCallback(
                           q_ser, XTaskQueuePort_Work, &items[static_cast<size_t>(i)], &SerializedCb) == XGR_S_OK);
        }
        const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (ser_shared.count.load() < 16 && std::chrono::steady_clock::now() < wait_deadline) {
            std::this_thread::yield();
        }
        OS_REQUIRE(ser_shared.count.load() == 16);
        OS_REQUIRE(ser_shared.order.size() == 16u);
        for (int32_t i = 0; i < 16; ++i) {
            OS_REQUIRE(ser_shared.order[static_cast<size_t>(i)] == i);
        }
        XTaskQueueCloseHandle(q_ser);
    }

    // 3B. ThreadPool Work + Manual Completion queue (canonical GDK game loop pattern)
    XTaskQueueHandle boot_q = nullptr;
    OS_REQUIRE(XTaskQueueCreate(
                   XTaskQueueDispatchMode_ThreadPool,
                   XTaskQueueDispatchMode_Manual,
                   &boot_q) == XGR_S_OK && boot_q != nullptr);

    // XUserAddAsync -> offline mock user profile
    std::atomic<bool> user_done{false};
    XAsyncBlock user_ab{};
    user_ab.queue = boot_q;
    user_ab.context = &user_done;
    user_ab.callback = &AsyncFlagCb;
    OS_REQUIRE(XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_ab) == XGR_S_OK);
    OS_REQUIRE(XTaskQueueDispatch(boot_q, XTaskQueuePort_Completion, 1000u));
    OS_REQUIRE(user_done.load());

    XUserHandle user = nullptr;
    OS_REQUIRE(XUserAddResult(&user_ab, &user) == XGR_S_OK && user != nullptr);

    uint64_t xuid = 0;
    OS_REQUIRE(XUserGetId(user, &xuid) == XGR_S_OK);
    OS_REQUIRE(xuid == 0x0009000000001337ULL);

    char gt[64]{};
    size_t gt_used = 0;
    OS_REQUIRE(XUserGetGamertag(user, XUserGamertagComponent_Classic, sizeof(gt), gt, &gt_used) == XGR_S_OK);
    OS_REQUIRE(std::strcmp(gt, "ScarlettPlayer") == 0);

    XUserState ustate = XUserState_SignedOut;
    OS_REQUIRE(XUserGetState(user, &ustate) == XGR_S_OK && ustate == XUserState_SignedIn);

    XUserAgeGroup uage = XUserAgeGroup_Unknown;
    OS_REQUIRE(XUserGetAgeGroup(user, &uage) == XGR_S_OK && uage == XUserAgeGroup_Adult);

    bool has_priv = false;
    XUserPrivilegeDenyReason deny = XUserPrivilegeDenyReason_Unknown;
    OS_REQUIRE(XUserCheckPrivilege(
                   user, XUserPrivilegeOptions_None, XUserPrivilege_Multiplayer, &has_priv, &deny) == XGR_S_OK);
    OS_REQUIRE(has_priv && deny == XUserPrivilegeDenyReason_None);

    // XUserGetGamerPictureAsync -> valid PNG header
    XAsyncBlock pic_ab{};
    OS_REQUIRE(XUserGetGamerPictureAsync(user, XUserGamerPictureSize_Small, &pic_ab) == XGR_S_OK);
    size_t pic_sz = 0;
    OS_REQUIRE(XUserGetGamerPictureResultSize(&pic_ab, &pic_sz) == XGR_S_OK && pic_sz > 16u);
    std::vector<uint8_t> pic_buf(pic_sz);
    size_t pic_used = 0;
    OS_REQUIRE(XUserGetGamerPictureResult(&pic_ab, pic_buf.size(), pic_buf.data(), &pic_used) == XGR_S_OK);
    OS_REQUIRE(pic_used >= 8u && pic_buf[0] == 0x89u && pic_buf[1] == 'P' && pic_buf[2] == 'N' && pic_buf[3] == 'G');

    // 3C. XUserGetTokenAndSignatureAsync -> XBL3.0 + 3-segment Base64URL JWT (opt-in mock XSTS)
    {
        struct ScopedMockXstsEnv {
            bool had_prev = false;
            std::string prev;
            ScopedMockXstsEnv() {
#if defined(_WIN32)
                char buf[256]{};
                const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", buf, sizeof(buf));
                if (n > 0 && n < sizeof(buf)) {
                    had_prev = true;
                    prev.assign(buf, n);
                }
                ::SetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", "1");
#else
                const char* v = std::getenv("OPENSCARLETT_MOCK_XSTS");
                if (v) {
                    had_prev = true;
                    prev = v;
                }
                setenv("OPENSCARLETT_MOCK_XSTS", "1", 1);
#endif
            }
            ~ScopedMockXstsEnv() {
#if defined(_WIN32)
                ::SetEnvironmentVariableA("OPENSCARLETT_MOCK_XSTS", had_prev ? prev.c_str() : nullptr);
#else
                if (had_prev) setenv("OPENSCARLETT_MOCK_XSTS", prev.c_str(), 1);
                else unsetenv("OPENSCARLETT_MOCK_XSTS");
#endif
            }
        } mock_xsts_guard;

        std::atomic<bool> tok_done{false};
        XAsyncBlock tok_ab{};
        tok_ab.queue = boot_q;
        tok_ab.context = &tok_done;
        tok_ab.callback = &AsyncFlagCb;
        OS_REQUIRE(XUserGetTokenAndSignatureAsync(
                       user, XUserGetTokenAndSignatureOptions_None,
                       "GET", "https://title.mgt.xboxlive.com/titles/default",
                       0, nullptr, 0, nullptr, &tok_ab) == XGR_S_OK);
        OS_REQUIRE(XTaskQueueDispatch(boot_q, XTaskQueuePort_Completion, 1000u));
        OS_REQUIRE(tok_done.load());

        size_t tsz = 0;
        OS_REQUIRE(XUserGetTokenAndSignatureResultSize(&tok_ab, &tsz) == XGR_S_OK &&
                   tsz > sizeof(XUserGetTokenAndSignatureData));
        std::vector<uint8_t> tbuf(tsz);
        XUserGetTokenAndSignatureData* tdata = nullptr;
        size_t tused = 0;
        OS_REQUIRE(XUserGetTokenAndSignatureResult(&tok_ab, tbuf.size(), tbuf.data(), &tdata, &tused) == XGR_S_OK);
        OS_REQUIRE(tdata != nullptr && tdata->token != nullptr && tdata->signature != nullptr);
        OS_REQUIRE(tdata->tokenSize > 20u && tdata->signatureSize > 1u);

        const std::string tok_str(tdata->token);
        const std::string expected_prefix = "XBL3.0 x=0000000000000001;";
        OS_REQUIRE(tok_str.rfind(expected_prefix, 0) == 0);
        ValidateJwtThreeSegments(tok_str.substr(expected_prefix.size()));
    }

    // 3D. XStore non-blocking license checks
    {
        XStoreContextHandle sctx = nullptr;
        OS_REQUIRE(XStoreCreateContext(user, &sctx) == XGR_S_OK && sctx != nullptr);

        XAsyncBlock gl_ab{};
        OS_REQUIRE(XStoreQueryGameLicenseAsync(sctx, &gl_ab) == XGR_S_OK);
        XStoreGameLicense glic{};
        OS_REQUIRE(XStoreQueryGameLicenseResult(&gl_ab, &glic) == XGR_S_OK);
        OS_REQUIRE(glic.isActive && !glic.isTrial);

        XAsyncBlock can_ab{};
        OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdAsync(sctx, "9NBLGGH4NNS1", &can_ab) == XGR_S_OK);
        XStoreCanAcquireLicenseResult can_res{};
        OS_REQUIRE(XStoreCanAcquireLicenseForStoreIdResult(&can_ab, &can_res) == XGR_S_OK);
        OS_REQUIRE(can_res.status == XStoreCanLicenseStatus_Licensable);

        XAsyncBlock acq_ab{};
        OS_REQUIRE(XStoreAcquireLicenseForPackageAsync(sctx, "OpenScarlett.TestPkg_1.0", &acq_ab) == XGR_S_OK);
        XStoreLicenseHandle lic_h = nullptr;
        OS_REQUIRE(XStoreAcquireLicenseForPackageResult(&acq_ab, &lic_h) == XGR_S_OK && lic_h != nullptr);
        OS_REQUIRE(XStoreIsLicenseValid(lic_h));
        XStoreCloseLicenseHandle(lic_h);

        XAsyncBlock assoc_ab{};
        OS_REQUIRE(XStoreQueryAssociatedProductsAsync(sctx, XStoreProductKind_Durable, 10u, &assoc_ab) == XGR_S_OK);
        XStoreProductQueryHandle assoc_q = nullptr;
        OS_REQUIRE(XStoreQueryAssociatedProductsResult(&assoc_ab, &assoc_q) == XGR_S_OK && assoc_q != nullptr);
        XStoreCloseProductsQueryHandle(assoc_q);

        XStoreCloseContextHandle(sctx);
    }

    // 3E. XSAPI non-blocking offline context + Achievements + Stats + Presence + Social + TitleStorage + MPA
    {
        XblInitArgs init_args{};
        init_args.queue = boot_q;
        init_args.scid = "00000000-0000-0000-0000-00005ca00001";
        OS_REQUIRE(XblInitialize(&init_args) == XBL_S_OK);

        XblContextHandle xbl_ctx = nullptr;
        OS_REQUIRE(XblContextCreateHandle(user, &xbl_ctx) == XBL_S_OK && xbl_ctx != nullptr);

        std::atomic<bool> ach_done{false};
        XAsyncBlock ach_ab{};
        ach_ab.queue = boot_q;
        ach_ab.context = &ach_done;
        ach_ab.callback = &AsyncFlagCb;
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdAsync(
                       xbl_ctx, xuid, 0x5CA00001u, XblAchievementType_All,
                       false, XblAchievementOrderBy_DefaultOrder, 0u, 32u, &ach_ab) == XBL_S_OK);
        OS_REQUIRE(XTaskQueueDispatch(boot_q, XTaskQueuePort_Completion, 1000u));
        OS_REQUIRE(ach_done.load());
        XblAchievementsResultHandle ach_res = nullptr;
        OS_REQUIRE(XblAchievementsGetAchievementsForTitleIdResult(&ach_ab, &ach_res) == XBL_S_OK && ach_res != nullptr);
        XblAchievementsResultCloseHandle(ach_res);

        // UserStatistics, Presence, Social, TitleStorage
        XAsyncBlock st_ab{};
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticAsync(
                       xbl_ctx, xuid, init_args.scid, "Score", &st_ab) == XBL_S_OK);
        size_t st_sz = 0;
        OS_REQUIRE(XblUserStatisticsGetSingleUserStatisticResultSize(&st_ab, &st_sz) == XBL_S_OK);

        XAsyncBlock pr_ab{};
        OS_REQUIRE(XblPresenceGetPresenceAsync(xbl_ctx, xuid, &pr_ab) == XBL_S_OK);
        XblPresenceRecordHandle pr_h = nullptr;
        OS_REQUIRE(XblPresenceGetPresenceResult(&pr_ab, &pr_h) == XBL_S_OK && pr_h != nullptr);
        XblPresenceRecordCloseHandle(pr_h);

        XAsyncBlock soc_ab{};
        OS_REQUIRE(XblSocialGetSocialRelationshipsAsync(
                       xbl_ctx, xuid, XblSocialRelationshipFilter_All, 0u, 10u, &soc_ab) == XBL_S_OK);
        XblSocialRelationshipResultHandle soc_h = nullptr;
        OS_REQUIRE(XblSocialGetSocialRelationshipsResult(&soc_ab, &soc_h) == XBL_S_OK && soc_h != nullptr);
        XblSocialRelationshipResultCloseHandle(soc_h);

        XAsyncBlock ts_ab{};
        OS_REQUIRE(XblTitleStorageGetQuotaAsync(
                       xbl_ctx, init_args.scid, XblTitleStorageType_TrustedPlatformStorage, &ts_ab) == XBL_S_OK);
        size_t used_b = 0, quota_b = 0;
        OS_REQUIRE(XblTitleStorageGetQuotaResult(&ts_ab, &used_b, &quota_b) == XBL_S_OK && quota_b > 0u);

#if defined(_WIN32)
        // Verify new XblMultiplayerActivity* non-blocking offline exports
        HMODULE h_xsapi = ::GetModuleHandleW(L"Microsoft.Xbox.Services.GDK.C.dll");
        if (!h_xsapi) {
            h_xsapi = ::LoadLibraryW(L"Microsoft.Xbox.Services.GDK.C.dll");
        }
        OS_REQUIRE(h_xsapi != nullptr);
        using FnMpaSet = XGameResult(OPENSCARLETT_XSAPI_CALL*)(XblContextHandle, const void*, bool, XAsyncBlock*);
        using FnMpaDel = XGameResult(OPENSCARLETT_XSAPI_CALL*)(XblContextHandle, XAsyncBlock*);
        auto p_mpa_set = reinterpret_cast<FnMpaSet>(::GetProcAddress(h_xsapi, "XblMultiplayerActivitySetActivityAsync"));
        auto p_mpa_del = reinterpret_cast<FnMpaDel>(::GetProcAddress(h_xsapi, "XblMultiplayerActivityDeleteActivityAsync"));
        OS_REQUIRE(p_mpa_set != nullptr && p_mpa_del != nullptr);

        int dummy_activity = 1;
        XAsyncBlock mpa_ab1{}, mpa_ab2{};
        OS_REQUIRE(p_mpa_set(xbl_ctx, &dummy_activity, true, &mpa_ab1) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&mpa_ab1, true) == XBL_S_OK);
        OS_REQUIRE(p_mpa_del(xbl_ctx, &mpa_ab2) == XBL_S_OK);
        OS_REQUIRE(XAsyncGetStatus(&mpa_ab2, true) == XBL_S_OK);
#endif

        XblContextCloseHandle(xbl_ctx);
        XblCleanup();
    }

    // 3F. XCurl offline XSTS / Xbox Live auth HTTP 200 JSON (easy + multi, zero network stalls)
    {
        OS_REQUIRE(curl_global_init(XCURL_GLOBAL_DEFAULT) == XCURLE_OK);

        XCurlEasyHandle easy = curl_easy_init();
        OS_REQUIRE(easy != nullptr);
        std::string xsts_body;
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://xsts.auth.xboxlive.com/xsts/authorize") == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_POSTFIELDS, "{\"RelyingParty\":\"http://xboxlive.com\"}") == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWrite) == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &xsts_body) == XCURLE_OK);
        OS_REQUIRE(curl_easy_perform(easy) == XCURLE_OK);

        long code = 0;
        OS_REQUIRE(curl_easy_getinfo(easy, XCURLINFO_RESPONSE_CODE, &code) == XCURLE_OK && code == 200L);
        OS_REQUIRE(xsts_body.find("\"IssueInstant\"") != std::string::npos);
        OS_REQUIRE(xsts_body.find("\"DisplayClaims\"") != std::string::npos);
        OS_REQUIRE(xsts_body.find("\"uhs\":\"0000000000000001\"") != std::string::npos);
        OS_REQUIRE(xsts_body.find("\"gtg\":\"ScarlettPlayer\"") != std::string::npos);

        // Extract "Token":"<jwt>" and validate 3-segment JWT!
        const std::string tok_key = "\"Token\":\"";
        const size_t tpos = xsts_body.find(tok_key);
        OS_REQUIRE(tpos != std::string::npos);
        const size_t tend = xsts_body.find('"', tpos + tok_key.size());
        OS_REQUIRE(tend != std::string::npos);
        ValidateJwtThreeSegments(xsts_body.substr(tpos + tok_key.size(), tend - (tpos + tok_key.size())));

        // Multi transfer against user.auth.xboxlive.com & title.mgt.xboxlive.com
        curl_easy_reset(easy);
        XCurlEasyHandle easy2 = curl_easy_init();
        OS_REQUIRE(easy2 != nullptr);
        std::string user_auth_body, title_mgt_body;
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_URL, "https://user.auth.xboxlive.com/user/authenticate") == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEFUNCTION, &CurlCaptureWrite) == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy, XCURLOPT_WRITEDATA, &user_auth_body) == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_URL, "https://title.mgt.xboxlive.com/titles/default") == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEFUNCTION, &CurlCaptureWrite) == XCURLE_OK);
        OS_REQUIRE(curl_easy_setopt(easy2, XCURLOPT_WRITEDATA, &title_mgt_body) == XCURLE_OK);

        XCurlMultiHandle multi = curl_multi_init();
        OS_REQUIRE(multi != nullptr);
        OS_REQUIRE(curl_multi_add_handle(multi, easy) == XCURLM_OK);
        OS_REQUIRE(curl_multi_add_handle(multi, easy2) == XCURLM_OK);
        int32_t running = -1;
        OS_REQUIRE(curl_multi_perform(multi, &running) == XCURLM_OK && running == 0);

        int completed = 0;
        int32_t in_q = 0;
        while (XCurlMsg* msg = curl_multi_info_read(multi, &in_q)) {
            OS_REQUIRE(msg->msg == XCURLMSG_DONE && msg->data.result == XCURLE_OK);
            ++completed;
        }
        OS_REQUIRE(completed == 2);
        OS_REQUIRE(user_auth_body.find("\"DisplayClaims\"") != std::string::npos);
        OS_REQUIRE(title_mgt_body.find("\"titleId\"") != std::string::npos);

        OS_REQUIRE(curl_multi_remove_handle(multi, easy) == XCURLM_OK);
        OS_REQUIRE(curl_multi_remove_handle(multi, easy2) == XCURLM_OK);
        OS_REQUIRE(curl_multi_cleanup(multi) == XCURLM_OK);
        curl_easy_cleanup(easy2);
        curl_easy_cleanup(easy);
        curl_global_cleanup();
    }

    XUserCloseHandle(user);
    XTaskQueueCloseHandle(boot_q);
    XGameRuntimeUninitialize();

    fs::remove_all(temp_save_root, ec);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("  PASS: Offline auth + XSTS/JWT + XStore + XSAPI + XCurl boot completed in %lld ms (< 2000 ms)\n",
                static_cast<long long>(elapsed_ms));
    OS_REQUIRE(elapsed_ms < 2000);
}

}  // namespace

int main() {
    TestKernelXAffinityRemapping();
    TestWinRtAppLifecycle();
    TestOfflineAuthStoreAndNetworkBoot();
    std::printf("[PASS] test_os_winrt_offline_auth (all 3 suites green)\n");
    return 0;
}
