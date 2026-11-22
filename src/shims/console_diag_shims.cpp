// MIT Console Diagnostic & Legacy Compatibility Shims for OpenScarlett.
// Compiled into standalone DLL targets via preprocessor target macros:
//   - OPENSCARLETT_DIAG_TARGET_ADVAPI32LEGACY     -> advapi32legacy.dll
//   - OPENSCARLETT_DIAG_TARGET_KERNELTRACECONTROL -> KernelTraceControl.dll
//   - OPENSCARLETT_DIAG_TARGET_HIX_XS             -> hix_xs.dll
//   - OPENSCARLETT_DIAG_TARGET_XBTP               -> xbtp.dll
//   - OPENSCARLETT_DIAG_TARGET_WLDP_XS            -> wldp_xs.dll
//   - OPENSCARLETT_DIAG_TARGET_NTDLL_UMS_XS       -> ntdll_ums_xs.dll
//
// Every exported API logs into openscarlett::CallTelemetry and stays portable
// across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
// Temporarily rename Win32 SDK prototypes that we re-export from advapi32legacy.dll,
// KernelTraceControl.dll, and ntdll_ums_xs.dll so MSVC, Clang-CL, and MinGW-w64
// never report dllimport/dllexport linkage conflicts.
#  define RegOpenKeyExW _os_win_RegOpenKeyExW
#  define RegOpenKeyExA _os_win_RegOpenKeyExA
#  define RegQueryValueExW _os_win_RegQueryValueExW
#  define RegQueryValueExA _os_win_RegQueryValueExA
#  define RegCloseKey _os_win_RegCloseKey
#  define GetUserNameW _os_win_GetUserNameW
#  define GetUserNameA _os_win_GetUserNameA
#  define CryptAcquireContextW _os_win_CryptAcquireContextW
#  define CryptGenRandom _os_win_CryptGenRandom
#  define CryptReleaseContext _os_win_CryptReleaseContext
#  define EventRegister _os_win_EventRegister
#  define EventWrite _os_win_EventWrite
#  define EventUnregister _os_win_EventUnregister
#  define StartKernelTrace _os_win_StartKernelTrace
#  define StopKernelTrace _os_win_StopKernelTrace
#  define EnableTraceEx2 _os_win_EnableTraceEx2
#  define TraceQueryInformation _os_win_TraceQueryInformation
#  define TraceSetInformation _os_win_TraceSetInformation
#  define CreateUmsCompletionList _os_win_CreateUmsCompletionList
#  define DeleteUmsCompletionList _os_win_DeleteUmsCompletionList
#  define DequeueUmsCompletionListItems _os_win_DequeueUmsCompletionListItems
#  define GetUmsCompletionListEvent _os_win_GetUmsCompletionListEvent
#  define CreateUmsThreadContext _os_win_CreateUmsThreadContext
#  define DeleteUmsThreadContext _os_win_DeleteUmsThreadContext
#  define EnterUmsSchedulingMode _os_win_EnterUmsSchedulingMode
#  define ExecuteUmsThread _os_win_ExecuteUmsThread
#  define UmsThreadYield _os_win_UmsThreadYield
#  define QueryUmsThreadInformation _os_win_QueryUmsThreadInformation
#  define SetUmsThreadInformation _os_win_SetUmsThreadInformation
#  define GetCurrentUmsThread _os_win_GetCurrentUmsThread
#  define GetNextUmsListItem _os_win_GetNextUmsListItem
#  include <windows.h>
#  undef RegOpenKeyExW
#  undef RegOpenKeyExA
#  undef RegQueryValueExW
#  undef RegQueryValueExA
#  undef RegCloseKey
#  undef GetUserNameW
#  undef GetUserNameA
#  undef CryptAcquireContextW
#  undef CryptGenRandom
#  undef CryptReleaseContext
#  undef EventRegister
#  undef EventWrite
#  undef EventUnregister
#  undef StartKernelTrace
#  undef StopKernelTrace
#  undef EnableTraceEx2
#  undef TraceQueryInformation
#  undef TraceSetInformation
#  undef CreateUmsCompletionList
#  undef DeleteUmsCompletionList
#  undef DequeueUmsCompletionListItems
#  undef GetUmsCompletionListEvent
#  undef CreateUmsThreadContext
#  undef DeleteUmsThreadContext
#  undef EnterUmsSchedulingMode
#  undef ExecuteUmsThread
#  undef UmsThreadYield
#  undef QueryUmsThreadInformation
#  undef SetUmsThreadInformation
#  undef GetCurrentUmsThread
#  undef GetNextUmsListItem
#endif

#if defined(_WIN32)
#  define OS_DIAG_API __declspec(dllexport)
#  define OS_DIAG_CALL __stdcall
#else
#  define OS_DIAG_API __attribute__((visibility("default")))
#  define OS_DIAG_CALL
#endif

namespace {

[[maybe_unused]] constexpr int32_t kHrOk = 0x00000000;
[[maybe_unused]] constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
[[maybe_unused]] constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
[[maybe_unused]] constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
[[maybe_unused]] constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);

int32_t QueryModuleTelemetry(
    const char* module_name,
    const char* symbol_name,
    uint64_t* out_total,
    uint64_t* out_ok) {
    if (out_total) *out_total = 0u;
    if (out_ok) *out_ok = 0u;
    if (!symbol_name) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == module_name && entry.symbol_name == symbol_name) {
            if (out_total) *out_total = entry.total_calls;
            if (out_ok) *out_ok = entry.ok_calls;
            return 1;
        }
    }
    return 0;
}

#if defined(_WIN32)
[[maybe_unused]] HMODULE LoadSystemDll(const wchar_t* dll_name) {
    wchar_t sys_dir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len > 0u && len < MAX_PATH - 32u) {
        std::wstring path(sys_dir, len);
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path += dll_name;
        HMODULE mod = LoadLibraryW(path.c_str());
        if (mod) return mod;
    }
    return LoadLibraryW(dll_name);
}
#endif

}  // namespace

// ===========================================================================
// TARGET 1: advapi32legacy.dll (UE4 startup / login / crypto / ETW parity)
// ===========================================================================
#if defined(OPENSCARLETT_DIAG_TARGET_ADVAPI32LEGACY)

