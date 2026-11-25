// MIT Functional Diagnostic & Platform Shims for OpenScarlett.
// Compiled into 4 standalone DLL targets via preprocessor target macros:
//   1. OPENSCARLETT_S7_TARGET_GAMECORECLIENT   -> gamecoreclient.dll
//   2. OPENSCARLETT_S7_TARGET_MINUSERX         -> minuserx.dll
//   3. OPENSCARLETT_S7_TARGET_XTFPIX           -> xtfpix.dll
//   4. OPENSCARLETT_S7_TARGET_XTFDEBUGMONITOR  -> xtfdebugmonitor.dll
// (mflnmhelp.dll and mfhevcdec.dll live in dedicated mflnmhelp_shim.cpp & mfhevcdec_shim.cpp.)
//
// Every export logs via openscarlett::CallTelemetry (with not_impl = 0) and stays
// portable and 0-warning clean across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(_WIN32)
#  define OS_S7_API __declspec(dllexport)
#  define OS_S7_CALL __stdcall
#  if defined(_MSC_VER)
#    define OS_S7_COM_API
#  else
#    define OS_S7_COM_API __declspec(dllexport)
#  endif
#else
#  define OS_S7_API __attribute__((visibility("default")))
#  define OS_S7_COM_API __attribute__((visibility("default")))
#  define OS_S7_CALL
#endif

namespace {

[[maybe_unused]] constexpr int32_t kHrOk = 0x00000000;
[[maybe_unused]] constexpr int32_t kHrFalse = 0x00000001;
[[maybe_unused]] constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
[[maybe_unused]] constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
[[maybe_unused]] constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
[[maybe_unused]] constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
[[maybe_unused]] constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
[[maybe_unused]] constexpr int32_t kHrInsufficientBuffer = static_cast<int32_t>(0x8007007Au);
[[maybe_unused]] constexpr int32_t kHrClassNoAggregation = static_cast<int32_t>(0x80040110u);

[[maybe_unused]] uint64_t S7QueryQpcTicks() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER li{};
    ::QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

[[maybe_unused]] uint64_t S7QueryQpcFreq() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER li{};
    ::QueryPerformanceFrequency(&li);
    return li.QuadPart > 0 ? static_cast<uint64_t>(li.QuadPart) : 10000000ull;
#else
    return 1000000000ull;
#endif
}

[[maybe_unused]] uint32_t S7CurrentThreadId() noexcept {
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetCurrentThreadId());
#else
    return 1u;
#endif
}

[[maybe_unused]] std::string S7EscapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8u);
    for (unsigned char c : in) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"': out.append("\\\""); break;
            case '\r': out.append("\\r"); break;
            case '\n': out.append("\\n"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

[[maybe_unused]] void CopyBoundedUtf8(const std::string& src, char* dst, uint32_t cap) noexcept {
    if (!dst || cap == 0u) return;
    const size_t n = std::min<size_t>(src.size(), static_cast<size_t>(cap - 1u));
    if (n > 0u) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

int32_t QueryS7Telemetry(const char* module_name,
                         const char* symbol_name,
                         uint64_t* out_total,
                         uint64_t* out_ok,
                         uint64_t* out_not_impl) {
    if (out_total) *out_total = 0u;
    if (out_ok) *out_ok = 0u;
    if (out_not_impl) *out_not_impl = 0u;
    if (!symbol_name) {
        return 0;
    }
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == module_name && entry.symbol_name == symbol_name) {
            if (out_total) *out_total = entry.total_calls;
            if (out_ok) *out_ok = entry.ok_calls;
            if (out_not_impl) *out_not_impl = entry.not_impl_calls;
            return 1;
        }
    }
    return 0;
}

int32_t QueryS7ModuleTotals(const char* module_name,
                            uint64_t* out_total_calls,
                            uint64_t* out_not_impl_calls) {
    uint64_t total = 0u;
    uint64_t not_impl = 0u;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == module_name) {
            total += entry.total_calls;
            not_impl += entry.not_impl_calls;
        }
    }
    if (out_total_calls) *out_total_calls = total;
    if (out_not_impl_calls) *out_not_impl_calls = not_impl;
    return 1;
}

}  // namespace

// ===========================================================================
// TARGET 1: gamecoreclient.dll — GameCore Package / Lifecycle / PLM Broker
// ===========================================================================
#if defined(OPENSCARLETT_S7_TARGET_GAMECORECLIENT)

namespace {

constexpr const char* kMod = "gamecoreclient.dll";

// PLM states:
//   1 = Running, 2 = Constrained, 3 = Suspended, 4 = Resumed, 5 = Terminating
constexpr uint32_t kPlmStateRunning = 1u;
constexpr uint32_t kPlmStateTerminating = 5u;

struct LifecycleSubscriber {
    void* callback = nullptr;
    void* context = nullptr;
};

struct GameCoreSessionEntry {
    std::string app_id;
    uint64_t opened_qpc = 0u;
};

struct GameCoreClientState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t plm_state = kPlmStateRunning;
    uint32_t lifecycle_events = 0u;
    uintptr_t next_session = 0x6C0C0001u;
    uint64_t next_cb_token = 1u;
    std::string app_id = "OpenScarlett.App";
    std::string package_family_name = "OpenScarlett.Default_8wekyb3d8bbwe";
    std::string sandbox_id = "RETAIL";
    std::unordered_map<uintptr_t, GameCoreSessionEntry> sessions;
    std::unordered_map<uint64_t, LifecycleSubscriber> callbacks;
};

GameCoreClientState& GetGcClient() {
    static GameCoreClientState* s = new GameCoreClientState();
    return *s;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

}  // namespace

extern "C" {

OS_S7_API int32_t OS_S7_CALL GameCoreClientInitialize(uint32_t flags) {
    (void)flags;
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count == 0u) {
        st.plm_state = kPlmStateRunning;
    }
    ++st.init_count;
    return RecordMod("GameCoreClientInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientShutdown(void) {
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    return RecordMod("GameCoreClientShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientOpenSession(const char* appId, void** outSession) {
    if (!outSession) {
        return RecordMod("GameCoreClientOpenSession", kHrPointer);
    }
    *outSession = nullptr;
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t h = st.next_session++;
    GameCoreSessionEntry entry{};
    entry.app_id = (appId && *appId) ? std::string(appId) : st.app_id;
    entry.opened_qpc = S7QueryQpcTicks();
    if (appId && *appId) {
        st.app_id = entry.app_id;
    }
    st.sessions.emplace(h, std::move(entry));
    *outSession = reinterpret_cast<void*>(h);
    return RecordMod("GameCoreClientOpenSession", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientCloseSession(void* session) {
    if (!session) {
        return RecordMod("GameCoreClientCloseSession", kHrHandle);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.sessions.erase(reinterpret_cast<uintptr_t>(session)) == 0u) {
        return RecordMod("GameCoreClientCloseSession", kHrHandle);
    }
    return RecordMod("GameCoreClientCloseSession", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientGetServiceState(uint32_t* outState) {
    if (!outState) {
        return RecordMod("GameCoreClientGetServiceState", kHrPointer);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outState = st.plm_state;
    return RecordMod("GameCoreClientGetServiceState", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientNotifyLifecycleEvent(uint32_t eventId) {
    uint32_t prev_state = kPlmStateRunning;
    uint32_t new_state = kPlmStateRunning;
    std::vector<LifecycleSubscriber> subs;
    {
        auto& st = GetGcClient();
        std::lock_guard<std::mutex> lk(st.mutex);
        ++st.lifecycle_events;
        prev_state = st.plm_state;
        if (eventId >= kPlmStateRunning && eventId <= kPlmStateTerminating) {
            st.plm_state = eventId;
        }
        new_state = st.plm_state;
        subs.reserve(st.callbacks.size());
        for (const auto& kv : st.callbacks) {
            subs.push_back(kv.second);
        }
    }
    using PfnLifecycleCb = void(OS_S7_CALL*)(uint32_t prevState, uint32_t newState, void* context);
    for (const auto& sub : subs) {
        if (sub.callback) {
            auto fn = reinterpret_cast<PfnLifecycleCb>(sub.callback);
            fn(prev_state, new_state, sub.context);
        }
    }
    return RecordMod("GameCoreClientNotifyLifecycleEvent", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientRegisterLifecycleCallback(
    void* callback,
    void* context,
    uint64_t* outToken) {
    if (!callback || !outToken) {
        return RecordMod("GameCoreClientRegisterLifecycleCallback", kHrInvalidArg);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint64_t tok = st.next_cb_token++;
    st.callbacks[tok] = LifecycleSubscriber{callback, context};
    *outToken = tok;
    return RecordMod("GameCoreClientRegisterLifecycleCallback", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientUnregisterLifecycleCallback(uint64_t token) {
    if (token == 0ull) {
        return RecordMod("GameCoreClientUnregisterLifecycleCallback", kHrInvalidArg);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.callbacks.erase(token) == 0u) {
        return RecordMod("GameCoreClientUnregisterLifecycleCallback", kHrInvalidArg);
    }
    return RecordMod("GameCoreClientUnregisterLifecycleCallback", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientSetPackageIdentity(
    const char* appId,
    const char* packageFamilyName,
    const char* sandboxId) {
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (appId && *appId) st.app_id = appId;
    if (packageFamilyName && *packageFamilyName) st.package_family_name = packageFamilyName;
    if (sandboxId && *sandboxId) st.sandbox_id = sandboxId;
    return RecordMod("GameCoreClientSetPackageIdentity", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientGetPackageIdentity(
    char* outAppId,
    uint32_t appIdCap,
    char* outPackageFamilyName,
    uint32_t pfnCap,
    char* outSandboxId,
    uint32_t sandboxCap) {
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    CopyBoundedUtf8(st.app_id, outAppId, appIdCap);
    CopyBoundedUtf8(st.package_family_name, outPackageFamilyName, pfnCap);
    CopyBoundedUtf8(st.sandbox_id, outSandboxId, sandboxCap);
    return RecordMod("GameCoreClientGetPackageIdentity", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientGetActiveSessionCount(uint32_t* outCount) {
    if (!outCount) {
        return RecordMod("GameCoreClientGetActiveSessionCount", kHrPointer);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outCount = static_cast<uint32_t>(st.sessions.size());
    return RecordMod("GameCoreClientGetActiveSessionCount", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientGetLifecycleEventCount(uint32_t* outCount) {
    if (!outCount) {
        return RecordMod("GameCoreClientGetLifecycleEventCount", kHrPointer);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outCount = st.lifecycle_events;
    return RecordMod("GameCoreClientGetLifecycleEventCount", kHrOk);
}

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#endif
OS_S7_COM_API int32_t OS_S7_CALL DllCanUnloadNow(void) {
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    const bool busy = (st.init_count > 0u) || !st.sessions.empty() || !st.callbacks.empty();
    return RecordMod("DllCanUnloadNow", busy ? kHrFalse : kHrOk);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettGameCoreClientGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryS7ModuleTotals(kMod, outTotalCalls, outNotImplCalls);
    }
    return QueryS7Telemetry(kMod, symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 4: minuserx.dll — Real Win32 User/Window/Message/DPI Forwarding Shim
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_MINUSERX)

namespace {

constexpr const char* kMod = "minuserx.dll";

struct MinUserXState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    int32_t cursor_visible = 1;
    bool cursor_override = false;
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    bool wnd_class_registered = false;
    std::unordered_set<uintptr_t> owned_windows;
};

MinUserXState& GetMinUserX() {
    static MinUserXState* s = new MinUserXState();
    return *s;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

#if defined(_WIN32)
constexpr const wchar_t* kMinUserXClassName = L"OpenScarlettMinUserXWindowClass";

LRESULT CALLBACK MinUserXWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (hdc) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            ::FillRect(hdc, &rc, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        }
        return 1;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureMinUserXClassRegisteredLocked(MinUserXState& st) {
    if (st.wnd_class_registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MinUserXWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kMinUserXClassName;
    if (::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        st.wnd_class_registered = true;
        return true;
    }
    return false;
}

uint32_t QueryHostDpiScalePercent() noexcept {
    UINT dpi = 0u;
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (u32) {
        using PfnGetDpiForSystem = UINT(WINAPI*)(void);
        auto fn = reinterpret_cast<PfnGetDpiForSystem>(
            reinterpret_cast<void*>(::GetProcAddress(u32, "GetDpiForSystem")));
        if (fn) {
            dpi = fn();
        }
    }
    if (dpi == 0u) {
        HDC hdc = ::GetDC(nullptr);
        if (hdc) {
            const int log_x = ::GetDeviceCaps(hdc, LOGPIXELSX);
            ::ReleaseDC(nullptr, hdc);
            if (log_x > 0) {
                dpi = static_cast<UINT>(log_x);
            }
        }
    }
    if (dpi == 0u) {
        return 100u;
    }
    uint32_t pct = static_cast<uint32_t>((dpi * 100u + 48u) / 96u);
    if (pct < 100u) pct = 100u;
    if (pct > 500u) pct = 500u;
    return pct;
}
#endif

}  // namespace

extern "C" {

OS_S7_API int32_t OS_S7_CALL MinUserXInitialize(uint32_t flags) {
    (void)flags;
    auto& st = GetMinUserX();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordMod("MinUserXInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXShutdown(void) {
    auto& st = GetMinUserX();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    return RecordMod("MinUserXShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetSystemMetrics(int32_t nIndex) {
    RecordMod("MinUserXGetSystemMetrics", kHrOk);
#if defined(_WIN32)
    if (nIndex == 0 || nIndex == 1) {
        char rbuf[64]{};
        const DWORD rlen = ::GetEnvironmentVariableA("OPENSCARLETT_DISPLAY_RES", rbuf, sizeof(rbuf));
        if (rlen > 0u && rlen < sizeof(rbuf)) {
            char* sep = nullptr;
            const unsigned long ew = std::strtoul(rbuf, &sep, 10);
            if (sep && (*sep == 'x' || *sep == 'X')) {
                const unsigned long eh = std::strtoul(sep + 1, nullptr, 10);
                if (ew >= 320ul && eh >= 180ul && ew <= 7680ul && eh <= 4320ul) {
                    return static_cast<int32_t>(nIndex == 0 ? ew : eh);
                }
            }
        }
    }
    const int val = ::GetSystemMetrics(nIndex);
    if (val != 0) {
        return static_cast<int32_t>(val);
    }
#endif
    if (nIndex == 0) return 1920;  // SM_CXSCREEN
    if (nIndex == 1) return 1080;  // SM_CYSCREEN
    return 0;
}

OS_S7_API void* OS_S7_CALL MinUserXGetForegroundWindow(void) {
    RecordMod("MinUserXGetForegroundWindow", kHrOk);
#if defined(_WIN32)
    return static_cast<void*>(::GetForegroundWindow());
#else
    return nullptr;
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXSetCursorVisible(int32_t visible) {
    RecordMod("MinUserXSetCursorVisible", kHrOk);
    auto& st = GetMinUserX();
    std::lock_guard<std::mutex> lk(st.mutex);
    const int32_t prev = st.cursor_visible;
    st.cursor_visible = visible ? 1 : 0;
    return prev;
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetDisplayScaleFactor(uint32_t* outScalePercent) {
    if (!outScalePercent) {
        return RecordMod("MinUserXGetDisplayScaleFactor", kHrPointer);
    }
#if defined(_WIN32)
    *outScalePercent = QueryHostDpiScalePercent();
#else
    *outScalePercent = 100u;
#endif
    return RecordMod("MinUserXGetDisplayScaleFactor", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXCreateWindow(
    const wchar_t* title,
    int32_t width,
    int32_t height,
    uint32_t flags,
    void** outHwnd) {
    if (!outHwnd) {
        return RecordMod("MinUserXCreateWindow", kHrPointer);
    }
    *outHwnd = nullptr;
    if (width <= 0 || height <= 0) {
        return RecordMod("MinUserXCreateWindow", kHrInvalidArg);
    }
#if defined(_WIN32)
    {
        auto& st = GetMinUserX();
        std::lock_guard<std::mutex> lk(st.mutex);
        if (!EnsureMinUserXClassRegisteredLocked(st)) {
            return RecordMod("MinUserXCreateWindow", kHrFail);
        }
    }
    const DWORD style = (flags & 1u) ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
    RECT rc{0, 0, width, height};
    ::AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = ::CreateWindowExW(
        0u,
        kMinUserXClassName,
        title ? title : L"OpenScarlett MinUserX",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        ::GetModuleHandleW(nullptr),
        nullptr);
    if (!hwnd) {
        return RecordMod("MinUserXCreateWindow", kHrFail);
    }
    {
        auto& st = GetMinUserX();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.owned_windows.insert(reinterpret_cast<uintptr_t>(hwnd));
    }
    *outHwnd = static_cast<void*>(hwnd);
    return RecordMod("MinUserXCreateWindow", kHrOk);
#else
    (void)title;
    (void)flags;
    return RecordMod("MinUserXCreateWindow", kHrFail);
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXDestroyWindow(void* hwnd) {
    if (!hwnd) {
        return RecordMod("MinUserXDestroyWindow", kHrHandle);
    }
#if defined(_WIN32)
    {
        auto& st = GetMinUserX();
        std::lock_guard<std::mutex> lk(st.mutex);
        st.owned_windows.erase(reinterpret_cast<uintptr_t>(hwnd));
    }
    if (!::DestroyWindow(static_cast<HWND>(hwnd))) {
        return RecordMod("MinUserXDestroyWindow", kHrFail);
    }
    return RecordMod("MinUserXDestroyWindow", kHrOk);
#else
    return RecordMod("MinUserXDestroyWindow", kHrFail);
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXPostMessage(
    void* hwnd,
    uint32_t msg,
    uintptr_t wParam,
    intptr_t lParam) {
    if (!hwnd) {
        return RecordMod("MinUserXPostMessage", kHrHandle);
    }
#if defined(_WIN32)
    if (!::PostMessageW(static_cast<HWND>(hwnd),
                        static_cast<UINT>(msg),
                        static_cast<WPARAM>(wParam),
                        static_cast<LPARAM>(lParam))) {
        return RecordMod("MinUserXPostMessage", kHrFail);
    }
    return RecordMod("MinUserXPostMessage", kHrOk);
#else
    (void)msg; (void)wParam; (void)lParam;
    return RecordMod("MinUserXPostMessage", kHrFail);
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXPeekMessage(
    void* hwnd,
    uint32_t* outMsg,
    uintptr_t* outWParam,
    intptr_t* outLParam,
    uint32_t removeMsg) {
    if (outMsg) *outMsg = 0u;
    if (outWParam) *outWParam = 0u;
    if (outLParam) *outLParam = 0;
#if defined(_WIN32)
    MSG m{};
    const BOOL got = ::PeekMessageW(
        &m,
        static_cast<HWND>(hwnd),
        0u,
        0u,
        removeMsg ? PM_REMOVE : PM_NOREMOVE);
    RecordMod("MinUserXPeekMessage", kHrOk);
    if (!got) {
        return 0;
    }
    if (outMsg) *outMsg = static_cast<uint32_t>(m.message);
    if (outWParam) *outWParam = static_cast<uintptr_t>(m.wParam);
    if (outLParam) *outLParam = static_cast<intptr_t>(m.lParam);
    return 1;
#else
    (void)hwnd; (void)removeMsg;
    RecordMod("MinUserXPeekMessage", kHrOk);
    return 0;
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXDispatchMessage(
    void* hwnd,
    uint32_t msg,
    uintptr_t wParam,
    intptr_t lParam) {
    if (!hwnd) {
        return RecordMod("MinUserXDispatchMessage", kHrHandle);
    }
#if defined(_WIN32)
    (void)::SendMessageW(static_cast<HWND>(hwnd),
                         static_cast<UINT>(msg),
                         static_cast<WPARAM>(wParam),
                         static_cast<LPARAM>(lParam));
    return RecordMod("MinUserXDispatchMessage", kHrOk);
#else
    (void)msg; (void)wParam; (void)lParam;
    return RecordMod("MinUserXDispatchMessage", kHrFail);
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetClientRect(
    void* hwnd,
    int32_t* outWidth,
    int32_t* outHeight) {
    if (!hwnd) {
        return RecordMod("MinUserXGetClientRect", kHrHandle);
    }
    if (!outWidth || !outHeight) {
        return RecordMod("MinUserXGetClientRect", kHrPointer);
    }
#if defined(_WIN32)
    RECT rc{};
    if (!::GetClientRect(static_cast<HWND>(hwnd), &rc)) {
        return RecordMod("MinUserXGetClientRect", kHrFail);
    }
    *outWidth = static_cast<int32_t>(rc.right - rc.left);
    *outHeight = static_cast<int32_t>(rc.bottom - rc.top);
    return RecordMod("MinUserXGetClientRect", kHrOk);
#else
    *outWidth = 0;
    *outHeight = 0;
    return RecordMod("MinUserXGetClientRect", kHrFail);
#endif
}

OS_S7_API int32_t OS_S7_CALL MinUserXSetCursorPos(int32_t x, int32_t y) {
    auto& st = GetMinUserX();
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.cursor_x = x;
        st.cursor_y = y;
        st.cursor_override = true;
#if defined(_WIN32)
        HWND fg = ::GetForegroundWindow();
        if (fg && st.owned_windows.find(reinterpret_cast<uintptr_t>(fg)) != st.owned_windows.end()) {
            (void)::SetCursorPos(x, y);
        }
#endif
    }
    return RecordMod("MinUserXSetCursorPos", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetCursorPos(int32_t* outX, int32_t* outY) {
    if (!outX || !outY) {
        return RecordMod("MinUserXGetCursorPos", kHrPointer);
    }
    auto& st = GetMinUserX();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.cursor_override) {
        *outX = st.cursor_x;
        *outY = st.cursor_y;
        return RecordMod("MinUserXGetCursorPos", kHrOk);
    }
#if defined(_WIN32)
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        *outX = static_cast<int32_t>(pt.x);
        *outY = static_cast<int32_t>(pt.y);
        return RecordMod("MinUserXGetCursorPos", kHrOk);
    }
#endif
    *outX = st.cursor_x;
    *outY = st.cursor_y;
    return RecordMod("MinUserXGetCursorPos", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettMinUserXGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryS7ModuleTotals(kMod, outTotalCalls, outNotImplCalls);
    }
    return QueryS7Telemetry(kMod, symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 5: xtfpix.dll — Real PIX Capture State Machine, Event Stack & JSON
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_XTFPIX)

namespace {

namespace fs = std::filesystem;
constexpr const char* kMod = "xtfpix.dll";
constexpr size_t kMaxPixRecords = 1024u;

struct PixOpenEvent {
    uint64_t color = 0u;
    std::string label;
    uint64_t begin_qpc = 0u;
    uint32_t thread_id = 0u;
    uint32_t depth = 0u;
};

struct PixCompletedRecord {
    bool is_marker = false;
    uint64_t color = 0u;
    std::string label;
    uint64_t begin_qpc = 0u;
    uint64_t end_qpc = 0u;
    uint32_t thread_id = 0u;
    uint32_t depth = 0u;
};

struct XtfPixState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    int32_t capturing = 0;
    uint32_t capture_flags = 0u;
    uint64_t capture_begin_qpc = 0u;
    uint64_t total_captures = 0u;
    uint64_t marker_count = 0u;
    uint64_t event_count = 0u;
    std::wstring active_capture_path;
    std::wstring last_capture_path;
    std::vector<PixOpenEvent> event_stack;
    std::deque<PixCompletedRecord> capture_records;
    std::deque<PixCompletedRecord> global_ring;
};

XtfPixState& GetXtfPix() {
    static XtfPixState* s = new XtfPixState();
    return *s;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

std::wstring ReadEnvWide(const wchar_t* name) {
#if defined(_WIN32)
    DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0u);
    if (needed == 0u) return {};
    std::wstring out(needed, L'\0');
    DWORD got = ::GetEnvironmentVariableW(name, out.data(), needed);
    if (got == 0u || got >= needed) return {};
    out.resize(got);
    return out;
#else
    (void)name;
    return {};
#endif
}

std::wstring ResolvePixCapturePath(const wchar_t* requestedPath, uint64_t captureIndex) {
    fs::path req;
    if (requestedPath && *requestedPath) {
        req = fs::path(requestedPath);
        if (req.is_absolute()) {
            return req.wstring();
        }
    }
    fs::path base_dir;
    std::wstring env_cap = ReadEnvWide(L"OPENSCARLETT_CAPTURE_DIR");
    if (!env_cap.empty()) {
        base_dir = fs::path(env_cap);
    } else {
        std::wstring env_work = ReadEnvWide(L"OPENSCARLETT_WORK_DIR");
        if (!env_work.empty()) {
            base_dir = fs::path(env_work) / L"pix_captures";
        } else {
            std::error_code ec;
            fs::path tmp = fs::temp_directory_path(ec);
            base_dir = (!ec && !tmp.empty()) ? (tmp / L"openscarlett_pix_captures")
                                             : fs::path(L"openscarlett_pix_captures");
        }
    }
    if (!req.empty()) {
        return (base_dir / req.filename()).wstring();
    }
    return (base_dir / (L"capture_" + std::to_wstring(captureIndex) + L".json")).wstring();
}

void PushPixRecordLocked(XtfPixState& st, const PixCompletedRecord& rec) {
    if (st.global_ring.size() >= kMaxPixRecords) {
        st.global_ring.pop_front();
    }
    st.global_ring.push_back(rec);
    if (st.capturing != 0) {
        if (st.capture_records.size() >= kMaxPixRecords) {
            st.capture_records.pop_front();
        }
        st.capture_records.push_back(rec);
    }
}

}  // namespace

extern "C" {

OS_S7_API int32_t OS_S7_CALL XtfPixInitialize(uint32_t flags) {
    (void)flags;
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordMod("XtfPixInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixShutdown(void) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    st.capturing = 0;
    st.event_stack.clear();
    return RecordMod("XtfPixShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixBeginCapture(const wchar_t* outputPath, uint32_t captureFlags) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.total_captures;
    st.capturing = 1;
    st.capture_flags = captureFlags;
    st.capture_begin_qpc = S7QueryQpcTicks();
    st.capture_records.clear();
    st.active_capture_path = ResolvePixCapturePath(outputPath, st.total_captures);
    return RecordMod("XtfPixBeginCapture", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixEndCapture(void) {
    std::wstring out_path_w;
    std::string body;
    {
        auto& st = GetXtfPix();
        std::lock_guard<std::mutex> lk(st.mutex);
        const uint64_t end_qpc = S7QueryQpcTicks();
        const uint64_t freq = S7QueryQpcFreq();
        st.capturing = 0;
        out_path_w = st.active_capture_path;
        st.last_capture_path = out_path_w;

        // Close any still-open scoped events into the capture snapshot without losing them.
        std::vector<PixCompletedRecord> records(st.capture_records.begin(), st.capture_records.end());
        for (const auto& open_ev : st.event_stack) {
            PixCompletedRecord rec{};
            rec.is_marker = false;
            rec.color = open_ev.color;
            rec.label = open_ev.label;
            rec.begin_qpc = open_ev.begin_qpc;
            rec.end_qpc = end_qpc;
            rec.thread_id = open_ev.thread_id;
            rec.depth = open_ev.depth;
            records.push_back(std::move(rec));
        }

        std::ostringstream oss;
        oss << "{\n"
            << "  \"module\": \"xtfpix.dll\",\n"
            << "  \"capture_index\": " << st.total_captures << ",\n"
            << "  \"capture_flags\": " << st.capture_flags << ",\n"
            << "  \"begin_qpc\": " << st.capture_begin_qpc << ",\n"
            << "  \"end_qpc\": " << end_qpc << ",\n"
            << "  \"qpc_frequency\": " << freq << ",\n"
            << "  \"total_markers\": " << st.marker_count << ",\n"
            << "  \"total_events\": " << st.event_count << ",\n"
            << "  \"records\": [\n";
        for (size_t i = 0u; i < records.size(); ++i) {
            const auto& r = records[i];
            if (i > 0u) oss << ",\n";
            const uint64_t dur = (r.end_qpc >= r.begin_qpc) ? (r.end_qpc - r.begin_qpc) : 0ull;
            oss << "    {\"kind\": \"" << (r.is_marker ? "marker" : "event")
                << "\", \"label\": \"" << S7EscapeJson(r.label)
                << "\", \"color\": " << r.color
                << ", \"depth\": " << r.depth
                << ", \"tid\": " << r.thread_id
                << ", \"begin_qpc\": " << r.begin_qpc
                << ", \"end_qpc\": " << r.end_qpc
                << ", \"duration_qpc\": " << dur << "}";
        }
        oss << "\n  ]\n"
            << "}\n";
        body = oss.str();
    }

    if (!out_path_w.empty()) {
        fs::path out_p(out_path_w);
        std::error_code ec;
        if (out_p.has_parent_path()) {
            fs::create_directories(out_p.parent_path(), ec);
        }
        std::ofstream ofs(out_p, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
            ofs.flush();
        }
    }
    return RecordMod("XtfPixEndCapture", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixIsCapturing(int32_t* outCapturing) {
    if (!outCapturing) {
        return RecordMod("XtfPixIsCapturing", kHrPointer);
    }
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    *outCapturing = st.capturing;
    return RecordMod("XtfPixIsCapturing", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixSetMarker(uint64_t color, const char* label) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.marker_count;
    const uint64_t now = S7QueryQpcTicks();
    PixCompletedRecord rec{};
    rec.is_marker = true;
    rec.color = color;
    rec.label = label ? label : "";
    rec.begin_qpc = now;
    rec.end_qpc = now;
    rec.thread_id = S7CurrentThreadId();
    rec.depth = static_cast<uint32_t>(st.event_stack.size());
    PushPixRecordLocked(st, rec);
    return RecordMod("XtfPixSetMarker", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixBeginEvent(uint64_t color, const char* label) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.event_count;
    PixOpenEvent ev{};
    ev.color = color;
    ev.label = label ? label : "";
    ev.begin_qpc = S7QueryQpcTicks();
    ev.thread_id = S7CurrentThreadId();
    ev.depth = static_cast<uint32_t>(st.event_stack.size());
    st.event_stack.push_back(std::move(ev));
    return RecordMod("XtfPixBeginEvent", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixEndEvent(void) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (!st.event_stack.empty()) {
        PixOpenEvent open_ev = std::move(st.event_stack.back());
        st.event_stack.pop_back();
        PixCompletedRecord rec{};
        rec.is_marker = false;
        rec.color = open_ev.color;
        rec.label = std::move(open_ev.label);
        rec.begin_qpc = open_ev.begin_qpc;
        rec.end_qpc = S7QueryQpcTicks();
        rec.thread_id = open_ev.thread_id;
        rec.depth = open_ev.depth;
        PushPixRecordLocked(st, rec);
    }
    return RecordMod("XtfPixEndEvent", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixGetCaptureSummary(
    uint64_t* outTotalCaptures,
    uint64_t* outMarkerCount,
    uint64_t* outEventCount,
    uint32_t* outCurrentDepth) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (outTotalCaptures) *outTotalCaptures = st.total_captures;
    if (outMarkerCount) *outMarkerCount = st.marker_count;
    if (outEventCount) *outEventCount = st.event_count;
    if (outCurrentDepth) *outCurrentDepth = static_cast<uint32_t>(st.event_stack.size());
    return RecordMod("XtfPixGetCaptureSummary", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixGetLastCapturePath(
    wchar_t* outPath,
    uint32_t pathCap,
    uint32_t* outWritten) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uint32_t needed = static_cast<uint32_t>(st.last_capture_path.size() + 1u);
    if (outWritten) *outWritten = needed;
    if (!outPath || pathCap == 0u) {
        return RecordMod("XtfPixGetLastCapturePath", outWritten ? kHrOk : kHrInvalidArg);
    }
    if (pathCap < needed) {
        return RecordMod("XtfPixGetLastCapturePath", kHrInsufficientBuffer);
    }
    std::memcpy(outPath, st.last_capture_path.c_str(), needed * sizeof(wchar_t));
    return RecordMod("XtfPixGetLastCapturePath", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettXtfPixGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryS7ModuleTotals(kMod, outTotalCalls, outNotImplCalls);
    }
    return QueryS7Telemetry(kMod, symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"

// ===========================================================================
// TARGET 6: xtfdebugmonitor.dll — Real Debug Output Ring & Multi-Client Fan-Out
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_XTFDEBUGMONITOR)

namespace {

constexpr const char* kMod = "xtfdebugmonitor.dll";
constexpr size_t kMaxDbgMonRing = 512u;

struct DebugOutputEntry {
    uint64_t seq = 0u;
    uint64_t qpc_ticks = 0u;
    uint32_t thread_id = 0u;
    uint32_t severity = 0u;
    std::string channel;
    std::string text;
};

struct DebugMonitorCallbackEntry {
    uintptr_t client_handle = 0u;  // 0 = global listener
    void* callback = nullptr;
    void* context = nullptr;
};

struct DebugMonitorSession {
    std::wstring address;
    bool started = false;
    uint32_t start_flags = 0u;
    uint64_t messages_written = 0u;
    std::deque<DebugOutputEntry> unread;
};

struct XtfDbgMonState {
    std::mutex mutex;
    uintptr_t next_client = 0xD8600001u;
    uint64_t next_cb_token = 1u;
    uint64_t next_seq = 1u;
    std::unordered_map<uintptr_t, DebugMonitorSession> clients;
    std::unordered_map<uint64_t, DebugMonitorCallbackEntry> callbacks;
    std::deque<DebugOutputEntry> global_ring;
};

XtfDbgMonState& GetDbgMon() {
    static XtfDbgMonState* s = new XtfDbgMonState();
    return *s;
}

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

int32_t WriteDebugOutputInternal(
    void* client,
    uint32_t severity,
    const char* channel,
    const char* text) {
    if (!client) {
        return kHrHandle;
    }
    const uintptr_t client_key = reinterpret_cast<uintptr_t>(client);
    const std::string ch_str = (channel && *channel) ? channel : "default";
    const std::string txt_str = text ? text : "";

    std::vector<DebugMonitorCallbackEntry> to_invoke;
    {
        auto& st = GetDbgMon();
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.clients.find(client_key);
        if (it == st.clients.end()) {
            return kHrHandle;
        }
        ++it->second.messages_written;

        DebugOutputEntry rec{};
        rec.seq = st.next_seq++;
        rec.qpc_ticks = S7QueryQpcTicks();
        rec.thread_id = S7CurrentThreadId();
        rec.severity = severity;
        rec.channel = ch_str;
        rec.text = txt_str;

        if (st.global_ring.size() >= kMaxDbgMonRing) {
            st.global_ring.pop_front();
        }
        st.global_ring.push_back(rec);

        // Fan-out to all started clients (and to the emitting client itself).
        for (auto& kv : st.clients) {
            if (kv.second.started || kv.first == client_key) {
                if (kv.second.unread.size() >= kMaxDbgMonRing) {
                    kv.second.unread.pop_front();
                }
                kv.second.unread.push_back(rec);
            }
        }

        // Snapshot matching active listeners to invoke outside lock.
        for (const auto& ckv : st.callbacks) {
            const auto& cb = ckv.second;
            if (!cb.callback) continue;
            if (cb.client_handle == 0u) {
                to_invoke.push_back(cb);
            } else {
                auto cit = st.clients.find(cb.client_handle);
                if (cit != st.clients.end() && cit->second.started) {
                    to_invoke.push_back(cb);
                }
            }
        }
    }

    using PfnDbgMonCb = void(OS_S7_CALL*)(uint32_t severity, const char* channel, const char* text, void* context);
    for (const auto& cb : to_invoke) {
        auto fn = reinterpret_cast<PfnDbgMonCb>(cb.callback);
        fn(severity, ch_str.c_str(), txt_str.c_str(), cb.context);
    }
    return kHrOk;
}

}  // namespace

extern "C" {

OS_S7_API int32_t OS_S7_CALL XtfCreateDebugMonitorClient(const wchar_t* address, void** ppClient) {
    if (!ppClient) {
        return RecordMod("XtfCreateDebugMonitorClient", kHrPointer);
    }
    *ppClient = nullptr;
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t h = st.next_client++;
    DebugMonitorSession sess{};
    sess.address = address ? std::wstring(address) : std::wstring(L"localhost");
    st.clients.emplace(h, std::move(sess));
    *ppClient = reinterpret_cast<void*>(h);
    return RecordMod("XtfCreateDebugMonitorClient", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorStart(void* client, uint32_t flags) {
    if (!client) {
        return RecordMod("XtfDebugMonitorStart", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it == st.clients.end()) {
        return RecordMod("XtfDebugMonitorStart", kHrHandle);
    }
    it->second.started = true;
    it->second.start_flags = flags;
    return RecordMod("XtfDebugMonitorStart", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorStop(void* client) {
    if (!client) {
        return RecordMod("XtfDebugMonitorStop", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it == st.clients.end()) {
        return RecordMod("XtfDebugMonitorStop", kHrHandle);
    }
    it->second.started = false;
    it->second.start_flags = 0u;
    return RecordMod("XtfDebugMonitorStop", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorWriteOutput(void* client, const char* text) {
    const int32_t hr = WriteDebugOutputInternal(client, 0u, "default", text);
    return RecordMod("XtfDebugMonitorWriteOutput", hr);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorWriteOutputEx(
    void* client,
    uint32_t severity,
    const char* channel,
    const char* text) {
    const int32_t hr = WriteDebugOutputInternal(client, severity, channel, text);
    return RecordMod("XtfDebugMonitorWriteOutputEx", hr);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorReadOutput(
    void* client,
    uint32_t* outSeverity,
    char* outChannel,
    uint32_t channelCap,
    char* outText,
    uint32_t textCap,
    uint32_t* outHasMore) {
    if (!client) {
        return RecordMod("XtfDebugMonitorReadOutput", kHrHandle);
    }
    if (outSeverity) *outSeverity = 0u;
    if (outChannel && channelCap > 0u) outChannel[0] = '\0';
    if (outText && textCap > 0u) outText[0] = '\0';
    if (outHasMore) *outHasMore = 0u;

    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it == st.clients.end()) {
        return RecordMod("XtfDebugMonitorReadOutput", kHrHandle);
    }
    if (it->second.unread.empty()) {
        return RecordMod("XtfDebugMonitorReadOutput", kHrFalse);
    }
    DebugOutputEntry rec = std::move(it->second.unread.front());
    it->second.unread.pop_front();
    if (outSeverity) *outSeverity = rec.severity;
    CopyBoundedUtf8(rec.channel, outChannel, channelCap);
    CopyBoundedUtf8(rec.text, outText, textCap);
    if (outHasMore) *outHasMore = it->second.unread.empty() ? 0u : 1u;
    return RecordMod("XtfDebugMonitorReadOutput", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorRegisterCallback(
    void* client,
    void* callback,
    void* context,
    uint64_t* outToken) {
    if (!callback || !outToken) {
        return RecordMod("XtfDebugMonitorRegisterCallback", kHrInvalidArg);
    }
    const uintptr_t client_key = reinterpret_cast<uintptr_t>(client);
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (client_key != 0u && st.clients.find(client_key) == st.clients.end()) {
        return RecordMod("XtfDebugMonitorRegisterCallback", kHrHandle);
    }
    const uint64_t tok = st.next_cb_token++;
    st.callbacks[tok] = DebugMonitorCallbackEntry{client_key, callback, context};
    *outToken = tok;
    return RecordMod("XtfDebugMonitorRegisterCallback", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorUnregisterCallback(uint64_t token) {
    if (token == 0ull) {
        return RecordMod("XtfDebugMonitorUnregisterCallback", kHrInvalidArg);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.callbacks.erase(token) == 0u) {
        return RecordMod("XtfDebugMonitorUnregisterCallback", kHrInvalidArg);
    }
    return RecordMod("XtfDebugMonitorUnregisterCallback", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorClose(void* client) {
    if (!client) {
        return RecordMod("XtfDebugMonitorClose", kHrHandle);
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(client);
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.clients.erase(key) == 0u) {
        return RecordMod("XtfDebugMonitorClose", kHrHandle);
    }
    for (auto it = st.callbacks.begin(); it != st.callbacks.end(); ) {
        if (it->second.client_handle == key) {
            it = st.callbacks.erase(it);
        } else {
            ++it;
        }
    }
    return RecordMod("XtfDebugMonitorClose", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorIsAttached(int32_t* outAttached) {
    if (!outAttached) {
        return RecordMod("XtfDebugMonitorIsAttached", kHrPointer);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    bool attached = false;
    bool has_global_listener = false;
    for (const auto& ckv : st.callbacks) {
        if (ckv.second.client_handle == 0u && ckv.second.callback != nullptr) {
            has_global_listener = true;
            break;
        }
    }
    for (const auto& kv : st.clients) {
        if (!kv.second.started) continue;
        if (kv.second.start_flags != 0u || has_global_listener) {
            attached = true;
            break;
        }
        for (const auto& ckv : st.callbacks) {
            if (ckv.second.client_handle == kv.first && ckv.second.callback != nullptr) {
                attached = true;
                break;
            }
        }
        if (attached) break;
    }
    *outAttached = attached ? 1 : 0;
    return RecordMod("XtfDebugMonitorIsAttached", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorGetStats(
    void* client,
    uint64_t* outMessagesWritten,
    uint64_t* outRingCount,
    uint32_t* outActiveClients) {
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (outMessagesWritten) {
        if (client != nullptr) {
            auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
            *outMessagesWritten = (it != st.clients.end()) ? it->second.messages_written : 0u;
        } else {
            uint64_t sum = 0u;
            for (const auto& kv : st.clients) sum += kv.second.messages_written;
            *outMessagesWritten = sum;
        }
    }
    if (outRingCount) {
        *outRingCount = static_cast<uint64_t>(st.global_ring.size());
    }
    if (outActiveClients) {
        uint32_t active = 0u;
        for (const auto& kv : st.clients) {
            if (kv.second.started) ++active;
        }
        *outActiveClients = active;
    }
    return RecordMod("XtfDebugMonitorGetStats", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettXtfDebugMonitorGetTelemetry(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return QueryS7ModuleTotals(kMod, outTotalCalls, outNotImplCalls);
    }
    return QueryS7Telemetry(kMod, symbolName, outTotalCalls, outOkCalls, outNotImplCalls);
}

}  // extern "C"

#else
#  error "Define one of OPENSCARLETT_S7_TARGET_{GAMECORECLIENT,MINUSERX,XTFPIX,XTFDEBUGMONITOR}"
#endif