namespace {

constexpr const char* kAdvapiMod = "advapi32legacy.dll";
constexpr uint32_t kErrSuccess = 0u;
constexpr uint32_t kErrInvalidParam = 87u;
constexpr uint32_t kErrInsufficientBuf = 122u;
constexpr uint32_t kErrCallNotImpl = 120u;

inline void RecordAdvapi(const char* sym, bool ok, int32_t code = 0) {
    openscarlett::CallTelemetry::Global().Record(
        kAdvapiMod,
        sym,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        code,
        -1);
}

#if defined(_WIN32)
using PfnRegOpenKeyExW = int32_t(WINAPI*)(void*, const wchar_t*, uint32_t, uint32_t, void**);
using PfnRegOpenKeyExA = int32_t(WINAPI*)(void*, const char*, uint32_t, uint32_t, void**);
using PfnRegQueryValueExW = int32_t(WINAPI*)(void*, const wchar_t*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
using PfnRegQueryValueExA = int32_t(WINAPI*)(void*, const char*, uint32_t*, uint32_t*, uint8_t*, uint32_t*);
using PfnRegCloseKey = int32_t(WINAPI*)(void*);
using PfnGetUserNameW = int32_t(WINAPI*)(wchar_t*, uint32_t*);
using PfnGetUserNameA = int32_t(WINAPI*)(char*, uint32_t*);
using PfnCryptAcquireContextW = int32_t(WINAPI*)(uintptr_t*, const wchar_t*, const wchar_t*, uint32_t, uint32_t);
using PfnCryptGenRandom = int32_t(WINAPI*)(uintptr_t, uint32_t, uint8_t*);
using PfnCryptReleaseContext = int32_t(WINAPI*)(uintptr_t, uint32_t);
using PfnEventRegister = uint32_t(WINAPI*)(const void*, void*, void*, uint64_t*);
using PfnEventWrite = uint32_t(WINAPI*)(uint64_t, const void*, uint32_t, void*);
using PfnEventUnregister = uint32_t(WINAPI*)(uint64_t);
using PfnBCryptGenRandom = int32_t(WINAPI*)(void*, uint8_t*, uint32_t, uint32_t);

struct HostAdvapi {
    std::mutex mutex;
    bool loaded = false;
    HMODULE advapi = nullptr;
    HMODULE bcrypt = nullptr;

    PfnRegOpenKeyExW reg_open_w = nullptr;
    PfnRegOpenKeyExA reg_open_a = nullptr;
    PfnRegQueryValueExW reg_query_w = nullptr;
    PfnRegQueryValueExA reg_query_a = nullptr;
    PfnRegCloseKey reg_close = nullptr;
    PfnGetUserNameW get_user_w = nullptr;
    PfnGetUserNameA get_user_a = nullptr;
    PfnCryptAcquireContextW crypt_acq_w = nullptr;
    PfnCryptGenRandom crypt_gen_random = nullptr;
    PfnCryptReleaseContext crypt_rel = nullptr;
    PfnEventRegister event_reg = nullptr;
    PfnEventWrite event_write = nullptr;
    PfnEventUnregister event_unreg = nullptr;
    PfnBCryptGenRandom bcrypt_gen_random = nullptr;

    std::atomic<uintptr_t> next_local_prov{0x00A10001u};
    std::atomic<uint64_t> next_local_etw{0x00E70001ull};
    std::unordered_set<uintptr_t> local_provs;
    std::unordered_set<uint64_t> local_etw_handles;

    void EnsureLoaded() {
        std::lock_guard<std::mutex> lk(mutex);
        if (loaded) return;
        loaded = true;
        advapi = LoadSystemDll(L"advapi32.dll");
        bcrypt = LoadSystemDll(L"bcrypt.dll");
        if (advapi) {
            auto sym = [&](const char* n) -> void* {
                return reinterpret_cast<void*>(GetProcAddress(advapi, n));
            };
            reg_open_w = reinterpret_cast<PfnRegOpenKeyExW>(sym("RegOpenKeyExW"));
            reg_open_a = reinterpret_cast<PfnRegOpenKeyExA>(sym("RegOpenKeyExA"));
            reg_query_w = reinterpret_cast<PfnRegQueryValueExW>(sym("RegQueryValueExW"));
            reg_query_a = reinterpret_cast<PfnRegQueryValueExA>(sym("RegQueryValueExA"));
            reg_close = reinterpret_cast<PfnRegCloseKey>(sym("RegCloseKey"));
            get_user_w = reinterpret_cast<PfnGetUserNameW>(sym("GetUserNameW"));
            get_user_a = reinterpret_cast<PfnGetUserNameA>(sym("GetUserNameA"));
            crypt_acq_w = reinterpret_cast<PfnCryptAcquireContextW>(sym("CryptAcquireContextW"));
            crypt_gen_random = reinterpret_cast<PfnCryptGenRandom>(sym("CryptGenRandom"));
            crypt_rel = reinterpret_cast<PfnCryptReleaseContext>(sym("CryptReleaseContext"));
            event_reg = reinterpret_cast<PfnEventRegister>(sym("EventRegister"));
            event_write = reinterpret_cast<PfnEventWrite>(sym("EventWrite"));
            event_unreg = reinterpret_cast<PfnEventUnregister>(sym("EventUnregister"));
        }
        if (bcrypt) {
            bcrypt_gen_random = reinterpret_cast<PfnBCryptGenRandom>(
                reinterpret_cast<void*>(GetProcAddress(bcrypt, "BCryptGenRandom")));
        }
    }

    void FillRandomFallback(uint8_t* dst, uint32_t len) {
        if (!dst || len == 0u) return;
        if (bcrypt_gen_random && bcrypt_gen_random(nullptr, dst, len, 0x00000002u) >= 0) {
            return;
        }
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32u) ^ static_cast<uint64_t>(rd()) ^
                     static_cast<uint64_t>(GetTickCount64());
        for (uint32_t i = 0u; i < len; ++i) {
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
            z = z ^ (z >> 31u);
            dst[i] = static_cast<uint8_t>(z & 0xFFu);
        }
    }
};

HostAdvapi& GetHostAdvapi() {
    static HostAdvapi* s = new HostAdvapi();
    return *s;
}
#endif

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL RegOpenKeyExW(
    void* hKey,
    const wchar_t* lpSubKey,
    uint32_t ulOptions,
    uint32_t samDesired,
    void** phkResult) {
    if (!phkResult) {
        RecordAdvapi("RegOpenKeyExW", false, static_cast<int32_t>(kErrInvalidParam));
        return static_cast<int32_t>(kErrInvalidParam);
    }
    *phkResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_open_w) {
        const int32_t st = host.reg_open_w(hKey, lpSubKey, ulOptions, samDesired, phkResult);
        RecordAdvapi("RegOpenKeyExW", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpSubKey; (void)ulOptions; (void)samDesired;
#endif
    RecordAdvapi("RegOpenKeyExW", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegOpenKeyExA(
    void* hKey,
    const char* lpSubKey,
    uint32_t ulOptions,
    uint32_t samDesired,
    void** phkResult) {
    if (!phkResult) {
        RecordAdvapi("RegOpenKeyExA", false, static_cast<int32_t>(kErrInvalidParam));
        return static_cast<int32_t>(kErrInvalidParam);
    }
    *phkResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_open_a) {
        const int32_t st = host.reg_open_a(hKey, lpSubKey, ulOptions, samDesired, phkResult);
        RecordAdvapi("RegOpenKeyExA", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpSubKey; (void)ulOptions; (void)samDesired;
#endif
    RecordAdvapi("RegOpenKeyExA", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegQueryValueExW(
    void* hKey,
    const wchar_t* lpValueName,
    uint32_t* lpReserved,
    uint32_t* lpType,
    uint8_t* lpData,
    uint32_t* lpcbData) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_query_w) {
        const int32_t st = host.reg_query_w(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        RecordAdvapi("RegQueryValueExW", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpValueName; (void)lpReserved; (void)lpType; (void)lpData; (void)lpcbData;
#endif
    RecordAdvapi("RegQueryValueExW", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegQueryValueExA(
    void* hKey,
    const char* lpValueName,
    uint32_t* lpReserved,
    uint32_t* lpType,
    uint8_t* lpData,
    uint32_t* lpcbData) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_query_a) {
        const int32_t st = host.reg_query_a(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
        RecordAdvapi("RegQueryValueExA", st == 0, st);
        return st;
    }
#else
    (void)hKey; (void)lpValueName; (void)lpReserved; (void)lpType; (void)lpData; (void)lpcbData;
#endif
    RecordAdvapi("RegQueryValueExA", false, static_cast<int32_t>(kErrCallNotImpl));
    return static_cast<int32_t>(kErrCallNotImpl);
}

OS_DIAG_API int32_t OS_DIAG_CALL RegCloseKey(void* hKey) {
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.reg_close) {
        const int32_t st = host.reg_close(hKey);
        RecordAdvapi("RegCloseKey", st == 0, st);
        return st;
    }
#else
    (void)hKey;
#endif
    RecordAdvapi("RegCloseKey", true, 0);
    return static_cast<int32_t>(kErrSuccess);
}

OS_DIAG_API int32_t OS_DIAG_CALL GetUserNameW(wchar_t* lpBuffer, uint32_t* pcbBuffer) {
    if (!pcbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("GetUserNameW", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.get_user_w) {
        const int32_t ok = host.get_user_w(lpBuffer, pcbBuffer);
        if (ok != 0 || GetLastError() == kErrInsufficientBuf) {
            RecordAdvapi("GetUserNameW", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
            return ok;
        }
    }
#endif
    static constexpr wchar_t kFallbackUserW[] = L"OpenScarlettUser";
    constexpr uint32_t kNeeded = static_cast<uint32_t>(sizeof(kFallbackUserW) / sizeof(wchar_t));
    if (!lpBuffer || *pcbBuffer < kNeeded) {
        *pcbBuffer = kNeeded;
#if defined(_WIN32)
        SetLastError(kErrInsufficientBuf);
#endif
        RecordAdvapi("GetUserNameW", false, static_cast<int32_t>(kErrInsufficientBuf));
        return 0;
    }
    std::memcpy(lpBuffer, kFallbackUserW, sizeof(kFallbackUserW));
    *pcbBuffer = kNeeded;
    RecordAdvapi("GetUserNameW", true, 0);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL GetUserNameA(char* lpBuffer, uint32_t* pcbBuffer) {
    if (!pcbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("GetUserNameA", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.get_user_a) {
        const int32_t ok = host.get_user_a(lpBuffer, pcbBuffer);
        if (ok != 0 || GetLastError() == kErrInsufficientBuf) {
            RecordAdvapi("GetUserNameA", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
            return ok;
        }
    }
#endif
    static constexpr char kFallbackUserA[] = "OpenScarlettUser";
    constexpr uint32_t kNeeded = static_cast<uint32_t>(sizeof(kFallbackUserA));
    if (!lpBuffer || *pcbBuffer < kNeeded) {
        *pcbBuffer = kNeeded;
#if defined(_WIN32)
        SetLastError(kErrInsufficientBuf);
#endif
        RecordAdvapi("GetUserNameA", false, static_cast<int32_t>(kErrInsufficientBuf));
        return 0;
    }
    std::memcpy(lpBuffer, kFallbackUserA, sizeof(kFallbackUserA));
    *pcbBuffer = kNeeded;
    RecordAdvapi("GetUserNameA", true, 0);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptAcquireContextW(
    uintptr_t* phProv,
    const wchar_t* szContainer,
    const wchar_t* szProvider,
    uint32_t dwProvType,
    uint32_t dwFlags) {
    if (!phProv) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptAcquireContextW", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
    *phProv = 0u;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.crypt_acq_w) {
        if (host.crypt_acq_w(phProv, szContainer, szProvider, dwProvType, dwFlags) != 0) {
            RecordAdvapi("CryptAcquireContextW", true, 0);
            return 1;
        }
    }
    const uintptr_t local_h = host.next_local_prov.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.local_provs.insert(local_h);
    }
    *phProv = local_h;
    RecordAdvapi("CryptAcquireContextW", true, 0);
    return 1;
#else
    (void)szContainer; (void)szProvider; (void)dwProvType; (void)dwFlags;
    *phProv = 0x00A10001u;
    RecordAdvapi("CryptAcquireContextW", true, 0);
    return 1;
#endif
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptGenRandom(
    uintptr_t hProv,
    uint32_t dwLen,
    uint8_t* pbBuffer) {
    if (dwLen > 0u && !pbBuffer) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptGenRandom", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
    if (dwLen == 0u) {
        RecordAdvapi("CryptGenRandom", true, 0);
        return 1;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    bool is_local = false;
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        is_local = (host.local_provs.find(hProv) != host.local_provs.end());
    }
    if (!is_local && host.crypt_gen_random) {
        if (host.crypt_gen_random(hProv, dwLen, pbBuffer) != 0) {
            RecordAdvapi("CryptGenRandom", true, 0);
            return 1;
        }
    }
    host.FillRandomFallback(pbBuffer, dwLen);
    RecordAdvapi("CryptGenRandom", true, 0);
    return 1;
#else
    (void)hProv;
    std::memset(pbBuffer, 0x5A, dwLen);
    RecordAdvapi("CryptGenRandom", true, 0);
    return 1;
#endif
}

OS_DIAG_API int32_t OS_DIAG_CALL CryptReleaseContext(uintptr_t hProv, uint32_t dwFlags) {
    if (hProv == 0u) {
#if defined(_WIN32)
        SetLastError(kErrInvalidParam);
#endif
        RecordAdvapi("CryptReleaseContext", false, static_cast<int32_t>(kErrInvalidParam));
        return 0;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        if (host.local_provs.erase(hProv) > 0u) {
            RecordAdvapi("CryptReleaseContext", true, 0);
            return 1;
        }
    }
    if (host.crypt_rel) {
        const int32_t ok = host.crypt_rel(hProv, dwFlags);
        RecordAdvapi("CryptReleaseContext", ok != 0, ok != 0 ? 0 : static_cast<int32_t>(GetLastError()));
        return ok;
    }
#else
    (void)dwFlags;
#endif
    RecordAdvapi("CryptReleaseContext", true, 0);
    return 1;
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventRegister(
    const void* ProviderId,
    void* EnableCallback,
    void* CallbackContext,
    uint64_t* RegHandle) {
    if (!ProviderId || !RegHandle) {
        RecordAdvapi("EventRegister", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
    *RegHandle = 0ull;
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    if (host.event_reg) {
        const uint32_t st = host.event_reg(ProviderId, EnableCallback, CallbackContext, RegHandle);
        if (st == kErrSuccess) {
            RecordAdvapi("EventRegister", true, 0);
            return kErrSuccess;
        }
    }
    const uint64_t h = host.next_local_etw.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.local_etw_handles.insert(h);
    }
    *RegHandle = h;
    RecordAdvapi("EventRegister", true, 0);
    return kErrSuccess;
#else
    (void)EnableCallback; (void)CallbackContext;
    *RegHandle = 1ull;
    RecordAdvapi("EventRegister", true, 0);
    return kErrSuccess;
#endif
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventWrite(
    uint64_t RegHandle,
    const void* EventDescriptor,
    uint32_t UserDataCount,
    void* UserData) {
    if (RegHandle == 0ull || !EventDescriptor) {
        RecordAdvapi("EventWrite", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    bool is_local = false;
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        is_local = (host.local_etw_handles.find(RegHandle) != host.local_etw_handles.end());
    }
    if (!is_local && host.event_write) {
        const uint32_t st = host.event_write(RegHandle, EventDescriptor, UserDataCount, UserData);
        RecordAdvapi("EventWrite", st == kErrSuccess, static_cast<int32_t>(st));
        return st;
    }
#else
    (void)UserDataCount; (void)UserData;
#endif
    RecordAdvapi("EventWrite", true, 0);
    return kErrSuccess;
}

OS_DIAG_API uint32_t OS_DIAG_CALL EventUnregister(uint64_t RegHandle) {
    if (RegHandle == 0ull) {
        RecordAdvapi("EventUnregister", false, static_cast<int32_t>(kErrInvalidParam));
        return kErrInvalidParam;
    }
#if defined(_WIN32)
    auto& host = GetHostAdvapi();
    host.EnsureLoaded();
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        if (host.local_etw_handles.erase(RegHandle) > 0u) {
            RecordAdvapi("EventUnregister", true, 0);
            return kErrSuccess;
        }
    }
    if (host.event_unreg) {
        const uint32_t st = host.event_unreg(RegHandle);
        RecordAdvapi("EventUnregister", st == kErrSuccess, static_cast<int32_t>(st));
        return st;
    }
#endif
    RecordAdvapi("EventUnregister", true, 0);
    return kErrSuccess;
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettAdvapiLegacyGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kAdvapiMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 2: KernelTraceControl.dll (ETW / kernel trace session clean stubs)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_KERNELTRACECONTROL)

namespace {

constexpr const char* kKtcMod = "KernelTraceControl.dll";
constexpr uint32_t kErrSuccess = 0u;
constexpr uint32_t kErrInvalidHandle = 6u;
constexpr uint32_t kErrInvalidParam = 87u;

struct TraceSession {
    uint64_t handle = 0u;
    uint32_t process_id = 0u;
    uint32_t enabled_providers = 0u;
    uint32_t update_count = 0u;
};

struct KtcState {
    std::mutex mutex;
    uint64_t next_handle = 0x77AC0001ull;
    std::unordered_map<uint64_t, TraceSession> sessions;
};

KtcState& GetKtc() {
    static KtcState* s = new KtcState();
    return *s;
}

inline uint32_t RecordKtc(const char* sym, uint32_t err) {
    openscarlett::CallTelemetry::Global().Record(
        kKtcMod,
        sym,
        err == kErrSuccess ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        static_cast<int32_t>(err),
        -1);
    return err;
}

}  // namespace

extern "C" {

OS_DIAG_API uint32_t OS_DIAG_CALL StartKernelTrace(
    uint64_t* TraceHandle,
    void* Properties,
    const void* StackTracingEventIds,
    uint32_t cStackTracingEventIds) {
    (void)Properties;
    if (!TraceHandle || (cStackTracingEventIds > 0u && !StackTracingEventIds)) {
        return RecordKtc("StartKernelTrace", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t h = st.next_handle++;
    st.sessions[h] = TraceSession{h, 0u, 0u, 0u};
    *TraceHandle = h;
    return RecordKtc("StartKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL StartKernelTraceForProcess(
    uint64_t* TraceHandle,
    void* Properties,
    uint32_t ProcessId) {
    (void)Properties;
    if (!TraceHandle) {
        return RecordKtc("StartKernelTraceForProcess", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t h = st.next_handle++;
    st.sessions[h] = TraceSession{h, ProcessId, 0u, 0u};
    *TraceHandle = h;
    return RecordKtc("StartKernelTraceForProcess", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL StopKernelTrace(
    uint64_t TraceHandle,
    void* Properties) {
    (void)Properties;
    if (TraceHandle == 0ull) {
        return RecordKtc("StopKernelTrace", kErrInvalidHandle);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.sessions.erase(TraceHandle) == 0u) {
        return RecordKtc("StopKernelTrace", kErrInvalidHandle);
    }
    return RecordKtc("StopKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL UpdateKernelTrace(
    uint64_t TraceHandle,
    void* Properties) {
    (void)Properties;
    if (TraceHandle == 0ull) {
        return RecordKtc("UpdateKernelTrace", kErrInvalidHandle);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(TraceHandle);
    if (it == st.sessions.end()) {
        return RecordKtc("UpdateKernelTrace", kErrInvalidHandle);
    }
    ++it->second.update_count;
    return RecordKtc("UpdateKernelTrace", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL EnableTraceEx2(
    uint64_t TraceHandle,
    const void* ProviderId,
    uint32_t ControlCode,
    uint8_t Level,
    uint64_t MatchAnyKeyword,
    uint64_t MatchAllKeyword,
    uint32_t Timeout,
    void* EnableParameters) {
    (void)ControlCode; (void)Level; (void)MatchAnyKeyword;
    (void)MatchAllKeyword; (void)Timeout; (void)EnableParameters;
    if (TraceHandle == 0ull || !ProviderId) {
        return RecordKtc("EnableTraceEx2", kErrInvalidParam);
    }
    auto& st = GetKtc();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(TraceHandle);
    if (it != st.sessions.end()) {
        ++it->second.enabled_providers;
    }
    return RecordKtc("EnableTraceEx2", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL TraceQueryInformation(
    uint64_t SessionHandle,
    uint32_t InformationClass,
    void* TraceInformation,
    uint32_t InformationLength,
    uint32_t* ReturnLength) {
    (void)SessionHandle; (void)InformationClass;
    if (InformationLength > 0u && !TraceInformation) {
        return RecordKtc("TraceQueryInformation", kErrInvalidParam);
    }
    if (TraceInformation && InformationLength > 0u) {
        std::memset(TraceInformation, 0, InformationLength);
    }
    if (ReturnLength) {
        *ReturnLength = InformationLength;
    }
    return RecordKtc("TraceQueryInformation", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL TraceSetInformation(
    uint64_t SessionHandle,
    uint32_t InformationClass,
    void* TraceInformation,
    uint32_t InformationLength) {
    (void)SessionHandle; (void)InformationClass;
    if (InformationLength > 0u && !TraceInformation) {
        return RecordKtc("TraceSetInformation", kErrInvalidParam);
    }
    return RecordKtc("TraceSetInformation", kErrSuccess);
}

OS_DIAG_API uint32_t OS_DIAG_CALL CreateMergedTraceFile(
    const wchar_t* wszMergedFileName,
    const wchar_t** wszTraceFileNames,
    uint32_t cTraceFileNames,
    uint32_t dwExtendedDataFlags) {
    (void)dwExtendedDataFlags;
    if (!wszMergedFileName || !*wszMergedFileName ||
        (cTraceFileNames > 0u && !wszTraceFileNames)) {
        return RecordKtc("CreateMergedTraceFile", kErrInvalidParam);
    }
    return RecordKtc("CreateMergedTraceFile", kErrSuccess);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettKernelTraceGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kKtcMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 3: hix_xs.dll (Xbox Hang-Dump / Debug Instrumentation Forwards)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_HIX_XS)

namespace {

namespace fs = std::filesystem;
constexpr const char* kHixMod = "hix_xs.dll";

struct HixCallbackEntry {
    void* callback = nullptr;
    void* context = nullptr;
};

struct HixState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t flags = 0u;
    uint32_t hang_timeout_ms = 5000u;
    uint64_t heartbeats = 0u;
    uint64_t dump_requests = 0u;
    uint64_t next_cb_token = 1u;
    std::string last_heartbeat_tag;
    std::map<uint32_t, std::pair<std::string, uint64_t>> breadcrumbs;
    std::unordered_map<uint64_t, HixCallbackEntry> callbacks;
};

HixState& GetHix() {
    static HixState* s = new HixState();
    return *s;
}

inline int32_t RecordHix(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kHixMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

void ForwardBreadcrumbToXCrd(uint32_t slot, const char* tag, uint64_t value) {
#if defined(_WIN32)
    HMODULE crd = GetModuleHandleW(L"xcrdapi.dll");
    if (!crd) return;
    using PfnSetBc = int32_t(WINAPI*)(uint32_t, const char*, uint64_t);
    auto fn = reinterpret_cast<PfnSetBc>(
        reinterpret_cast<void*>(GetProcAddress(crd, "XCrdSetBreadcrumb")));
    if (fn) {
        (void)fn(slot, tag, value);
    }
#else
    (void)slot; (void)tag; (void)value;
#endif
}

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL HixInitialize(uint32_t flags) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.flags = flags;
    return RecordHix("HixInitialize", kHrOk);
}

OS_DIAG_API void OS_DIAG_CALL HixUninitialize(void) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    RecordHix("HixUninitialize", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixSetHangTimeoutMs(uint32_t timeoutMs) {
    if (timeoutMs == 0u) {
        return RecordHix("HixSetHangTimeoutMs", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.hang_timeout_ms = timeoutMs;
    return RecordHix("HixSetHangTimeoutMs", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixGetHangTimeoutMs(uint32_t* outTimeoutMs) {
    if (!outTimeoutMs) {
        return RecordHix("HixGetHangTimeoutMs", kHrPointer);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outTimeoutMs = st.hang_timeout_ms;
    return RecordHix("HixGetHangTimeoutMs", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixHeartbeat(const char* tag) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.heartbeats;
    if (tag) {
        st.last_heartbeat_tag = tag;
    }
    return RecordHix("HixHeartbeat", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRegisterHangCallback(
    void* callback,
    void* context,
    uint64_t* outToken) {
    if (!callback || !outToken) {
        return RecordHix("HixRegisterHangCallback", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t tok = st.next_cb_token++;
    st.callbacks[tok] = HixCallbackEntry{callback, context};
    *outToken = tok;
    return RecordHix("HixRegisterHangCallback", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixUnregisterHangCallback(uint64_t token) {
    if (token == 0ull) {
        return RecordHix("HixUnregisterHangCallback", kHrInvalidArg);
    }
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.callbacks.erase(token) == 0u) {
        return RecordHix("HixUnregisterHangCallback", kHrInvalidArg);
    }
    return RecordHix("HixUnregisterHangCallback", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRecordBreadcrumb(
    uint32_t slot,
    const char* tag,
    uint64_t value) {
    if (slot >= 256u || !tag || !*tag) {
        return RecordHix("HixRecordBreadcrumb", kHrInvalidArg);
    }
    {
        auto& st = GetHix();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.breadcrumbs[slot] = std::make_pair(std::string(tag), value);
    }
    ForwardBreadcrumbToXCrd(slot, tag, value);
    return RecordHix("HixRecordBreadcrumb", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixRequestHangDump(
    const char* reason,
    const char* optionalOutputPath) {
    if (!reason || !*reason) {
        return RecordHix("HixRequestHangDump", kHrInvalidArg);
    }
    std::ostringstream oss;
    {
        auto& st = GetHix();
        std::lock_guard<std::mutex> lk(st.mutex);
        ++st.dump_requests;
        oss << "{\n"
            << "  \"module\": \"hix_xs.dll\",\n"
            << "  \"reason\": \"" << reason << "\",\n"
            << "  \"timeout_ms\": " << st.hang_timeout_ms << ",\n"
            << "  \"heartbeats\": " << st.heartbeats << ",\n"
            << "  \"last_heartbeat\": \"" << st.last_heartbeat_tag << "\",\n"
            << "  \"dump_requests\": " << st.dump_requests << "\n"
            << "}\n";
    }
    if (optionalOutputPath && *optionalOutputPath) {
        fs::path out_path(optionalOutputPath);
        std::error_code ec;
        if (out_path.has_parent_path()) {
            fs::create_directories(out_path.parent_path(), ec);
        }
        std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return RecordHix("HixRequestHangDump", kHrFail);
        }
        const std::string body = oss.str();
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        ofs.flush();
        if (!ofs.good()) {
            return RecordHix("HixRequestHangDump", kHrFail);
        }
    }
    return RecordHix("HixRequestHangDump", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL HixGetStats(
    uint64_t* outHeartbeats,
    uint64_t* outDumpRequests) {
    auto& st = GetHix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (outHeartbeats) *outHeartbeats = st.heartbeats;
    if (outDumpRequests) *outDumpRequests = st.dump_requests;
    return RecordHix("HixGetStats", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettHixGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kHixMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 4: xbtp.dll (Xbox Binary Telemetry / Profiling Session Stubs)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_XBTP)

namespace {

constexpr const char* kXbtpMod = "xbtp.dll";

struct XbtpSession {
    std::string name;
    uint32_t flags = 0u;
    uint64_t events_written = 0u;
    uint64_t bytes_written = 0u;
    uint64_t flush_count = 0u;
};

struct XbtpState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t flags = 0u;
    uintptr_t next_handle = 0x7B790001u;
    std::unordered_map<uintptr_t, XbtpSession> sessions;
};

XbtpState& GetXbtp() {
    static XbtpState* s = new XbtpState();
    return *s;
}

inline int32_t RecordXbtp(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kXbtpMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL XbtpInitialize(uint32_t flags) {
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    st.flags = flags;
    return RecordXbtp("XbtpInitialize", kHrOk);
}

OS_DIAG_API void OS_DIAG_CALL XbtpShutdown(void) {
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    RecordXbtp("XbtpShutdown", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpOpenSession(
    const char* sessionName,
    uint32_t flags,
    void** outSession) {
    if (!outSession) {
        return RecordXbtp("XbtpOpenSession", kHrPointer);
    }
    *outSession = nullptr;
    if (!sessionName || !*sessionName) {
        return RecordXbtp("XbtpOpenSession", kHrInvalidArg);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t h = st.next_handle++;
    XbtpSession sess{};
    sess.name = sessionName;
    sess.flags = flags;
    st.sessions.emplace(h, std::move(sess));
    *outSession = reinterpret_cast<void*>(h);
    return RecordXbtp("XbtpOpenSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpCloseSession(void* session) {
    if (!session) {
        return RecordXbtp("XbtpCloseSession", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t key = reinterpret_cast<uintptr_t>(session);
    if (st.sessions.erase(key) == 0u) {
        return RecordXbtp("XbtpCloseSession", kHrHandle);
    }
    return RecordXbtp("XbtpCloseSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpWriteEvent(
    void* session,
    uint32_t eventId,
    const void* payload,
    uint32_t payloadBytes) {
    (void)eventId;
    if (!session) {
        return RecordXbtp("XbtpWriteEvent", kHrHandle);
    }
    if (payloadBytes > 0u && !payload) {
        return RecordXbtp("XbtpWriteEvent", kHrInvalidArg);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpWriteEvent", kHrHandle);
    }
    ++it->second.events_written;
    it->second.bytes_written += payloadBytes;
    return RecordXbtp("XbtpWriteEvent", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpFlushSession(void* session) {
    if (!session) {
        return RecordXbtp("XbtpFlushSession", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpFlushSession", kHrHandle);
    }
    ++it->second.flush_count;
    return RecordXbtp("XbtpFlushSession", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL XbtpGetSessionStats(
    void* session,
    uint64_t* outEventsWritten,
    uint64_t* outBytesWritten,
    uint64_t* outFlushCount) {
    if (!session) {
        return RecordXbtp("XbtpGetSessionStats", kHrHandle);
    }
    auto& st = GetXbtp();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.sessions.find(reinterpret_cast<uintptr_t>(session));
    if (it == st.sessions.end()) {
        return RecordXbtp("XbtpGetSessionStats", kHrHandle);
    }
    if (outEventsWritten) *outEventsWritten = it->second.events_written;
    if (outBytesWritten) *outBytesWritten = it->second.bytes_written;
    if (outFlushCount) *outFlushCount = it->second.flush_count;
    return RecordXbtp("XbtpGetSessionStats", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettXbtpGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kXbtpMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 5: wldp_xs.dll (Windows Lockdown Policy Watermark & Production Rows)
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_WLDP_XS)

namespace {

constexpr const char* kWldpMod = "wldp_xs.dll";
constexpr int32_t kHrInsufficientBuffer = static_cast<int32_t>(0x8007007Au);

// Windows WLDP constants (public wldp.h specification)
constexpr uint32_t kWldpLockdownModeUnlocked = 0u;
constexpr uint32_t kWldpLockdownRestrictionNone = 0u;
constexpr uint32_t kWldpLockdownDefinedFlag = 0x80000000u;  // Defined & off/non-enforced

constexpr uint32_t kWldpValueBoolean = 0u;
constexpr uint32_t kWldpValueInteger = 1u;
constexpr uint32_t kWldpValueString = 3u;

struct OsWldpUnicodeString {
    uint16_t Length;
    uint16_t MaximumLength;
    const wchar_t* Buffer;
};

inline int32_t RecordWldp(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kWldpMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

std::wstring LowerWldpUnicodeString(const OsWldpUnicodeString* us) {
    if (!us || !us->Buffer || us->Length == 0u) {
        return {};
    }
    const size_t chars = static_cast<size_t>(us->Length / sizeof(wchar_t));
    std::wstring out;
    out.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        const wchar_t ch = us->Buffer[i];
        if (ch == L'\0') break;
        out.push_back(static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch))));
    }
    return out;
}

bool ContainsAnyW(const std::wstring& s, const std::vector<const wchar_t*>& needles) {
    for (const wchar_t* n : needles) {
        if (s.find(n) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL WldpQueryDynamicCodeTrust(
    void* fileHandle,
    void* baseImage,
    uint32_t imageSize) {
    if (!fileHandle && (!baseImage || imageSize == 0u)) {
        return RecordWldp("WldpQueryDynamicCodeTrust", kHrInvalidArg);
    }
    return RecordWldp("WldpQueryDynamicCodeTrust", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL WldpIsClassInApprovedList(
    const void* classID,
    void* hostInformation,
    int32_t* isApproved,
    uint32_t optionalFlags) {
    (void)hostInformation;
    (void)optionalFlags;
    if (!classID || !isApproved) {
        return RecordWldp("WldpIsClassInApprovedList", kHrInvalidArg);
    }
    *isApproved = 1;
    return RecordWldp("WldpIsClassInApprovedList", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL WldpQueryWindowsLockdownMode(uint32_t* lockdownMode) {
    if (!lockdownMode) {
        return RecordWldp("WldpQueryWindowsLockdownMode", kHrInvalidArg);
    }
    *lockdownMode = kWldpLockdownModeUnlocked;
    return RecordWldp("WldpQueryWindowsLockdownMode", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL WldpQueryWindowsLockdownRestriction(
    uint32_t* lockdownRestriction) {
    if (!lockdownRestriction) {
        return RecordWldp("WldpQueryWindowsLockdownRestriction", kHrInvalidArg);
    }
    *lockdownRestriction = kWldpLockdownRestrictionNone;
    return RecordWldp("WldpQueryWindowsLockdownRestriction", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL WldpGetLockdownPolicy(
    const void* hostInformation,
    uint32_t* lockdownState,
    uint32_t lockdownFlags) {
    (void)hostInformation;
    (void)lockdownFlags;
    if (!lockdownState) {
        return RecordWldp("WldpGetLockdownPolicy", kHrInvalidArg);
    }
    *lockdownState = kWldpLockdownDefinedFlag;
    return RecordWldp("WldpGetLockdownPolicy", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL WldpIsProductionConfiguration(int32_t* isProductionConfig) {
    if (!isProductionConfig) {
        return RecordWldp("WldpIsProductionConfiguration", kHrInvalidArg);
    }
    *isProductionConfig = 1;
    return RecordWldp("WldpIsProductionConfiguration", kHrOk);
}

// Clean-room WLDP policy/watermark query rows matching Xbox OS / Windows security
// policy table semantics:
//   - Watermark / TestSigning / DevMode / Lockdown / UMCI / Audit -> Boolean 0 (disabled / unlocked)
//   - Production / Retail / Approved -> Boolean 1 (production-compatible retail config)
//   - Sandbox / Environment / Mode -> String L"RETAIL"
//   - Version / OsVersion -> String L"10.0.26100.2499"
//   - Unknown well-formed rows -> Integer 0 (non-locked baseline)
OS_DIAG_API int32_t OS_DIAG_CALL WldpQuerySecurityPolicy(
    const void* Provider,
    const void* Key,
    const void* ValueName,
    uint32_t* ValueType,
    void* Value,
    uint32_t* ValueSize) {
    const auto* prov = static_cast<const OsWldpUnicodeString*>(Provider);
    const auto* key = static_cast<const OsWldpUnicodeString*>(Key);
    const auto* val_name = static_cast<const OsWldpUnicodeString*>(ValueName);
    if (!prov || !key || !val_name || !ValueType || !ValueSize) {
        return RecordWldp("WldpQuerySecurityPolicy", kHrInvalidArg);
    }
    if ((prov->Length > 0u && !prov->Buffer) ||
        (key->Length > 0u && !key->Buffer) ||
        (val_name->Length > 0u && !val_name->Buffer)) {
        return RecordWldp("WldpQuerySecurityPolicy", kHrInvalidArg);
    }

    const std::wstring prov_lo = LowerWldpUnicodeString(prov);
    const std::wstring key_lo = LowerWldpUnicodeString(key);
    const std::wstring name_lo = LowerWldpUnicodeString(val_name);
    const std::wstring combined = prov_lo + L"/" + key_lo + L"/" + name_lo;

    uint32_t out_type = kWldpValueInteger;
    uint32_t int_val = 0u;
    std::wstring str_val;
    bool is_string = false;

    if (ContainsAnyW(name_lo, {L"watermark", L"testsigning", L"devmode", L"lockdown",
                               L"enforced", L"umci", L"audit", L"hvci", L"debug"})) {
        out_type = kWldpValueBoolean;
        int_val = 0u;  // No watermark, non-locked, non-enforced
    } else if (ContainsAnyW(name_lo, {L"production", L"retail", L"approved", L"allow"})) {
        out_type = kWldpValueBoolean;
        int_val = 1u;  // Production-compatible retail configuration
    } else if (ContainsAnyW(name_lo, {L"sandbox", L"environment", L"configuration"})) {
        out_type = kWldpValueString;
        str_val = L"RETAIL";
        is_string = true;
    } else if (ContainsAnyW(name_lo, {L"version"})) {
        out_type = kWldpValueString;
        str_val = L"10.0.26100.2499";
        is_string = true;
    } else if (ContainsAnyW(combined, {L"watermark", L"lockdown"})) {
        out_type = kWldpValueBoolean;
        int_val = 0u;
    } else {
        out_type = kWldpValueInteger;
        int_val = 0u;
    }

    *ValueType = out_type;
    if (is_string) {
        const uint32_t needed =
            static_cast<uint32_t>((str_val.size() + 1u) * sizeof(wchar_t));
        if (!Value || *ValueSize < needed) {
            *ValueSize = needed;
            return RecordWldp("WldpQuerySecurityPolicy", kHrInsufficientBuffer);
        }
        std::memcpy(Value, str_val.c_str(), needed);
        *ValueSize = needed;
        return RecordWldp("WldpQuerySecurityPolicy", kHrOk);
    }

    constexpr uint32_t kNeeded = static_cast<uint32_t>(sizeof(uint32_t));
    if (!Value || *ValueSize < kNeeded) {
        *ValueSize = kNeeded;
        return RecordWldp("WldpQuerySecurityPolicy", kHrInsufficientBuffer);
    }
    std::memcpy(Value, &int_val, kNeeded);
    *ValueSize = kNeeded;
    return RecordWldp("WldpQuerySecurityPolicy", kHrOk);
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettWldpGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kWldpMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 6: ntdll_ums_xs.dll (Clean-Room ntdll / Win32 UMS Helper Shims)
// Portable across MSVC, Clang-CL, MinGW-w64, Win10 & Win11 (where kernel UMS
// was removed). Exports both Win32 *Ums* and ntdll Rtl*Ums* NTSTATUS entry points.
// ===========================================================================
#elif defined(OPENSCARLETT_DIAG_TARGET_NTDLL_UMS_XS)

namespace {

constexpr const char* kUmsMod = "ntdll_ums_xs.dll";
constexpr uint32_t kUmsVersion = 0x0100u;
constexpr uint32_t kUmsReasonStartup = 0u;
constexpr uint32_t kUmsReasonYield = 2u;

constexpr uint32_t kUmsInfoUserContext = 1u;
constexpr uint32_t kUmsInfoPriority = 2u;
constexpr uint32_t kUmsInfoAffinity = 3u;
constexpr uint32_t kUmsInfoTeb = 4u;
constexpr uint32_t kUmsInfoIsSuspended = 5u;
constexpr uint32_t kUmsInfoIsTerminated = 6u;

constexpr int32_t kNtStatusSuccess = 0x00000000;
constexpr int32_t kNtStatusInvalidParameter = static_cast<int32_t>(0xC000000Du);
constexpr int32_t kNtStatusBufferTooSmall = static_cast<int32_t>(0xC0000023u);
constexpr int32_t kNtStatusTimeout = 0x00000102;
constexpr int32_t kNtStatusUnsuccessful = static_cast<int32_t>(0xC0000001u);

using PfnDiagUmsSchedProc = void(OS_DIAG_CALL*)(uint32_t, uintptr_t, void*);
using PfnDiagUmsWorkerProc = void(OS_DIAG_CALL*)(void*);

struct DiagUmsStartupInfo {
    uint32_t UmsVersion;
    void* CompletionList;
    PfnDiagUmsSchedProc SchedulerProc;
    void* SchedulerParam;
};

struct DiagUmsContext;

struct DiagUmsList {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<DiagUmsContext*> items;
#if defined(_WIN32)
    HANDLE event = nullptr;
#endif
};

struct DiagUmsContext {
    void* user_context = nullptr;
    int32_t priority = 0;
    uintptr_t affinity = 1u;
    uint64_t synthetic_teb[16]{};
    void* teb = nullptr;
    bool is_suspended = false;
    bool is_terminated = false;
    bool running = false;
    bool has_yielded = false;
    void* yield_param = nullptr;
    DiagUmsContext* next_list_item = nullptr;
    PfnDiagUmsWorkerProc worker_entry = nullptr;
    void* worker_param = nullptr;
#if defined(_WIN32)
    void* worker_fiber = nullptr;
    void* scheduler_fiber = nullptr;
#endif
};

struct DiagUmsTls {
    bool in_scheduler = false;
    DiagUmsList* completion_list = nullptr;
    PfnDiagUmsSchedProc scheduler_proc = nullptr;
    void* scheduler_param = nullptr;
    void* scheduler_fiber = nullptr;
    DiagUmsContext* current_worker = nullptr;
};

thread_local DiagUmsTls g_diag_ums_tls;

struct DiagUmsState {
    std::mutex mutex;
    std::unordered_map<void*, std::unique_ptr<DiagUmsList>> lists;
    std::unordered_map<void*, std::unique_ptr<DiagUmsContext>> contexts;

    bool ValidList(void* p) {
        std::lock_guard<std::mutex> lk(mutex);
        return lists.find(p) != lists.end();
    }

    bool ValidContext(void* p) {
        std::lock_guard<std::mutex> lk(mutex);
        return contexts.find(p) != contexts.end();
    }
};

DiagUmsState& GetDiagUms() {
    static DiagUmsState* s = new DiagUmsState();
    return *s;
}

inline void RecordUms(const char* sym, bool ok, int32_t code = 0) {
    openscarlett::CallTelemetry::Global().Record(
        kUmsMod,
        sym,
        ok ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        code,
        -1);
}

inline void SetUmsWinErr(uint32_t err) {
#if defined(_WIN32)
    ::SetLastError(static_cast<DWORD>(err));
#else
    (void)err;
#endif
}

void EnqueueDiagUmsItem(DiagUmsList* list, DiagUmsContext* ctx) {
    if (!list || !ctx) return;
    {
        std::lock_guard<std::mutex> lk(list->mutex);
        list->items.push_back(ctx);
#if defined(_WIN32)
        if (list->event) {
            ::SetEvent(list->event);
        }
#endif
    }
    list->cv.notify_all();
}

#if defined(_WIN32)
void WINAPI DiagUmsFiberTrampoline(PVOID param) {
    auto* ctx = static_cast<DiagUmsContext*>(param);
    if (ctx && ctx->worker_entry) {
        ctx->worker_entry(ctx->worker_param);
    }
    if (ctx) {
        ctx->is_terminated = true;
        ctx->has_yielded = false;
        if (ctx->scheduler_fiber) {
            ::SwitchToFiber(ctx->scheduler_fiber);
        }
    }
}
#endif

}  // namespace

extern "C" {

OS_DIAG_API int32_t OS_DIAG_CALL CreateUmsCompletionList(void** UmsCompletionList) {
    if (!UmsCompletionList) {
        SetUmsWinErr(87u);
        RecordUms("CreateUmsCompletionList", false, kHrInvalidArg);
        return 0;
    }
    *UmsCompletionList = nullptr;
    auto list = std::make_unique<DiagUmsList>();
#if defined(_WIN32)
    list->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!list->event) {
        SetUmsWinErr(8u);
        RecordUms("CreateUmsCompletionList", false, kHrFail);
        return 0;
    }
#endif
    void* raw = list.get();
    {
        auto& st = GetDiagUms();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.lists.emplace(raw, std::move(list));
    }
    *UmsCompletionList = raw;
    SetUmsWinErr(0u);
    RecordUms("CreateUmsCompletionList", true, kHrOk);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL DeleteUmsCompletionList(void* UmsCompletionList) {
    if (!UmsCompletionList) {
        SetUmsWinErr(87u);
        RecordUms("DeleteUmsCompletionList", false, kHrInvalidArg);
        return 0;
    }
    std::unique_ptr<DiagUmsList> owned;
    {
        auto& st = GetDiagUms();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.lists.find(UmsCompletionList);
        if (it == st.lists.end()) {
            SetUmsWinErr(87u);
            RecordUms("DeleteUmsCompletionList", false, kHrInvalidArg);
            return 0;
        }
        {
            std::lock_guard<std::mutex> llk(it->second->mutex);
            if (!it->second->items.empty()) {
                SetUmsWinErr(87u);
                RecordUms("DeleteUmsCompletionList", false, kHrInvalidArg);
                return 0;
            }
        }
        owned = std::move(it->second);
        st.lists.erase(it);
    }
#if defined(_WIN32)
    if (owned && owned->event) {
        ::CloseHandle(owned->event);
        owned->event = nullptr;
    }
#endif
    SetUmsWinErr(0u);
    RecordUms("DeleteUmsCompletionList", true, kHrOk);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL GetUmsCompletionListEvent(
    void* UmsCompletionList,
    void** UmsCompletionEvent) {
    if (!UmsCompletionList || !UmsCompletionEvent) {
        SetUmsWinErr(87u);
        RecordUms("GetUmsCompletionListEvent", false, kHrInvalidArg);
        return 0;
    }
    *UmsCompletionEvent = nullptr;
    auto& st = GetDiagUms();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.lists.find(UmsCompletionList);
    if (it == st.lists.end()) {
        SetUmsWinErr(87u);
        RecordUms("GetUmsCompletionListEvent", false, kHrInvalidArg);
        return 0;
    }
#if defined(_WIN32)
    *UmsCompletionEvent = static_cast<void*>(it->second->event);
#else
    *UmsCompletionEvent = it->second.get();
#endif
    SetUmsWinErr(0u);
    RecordUms("GetUmsCompletionListEvent", true, kHrOk);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL DequeueUmsCompletionListItems(
    void* UmsCompletionList,
    uint32_t WaitTimeOut,
    void** UmsThreadList) {
    if (!UmsCompletionList || !UmsThreadList) {
        SetUmsWinErr(87u);
        RecordUms("DequeueUmsCompletionListItems", false, kHrInvalidArg);
        return 0;
    }
    *UmsThreadList = nullptr;
    DiagUmsList* list = nullptr;
    {
        auto& st = GetDiagUms();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.lists.find(UmsCompletionList);
        if (it == st.lists.end()) {
            SetUmsWinErr(87u);
            RecordUms("DequeueUmsCompletionListItems", false, kHrInvalidArg);
            return 0;
        }
        list = it->second.get();
    }

    std::vector<DiagUmsContext*> drained;
    {
        std::unique_lock<std::mutex> llk(list->mutex);
        if (list->items.empty() && WaitTimeOut != 0u) {
            if (WaitTimeOut == 0xFFFFFFFFu) {
                list->cv.wait(llk, [&]() { return !list->items.empty(); });
            } else {
                (void)list->cv.wait_for(
                    llk, std::chrono::milliseconds(WaitTimeOut),
                    [&]() { return !list->items.empty(); });
            }
        }
        if (list->items.empty()) {
            if (WaitTimeOut == 0u) {
                SetUmsWinErr(0u);
                RecordUms("DequeueUmsCompletionListItems", true, kHrOk);
                return 1;
            }
            SetUmsWinErr(1460u);  // ERROR_TIMEOUT
            RecordUms("DequeueUmsCompletionListItems", false, kHrFail);
            return 0;
        }
        while (!list->items.empty()) {
            drained.push_back(list->items.front());
            list->items.pop_front();
        }
#if defined(_WIN32)
        if (list->event) {
            ::ResetEvent(list->event);
        }
#endif
    }

    for (size_t i = 0; i < drained.size(); ++i) {
        drained[i]->next_list_item = (i + 1u < drained.size()) ? drained[i + 1u] : nullptr;
    }
    *UmsThreadList = drained.front();
    SetUmsWinErr(0u);
    RecordUms("DequeueUmsCompletionListItems", true, kHrOk);
    return 1;
}

OS_DIAG_API void* OS_DIAG_CALL GetNextUmsListItem(void* UmsContext) {
    if (!UmsContext || !GetDiagUms().ValidContext(UmsContext)) {
        SetUmsWinErr(87u);
        RecordUms("GetNextUmsListItem", false, kHrInvalidArg);
        return nullptr;
    }
    SetUmsWinErr(0u);
    RecordUms("GetNextUmsListItem", true, kHrOk);
    return static_cast<DiagUmsContext*>(UmsContext)->next_list_item;
}

OS_DIAG_API int32_t OS_DIAG_CALL CreateUmsThreadContext(void** lpUmsThread) {
    if (!lpUmsThread) {
        SetUmsWinErr(87u);
        RecordUms("CreateUmsThreadContext", false, kHrInvalidArg);
        return 0;
    }
    *lpUmsThread = nullptr;
    auto ctx = std::make_unique<DiagUmsContext>();
    ctx->teb = ctx->synthetic_teb;
    void* raw = ctx.get();
    {
        auto& st = GetDiagUms();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.contexts.emplace(raw, std::move(ctx));
    }
    *lpUmsThread = raw;
    SetUmsWinErr(0u);
    RecordUms("CreateUmsThreadContext", true, kHrOk);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL DeleteUmsThreadContext(void* UmsThread) {
    if (!UmsThread) {
        SetUmsWinErr(87u);
        RecordUms("DeleteUmsThreadContext", false, kHrInvalidArg);
        return 0;
    }
    std::unique_ptr<DiagUmsContext> owned;
    {
        auto& st = GetDiagUms();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.contexts.find(UmsThread);
        if (it == st.contexts.end() || it->second->running) {
            SetUmsWinErr(87u);
            RecordUms("DeleteUmsThreadContext", false, kHrInvalidArg);
            return 0;
        }
        owned = std::move(it->second);
        st.contexts.erase(it);
    }
#if defined(_WIN32)
    if (owned && owned->worker_fiber) {
        ::DeleteFiber(owned->worker_fiber);
        owned->worker_fiber = nullptr;
    }
#endif
    SetUmsWinErr(0u);
    RecordUms("DeleteUmsThreadContext", true, kHrOk);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL QueryUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength,
    uint32_t* ReturnLength) {
    if (ReturnLength) *ReturnLength = 0u;
    if (!UmsThread || !GetDiagUms().ValidContext(UmsThread)) {
        SetUmsWinErr(87u);
        RecordUms("QueryUmsThreadInformation", false, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<DiagUmsContext*>(UmsThread);
    auto write_val = [&](const void* src, uint32_t needed) -> int32_t {
        if (ReturnLength) *ReturnLength = needed;
        if (!UmsThreadInformation || UmsThreadInformationLength < needed) {
            SetUmsWinErr(122u);
            RecordUms("QueryUmsThreadInformation", false, kHrInvalidArg);
            return 0;
        }
        std::memcpy(UmsThreadInformation, src, needed);
        SetUmsWinErr(0u);
        RecordUms("QueryUmsThreadInformation", true, kHrOk);
        return 1;
    };
    switch (UmsThreadInfoClass) {
        case kUmsInfoUserContext:
            return write_val(&ctx->user_context, static_cast<uint32_t>(sizeof(void*)));
        case kUmsInfoPriority:
            return write_val(&ctx->priority, static_cast<uint32_t>(sizeof(int32_t)));
        case kUmsInfoAffinity:
            return write_val(&ctx->affinity, static_cast<uint32_t>(sizeof(uintptr_t)));
        case kUmsInfoTeb:
            return write_val(&ctx->teb, static_cast<uint32_t>(sizeof(void*)));
        case kUmsInfoIsSuspended: {
            if (UmsThreadInformationLength == 1u && UmsThreadInformation) {
                const uint8_t b = ctx->is_suspended ? 1u : 0u;
                return write_val(&b, 1u);
            }
            const uint32_t v = ctx->is_suspended ? 1u : 0u;
            return write_val(&v, static_cast<uint32_t>(sizeof(uint32_t)));
        }
        case kUmsInfoIsTerminated: {
            if (UmsThreadInformationLength == 1u && UmsThreadInformation) {
                const uint8_t b = ctx->is_terminated ? 1u : 0u;
                return write_val(&b, 1u);
            }
            const uint32_t v = ctx->is_terminated ? 1u : 0u;
            return write_val(&v, static_cast<uint32_t>(sizeof(uint32_t)));
        }
        default:
            SetUmsWinErr(87u);
            RecordUms("QueryUmsThreadInformation", false, kHrInvalidArg);
            return 0;
    }
}

OS_DIAG_API int32_t OS_DIAG_CALL SetUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength) {
    if (!UmsThread || !UmsThreadInformation || !GetDiagUms().ValidContext(UmsThread)) {
        SetUmsWinErr(87u);
        RecordUms("SetUmsThreadInformation", false, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<DiagUmsContext*>(UmsThread);
    switch (UmsThreadInfoClass) {
        case kUmsInfoUserContext:
            if (UmsThreadInformationLength != sizeof(void*)) {
                SetUmsWinErr(87u);
                RecordUms("SetUmsThreadInformation", false, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->user_context, UmsThreadInformation, sizeof(void*));
            break;
        case kUmsInfoPriority:
            if (UmsThreadInformationLength != sizeof(int32_t)) {
                SetUmsWinErr(87u);
                RecordUms("SetUmsThreadInformation", false, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->priority, UmsThreadInformation, sizeof(int32_t));
            break;
        case kUmsInfoAffinity:
            if (UmsThreadInformationLength != sizeof(uintptr_t)) {
                SetUmsWinErr(87u);
                RecordUms("SetUmsThreadInformation", false, kHrInvalidArg);
                return 0;
            }
            std::memcpy(&ctx->affinity, UmsThreadInformation, sizeof(uintptr_t));
            break;
        default:
            SetUmsWinErr(87u);
            RecordUms("SetUmsThreadInformation", false, kHrInvalidArg);
            return 0;
    }
    SetUmsWinErr(0u);
    RecordUms("SetUmsThreadInformation", true, kHrOk);
    return 1;
}

OS_DIAG_API void* OS_DIAG_CALL GetCurrentUmsThread(void) {
    RecordUms("GetCurrentUmsThread", true, kHrOk);
    return g_diag_ums_tls.current_worker;
}

OS_DIAG_API int32_t OS_DIAG_CALL EnterUmsSchedulingMode(const void* SchedulerStartupInfo) {
    if (!SchedulerStartupInfo || g_diag_ums_tls.in_scheduler) {
        SetUmsWinErr(87u);
        RecordUms("EnterUmsSchedulingMode", false, kHrInvalidArg);
        return 0;
    }
    const auto* info = static_cast<const DiagUmsStartupInfo*>(SchedulerStartupInfo);
    if (info->UmsVersion != kUmsVersion ||
        !info->CompletionList ||
        !info->SchedulerProc ||
        !GetDiagUms().ValidList(info->CompletionList)) {
        SetUmsWinErr(87u);
        RecordUms("EnterUmsSchedulingMode", false, kHrInvalidArg);
        return 0;
    }
#if defined(_WIN32)
    const bool was_fiber = (::IsThreadAFiber() != FALSE);
    void* sched_fiber = was_fiber ? ::GetCurrentFiber() : ::ConvertThreadToFiber(nullptr);
    if (!sched_fiber) {
        SetUmsWinErr(8u);
        RecordUms("EnterUmsSchedulingMode", false, kHrFail);
        return 0;
    }
#else
    void* sched_fiber = nullptr;
#endif
    g_diag_ums_tls.in_scheduler = true;
    g_diag_ums_tls.completion_list = static_cast<DiagUmsList*>(info->CompletionList);
    g_diag_ums_tls.scheduler_proc = info->SchedulerProc;
    g_diag_ums_tls.scheduler_param = info->SchedulerParam;
    g_diag_ums_tls.scheduler_fiber = sched_fiber;
    g_diag_ums_tls.current_worker = nullptr;

    RecordUms("EnterUmsSchedulingMode", true, kHrOk);
    info->SchedulerProc(kUmsReasonStartup, 0u, info->SchedulerParam);

    g_diag_ums_tls.in_scheduler = false;
    g_diag_ums_tls.completion_list = nullptr;
    g_diag_ums_tls.scheduler_proc = nullptr;
    g_diag_ums_tls.scheduler_param = nullptr;
    g_diag_ums_tls.scheduler_fiber = nullptr;
    g_diag_ums_tls.current_worker = nullptr;
#if defined(_WIN32)
    if (!was_fiber) {
        (void)::ConvertFiberToThread();
    }
#endif
    SetUmsWinErr(0u);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL ExecuteUmsThread(void* UmsThread) {
    if (!UmsThread || !g_diag_ums_tls.in_scheduler || !GetDiagUms().ValidContext(UmsThread)) {
        SetUmsWinErr(87u);
        RecordUms("ExecuteUmsThread", false, kHrInvalidArg);
        return 0;
    }
    auto* ctx = static_cast<DiagUmsContext*>(UmsThread);
    if (ctx->is_terminated || ctx->running) {
        SetUmsWinErr(87u);
        RecordUms("ExecuteUmsThread", false, kHrInvalidArg);
        return 0;
    }
    RecordUms("ExecuteUmsThread", true, kHrOk);
    if (!ctx->worker_entry) {
        ctx->is_terminated = true;
        EnqueueDiagUmsItem(g_diag_ums_tls.completion_list, ctx);
        SetUmsWinErr(0u);
        return 1;
    }
#if defined(_WIN32)
    if (!ctx->worker_fiber) {
        ctx->worker_fiber = ::CreateFiber(0u, &DiagUmsFiberTrampoline, ctx);
        if (!ctx->worker_fiber) {
            SetUmsWinErr(8u);
            return 0;
        }
    }
    ctx->scheduler_fiber = g_diag_ums_tls.scheduler_fiber;
    ctx->running = true;
    ctx->has_yielded = false;
    ctx->yield_param = nullptr;
    DiagUmsContext* prev = g_diag_ums_tls.current_worker;
    g_diag_ums_tls.current_worker = ctx;

    ::SwitchToFiber(ctx->worker_fiber);

    g_diag_ums_tls.current_worker = prev;
    ctx->running = false;

    if (ctx->has_yielded) {
        EnqueueDiagUmsItem(g_diag_ums_tls.completion_list, ctx);
        if (g_diag_ums_tls.scheduler_proc) {
            g_diag_ums_tls.scheduler_proc(
                kUmsReasonYield,
                reinterpret_cast<uintptr_t>(ctx),
                ctx->yield_param);
        }
    } else if (ctx->is_terminated) {
        if (ctx->worker_fiber) {
            ::DeleteFiber(ctx->worker_fiber);
            ctx->worker_fiber = nullptr;
        }
        EnqueueDiagUmsItem(g_diag_ums_tls.completion_list, ctx);
    }
#else
    ctx->running = true;
    DiagUmsContext* prev = g_diag_ums_tls.current_worker;
    g_diag_ums_tls.current_worker = ctx;
    ctx->worker_entry(ctx->worker_param);
    g_diag_ums_tls.current_worker = prev;
    ctx->running = false;
    ctx->is_terminated = true;
    EnqueueDiagUmsItem(g_diag_ums_tls.completion_list, ctx);
#endif
    SetUmsWinErr(0u);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL UmsThreadYield(void* SchedulerParam) {
    DiagUmsContext* ctx = g_diag_ums_tls.current_worker;
    if (!ctx) {
        SetUmsWinErr(1u);
        RecordUms("UmsThreadYield", false, kHrInvalidArg);
        return 0;
    }
    RecordUms("UmsThreadYield", true, kHrOk);
#if defined(_WIN32)
    if (!ctx->scheduler_fiber) {
        SetUmsWinErr(1u);
        return 0;
    }
    ctx->has_yielded = true;
    ctx->yield_param = SchedulerParam;
    ::SwitchToFiber(ctx->scheduler_fiber);
    SetUmsWinErr(0u);
    return 1;
#else
    (void)SchedulerParam;
    SetUmsWinErr(0u);
    return 1;
#endif
}

// ntdll Rtl*Ums* NTSTATUS wrappers over the clean-room cooperative UMS state
OS_DIAG_API int32_t OS_DIAG_CALL RtlCreateUmsCompletionList(void** UmsCompletionList) {
    return CreateUmsCompletionList(UmsCompletionList) ? kNtStatusSuccess : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlDeleteUmsCompletionList(void* UmsCompletionList) {
    return DeleteUmsCompletionList(UmsCompletionList) ? kNtStatusSuccess : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlGetUmsCompletionListEvent(
    void* UmsCompletionList,
    void** UmsCompletionEvent) {
    return GetUmsCompletionListEvent(UmsCompletionList, UmsCompletionEvent)
               ? kNtStatusSuccess
               : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlDequeueUmsCompletionListItems(
    void* UmsCompletionList,
    uint32_t WaitTimeOut,
    void** UmsThreadList) {
    if (DequeueUmsCompletionListItems(UmsCompletionList, WaitTimeOut, UmsThreadList)) {
        return kNtStatusSuccess;
    }
#if defined(_WIN32)
    if (::GetLastError() == 1460u) {
        return kNtStatusTimeout;
    }
#endif
    return kNtStatusInvalidParameter;
}

OS_DIAG_API void* OS_DIAG_CALL RtlGetNextUmsListItem(void* UmsContext) {
    return GetNextUmsListItem(UmsContext);
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlCreateUmsThreadContext(void** lpUmsThread) {
    return CreateUmsThreadContext(lpUmsThread) ? kNtStatusSuccess : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlDeleteUmsThreadContext(void* UmsThread) {
    return DeleteUmsThreadContext(UmsThread) ? kNtStatusSuccess : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlQueryUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength,
    uint32_t* ReturnLength) {
    if (QueryUmsThreadInformation(
            UmsThread, UmsThreadInfoClass, UmsThreadInformation,
            UmsThreadInformationLength, ReturnLength)) {
        return kNtStatusSuccess;
    }
#if defined(_WIN32)
    if (::GetLastError() == 122u) {
        return kNtStatusBufferTooSmall;
    }
#endif
    return kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlSetUmsThreadInformation(
    void* UmsThread,
    uint32_t UmsThreadInfoClass,
    void* UmsThreadInformation,
    uint32_t UmsThreadInformationLength) {
    return SetUmsThreadInformation(
               UmsThread, UmsThreadInfoClass, UmsThreadInformation, UmsThreadInformationLength)
               ? kNtStatusSuccess
               : kNtStatusInvalidParameter;
}

OS_DIAG_API void* OS_DIAG_CALL RtlGetCurrentUmsThread(void) {
    return GetCurrentUmsThread();
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlEnterUmsSchedulingMode(const void* SchedulerStartupInfo) {
    return EnterUmsSchedulingMode(SchedulerStartupInfo) ? kNtStatusSuccess : kNtStatusInvalidParameter;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlExecuteUmsThread(void* UmsThread) {
    return ExecuteUmsThread(UmsThread) ? kNtStatusSuccess : kNtStatusUnsuccessful;
}

OS_DIAG_API int32_t OS_DIAG_CALL RtlUmsThreadYield(void* SchedulerParam) {
    return UmsThreadYield(SchedulerParam) ? kNtStatusSuccess : kNtStatusUnsuccessful;
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettUmsBindWorkerProc(
    void* UmsThread,
    void* EntryProc,
    void* EntryParam) {
    if (!UmsThread || !EntryProc || !GetDiagUms().ValidContext(UmsThread)) {
        SetUmsWinErr(87u);
        return 0;
    }
    auto* ctx = static_cast<DiagUmsContext*>(UmsThread);
    if (ctx->running || ctx->is_terminated) {
        SetUmsWinErr(87u);
        return 0;
    }
    ctx->worker_entry = reinterpret_cast<PfnDiagUmsWorkerProc>(EntryProc);
    ctx->worker_param = EntryParam;
    SetUmsWinErr(0u);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettUmsEnqueueCompletionItem(
    void* UmsCompletionList,
    void* UmsThread) {
    if (!UmsCompletionList || !UmsThread ||
        !GetDiagUms().ValidList(UmsCompletionList) ||
        !GetDiagUms().ValidContext(UmsThread)) {
        SetUmsWinErr(87u);
        return 0;
    }
    EnqueueDiagUmsItem(
        static_cast<DiagUmsList*>(UmsCompletionList),
        static_cast<DiagUmsContext*>(UmsThread));
    SetUmsWinErr(0u);
    return 1;
}

OS_DIAG_API int32_t OS_DIAG_CALL OpenScarlettUmsGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    return QueryModuleTelemetry(kUmsMod, symbolName, outTotalCalls, outOkCalls);
}

}  // extern "C"

#else
#  error "Define one of OPENSCARLETT_DIAG_TARGET_{ADVAPI32LEGACY,KERNELTRACECONTROL,HIX_XS,XBTP,WLDP_XS,NTDLL_UMS_XS}"
#endif
