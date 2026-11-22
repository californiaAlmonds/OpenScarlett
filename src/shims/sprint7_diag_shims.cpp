// MIT Sprint 7 Fixed-Return Diagnostic & Media Compatibility Shims for OpenScarlett.
// Compiled into 6 standalone DLL targets via preprocessor target macros:
//   1. OPENSCARLETT_S7_TARGET_MFLNMHELP        -> mflnmhelp.dll
//   2. OPENSCARLETT_S7_TARGET_MFHEVCDEC        -> mfhevcdec.dll
//   3. OPENSCARLETT_S7_TARGET_GAMECORECLIENT   -> gamecoreclient.dll
//   4. OPENSCARLETT_S7_TARGET_MINUSERX         -> minuserx.dll
//   5. OPENSCARLETT_S7_TARGET_XTFPIX           -> xtfpix.dll
//   6. OPENSCARLETT_S7_TARGET_XTFDEBUGMONITOR  -> xtfdebugmonitor.dll
//
// Every export logs via openscarlett::CallTelemetry (with not_impl = 0) and stays
// portable and 0-warning clean across MSVC (/W4), Clang-CL, and MinGW-w64.

#include "openscarlett/core/telemetry.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
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
[[maybe_unused]] constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
[[maybe_unused]] constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
[[maybe_unused]] constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
[[maybe_unused]] constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
[[maybe_unused]] constexpr int32_t kHrClassNoAggregation = static_cast<int32_t>(0x80040110u);

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
// TARGET 1: mflnmhelp.dll — Media Foundation License & Network Helper Stubs
// ===========================================================================
#if defined(OPENSCARLETT_S7_TARGET_MFLNMHELP)

namespace {

constexpr const char* kMod = "mflnmhelp.dll";

struct MfLnmState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uintptr_t next_license = 0x4C4E0001u;
    std::unordered_map<uintptr_t, std::wstring> licenses;
};

MfLnmState& GetMfLnm() {
    static MfLnmState* s = new MfLnmState();
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

OS_S7_API int32_t OS_S7_CALL MfLnmHelperInitialize(uint32_t flags) {
    (void)flags;
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.init_count;
    return RecordMod("MfLnmHelperInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MfLnmHelperShutdown(void) {
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.init_count > 0u) {
        --st.init_count;
    }
    return RecordMod("MfLnmHelperShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MfLnmCheckLicenseStatus(const wchar_t* contentId, uint32_t* outStatus) {
    (void)contentId;
    if (!outStatus) {
        return RecordMod("MfLnmCheckLicenseStatus", kHrPointer);
    }
    *outStatus = 1u;  // Ready / licensed in local compatibility mode
    return RecordMod("MfLnmCheckLicenseStatus", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MfLnmQueryNetworkCapabilities(uint32_t* outCaps) {
    if (!outCaps) {
        return RecordMod("MfLnmQueryNetworkCapabilities", kHrPointer);
    }
    *outCaps = 0x1u;
    return RecordMod("MfLnmQueryNetworkCapabilities", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MfLnmAcquireLicense(const wchar_t* contentId, void** outLicenseHandle) {
    if (!outLicenseHandle) {
        return RecordMod("MfLnmAcquireLicense", kHrPointer);
    }
    *outLicenseHandle = nullptr;
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    const uintptr_t handle = st.next_license++;
    st.licenses[handle] = contentId ? std::wstring(contentId) : std::wstring();
    *outLicenseHandle = reinterpret_cast<void*>(handle);
    return RecordMod("MfLnmAcquireLicense", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MfLnmReleaseLicense(void* licenseHandle) {
    if (!licenseHandle) {
        return RecordMod("MfLnmReleaseLicense", kHrHandle);
    }
    auto& st = GetMfLnm();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.licenses.erase(reinterpret_cast<uintptr_t>(licenseHandle));
    return RecordMod("MfLnmReleaseLicense", kHrOk);
}

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#endif
OS_S7_COM_API int32_t OS_S7_CALL DllCanUnloadNow(void) {
    return RecordMod("DllCanUnloadNow", kHrFalse);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettMfLnmHelpGetTelemetry(
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
// TARGET 2: mfhevcdec.dll — Media Foundation HEVC Decoder COM / Factory Stubs
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_MFHEVCDEC)

namespace {

constexpr const char* kMod = "mfhevcdec.dll";

inline int32_t RecordMod(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kMod,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

struct S7Guid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};

constexpr S7Guid kIidUnknown = {0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr S7Guid kIidClassFactory = {0x00000001u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

bool GuidEq(const void* a, const S7Guid& b) noexcept {
    return a && std::memcmp(a, &b, sizeof(S7Guid)) == 0;
}

struct HevcStubObject {
    virtual int32_t OS_S7_CALL QueryInterface(const void* riid, void** ppv) {
        if (!ppv) return RecordMod("IMFHevcDecoder::QueryInterface", kHrPointer);
        *ppv = nullptr;
        if (!riid) return RecordMod("IMFHevcDecoder::QueryInterface", kHrInvalidArg);
        AddRef();
        *ppv = this;
        return RecordMod("IMFHevcDecoder::QueryInterface", kHrOk);
    }
    virtual uint32_t OS_S7_CALL AddRef() {
        RecordMod("IMFHevcDecoder::AddRef", kHrOk);
        return ref_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    virtual uint32_t OS_S7_CALL Release() {
        RecordMod("IMFHevcDecoder::Release", kHrOk);
        const uint32_t rem = ref_count.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }
    virtual ~HevcStubObject() = default;
    std::atomic<uint32_t> ref_count{1u};
};

struct HevcClassFactory {
    virtual int32_t OS_S7_CALL QueryInterface(const void* riid, void** ppv) {
        if (!ppv) return RecordMod("HevcClassFactory::QueryInterface", kHrPointer);
        *ppv = nullptr;
        if (!riid) return RecordMod("HevcClassFactory::QueryInterface", kHrInvalidArg);
        if (GuidEq(riid, kIidUnknown) || GuidEq(riid, kIidClassFactory)) {
            AddRef();
            *ppv = this;
            return RecordMod("HevcClassFactory::QueryInterface", kHrOk);
        }
        return RecordMod("HevcClassFactory::QueryInterface", kHrNoInterface);
    }
    virtual uint32_t OS_S7_CALL AddRef() {
        RecordMod("HevcClassFactory::AddRef", kHrOk);
        return ref_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    virtual uint32_t OS_S7_CALL Release() {
        RecordMod("HevcClassFactory::Release", kHrOk);
        const uint32_t cur = ref_count.load(std::memory_order_acquire);
        return cur > 1u ? ref_count.fetch_sub(1u, std::memory_order_acq_rel) - 1u : 1u;
    }
    virtual int32_t OS_S7_CALL CreateInstance(void* pUnkOuter, const void* riid, void** ppvObject) {
        if (!ppvObject) return RecordMod("HevcClassFactory::CreateInstance", kHrPointer);
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) {
            return RecordMod("HevcClassFactory::CreateInstance", kHrClassNoAggregation);
        }
        auto* obj = new HevcStubObject();
        const int32_t hr = obj->QueryInterface(riid ? riid : &kIidUnknown, ppvObject);
        obj->Release();
        return RecordMod("HevcClassFactory::CreateInstance", hr);
    }
    virtual int32_t OS_S7_CALL LockServer(int32_t fLock) {
        if (fLock) {
            lock_count.fetch_add(1u, std::memory_order_relaxed);
        } else {
            uint32_t cur = lock_count.load(std::memory_order_relaxed);
            if (cur > 0u) {
                lock_count.fetch_sub(1u, std::memory_order_relaxed);
            }
        }
        return RecordMod("HevcClassFactory::LockServer", kHrOk);
    }
    std::atomic<uint32_t> ref_count{1u};
    std::atomic<uint32_t> lock_count{0u};
};

HevcClassFactory& GetFactory() {
    static HevcClassFactory* s = new HevcClassFactory();
    return *s;
}

std::atomic<uint32_t> g_hevc_init_count{0u};

}  // namespace

extern "C" {

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllGetClassObject,PRIVATE")
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#  pragma comment(linker, "/export:DllRegisterServer,PRIVATE")
#  pragma comment(linker, "/export:DllUnregisterServer,PRIVATE")
#endif

OS_S7_COM_API int32_t OS_S7_CALL DllGetClassObject(const void* rclsid, const void* riid, void** ppv) {
    (void)rclsid;
    if (!ppv) {
        return RecordMod("DllGetClassObject", kHrPointer);
    }
    *ppv = nullptr;
    auto& factory = GetFactory();
    const int32_t hr = factory.QueryInterface(riid ? riid : &kIidClassFactory, ppv);
    return RecordMod("DllGetClassObject", hr);
}

OS_S7_COM_API int32_t OS_S7_CALL DllCanUnloadNow(void) {
    return RecordMod("DllCanUnloadNow", kHrOk);
}

OS_S7_COM_API int32_t OS_S7_CALL DllRegisterServer(void) {
    return RecordMod("DllRegisterServer", kHrOk);
}

OS_S7_COM_API int32_t OS_S7_CALL DllUnregisterServer(void) {
    return RecordMod("DllUnregisterServer", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MFHevcDecoderInitialize(uint32_t flags) {
    (void)flags;
    g_hevc_init_count.fetch_add(1u, std::memory_order_relaxed);
    return RecordMod("MFHevcDecoderInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MFHevcDecoderShutdown(void) {
    uint32_t cur = g_hevc_init_count.load(std::memory_order_relaxed);
    if (cur > 0u) {
        g_hevc_init_count.fetch_sub(1u, std::memory_order_relaxed);
    }
    return RecordMod("MFHevcDecoderShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MFHevcDecoderGetCapabilities(
    uint32_t* outProfileFlags,
    uint32_t* outMaxWidth,
    uint32_t* outMaxHeight) {
    if (outProfileFlags) *outProfileFlags = 0x3u;  // Main + Main10
    if (outMaxWidth) *outMaxWidth = 3840u;
    if (outMaxHeight) *outMaxHeight = 2160u;
    return RecordMod("MFHevcDecoderGetCapabilities", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL OpenScarlettMfHevcDecGetTelemetry(
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
// TARGET 3: gamecoreclient.dll — GameCore Client Service & Session Stubs
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_GAMECORECLIENT)

namespace {

constexpr const char* kMod = "gamecoreclient.dll";

struct GameCoreClientState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    uint32_t lifecycle_events = 0u;
    uintptr_t next_session = 0x6C0C0001u;
    std::unordered_map<uintptr_t, std::string> sessions;
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
    st.sessions[h] = appId ? std::string(appId) : std::string("OpenScarlett.App");
    *outSession = reinterpret_cast<void*>(h);
    return RecordMod("GameCoreClientOpenSession", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientCloseSession(void* session) {
    if (!session) {
        return RecordMod("GameCoreClientCloseSession", kHrHandle);
    }
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.sessions.erase(reinterpret_cast<uintptr_t>(session));
    return RecordMod("GameCoreClientCloseSession", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientGetServiceState(uint32_t* outState) {
    if (!outState) {
        return RecordMod("GameCoreClientGetServiceState", kHrPointer);
    }
    *outState = 1u;  // Running / Active
    return RecordMod("GameCoreClientGetServiceState", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL GameCoreClientNotifyLifecycleEvent(uint32_t eventId) {
    (void)eventId;
    auto& st = GetGcClient();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.lifecycle_events;
    return RecordMod("GameCoreClientNotifyLifecycleEvent", kHrOk);
}

#if defined(_MSC_VER)
#  pragma comment(linker, "/export:DllCanUnloadNow,PRIVATE")
#endif
OS_S7_COM_API int32_t OS_S7_CALL DllCanUnloadNow(void) {
    return RecordMod("DllCanUnloadNow", kHrOk);
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
// TARGET 4: minuserx.dll — Minimal Xbox Win32 User Shim Stubs
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_MINUSERX)

namespace {

constexpr const char* kMod = "minuserx.dll";

std::atomic<uint32_t> g_minuser_init{0u};
std::atomic<int32_t> g_cursor_visible{1};

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

OS_S7_API int32_t OS_S7_CALL MinUserXInitialize(uint32_t flags) {
    (void)flags;
    g_minuser_init.fetch_add(1u, std::memory_order_relaxed);
    return RecordMod("MinUserXInitialize", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXShutdown(void) {
    uint32_t cur = g_minuser_init.load(std::memory_order_relaxed);
    if (cur > 0u) {
        g_minuser_init.fetch_sub(1u, std::memory_order_relaxed);
    }
    return RecordMod("MinUserXShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetSystemMetrics(int32_t nIndex) {
    RecordMod("MinUserXGetSystemMetrics", kHrOk);
#if defined(_WIN32)
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
    return g_cursor_visible.exchange(visible ? 1 : 0, std::memory_order_acq_rel);
}

OS_S7_API int32_t OS_S7_CALL MinUserXGetDisplayScaleFactor(uint32_t* outScalePercent) {
    if (!outScalePercent) {
        return RecordMod("MinUserXGetDisplayScaleFactor", kHrPointer);
    }
    *outScalePercent = 100u;
    return RecordMod("MinUserXGetDisplayScaleFactor", kHrOk);
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
// TARGET 5: xtfpix.dll — Xbox Tool Framework PIX Capture & Marker Stubs
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_XTFPIX)

namespace {

constexpr const char* kMod = "xtfpix.dll";

struct XtfPixState {
    std::mutex mutex;
    uint32_t init_count = 0u;
    int32_t capturing = 0;
    uint64_t marker_count = 0u;
    uint64_t event_depth = 0u;
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
    return RecordMod("XtfPixShutdown", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixBeginCapture(const wchar_t* outputPath, uint32_t captureFlags) {
    (void)outputPath;
    (void)captureFlags;
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.capturing = 1;
    return RecordMod("XtfPixBeginCapture", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixEndCapture(void) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.capturing = 0;
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
    (void)color;
    (void)label;
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.marker_count;
    return RecordMod("XtfPixSetMarker", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixBeginEvent(uint64_t color, const char* label) {
    (void)color;
    (void)label;
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    ++st.event_depth;
    return RecordMod("XtfPixBeginEvent", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfPixEndEvent(void) {
    auto& st = GetXtfPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    if (st.event_depth > 0u) {
        --st.event_depth;
    }
    return RecordMod("XtfPixEndEvent", kHrOk);
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
// TARGET 6: xtfdebugmonitor.dll — Xbox Tool Framework Debug Monitor Stubs
// ===========================================================================
#elif defined(OPENSCARLETT_S7_TARGET_XTFDEBUGMONITOR)

namespace {

constexpr const char* kMod = "xtfdebugmonitor.dll";

struct DebugMonitorSession {
    std::wstring address;
    bool started = false;
    uint64_t messages_written = 0u;
};

struct XtfDbgMonState {
    std::mutex mutex;
    uintptr_t next_client = 0xD8600001u;
    std::unordered_map<uintptr_t, DebugMonitorSession> clients;
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
    (void)flags;
    if (!client) {
        return RecordMod("XtfDebugMonitorStart", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it != st.clients.end()) {
        it->second.started = true;
    }
    return RecordMod("XtfDebugMonitorStart", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorStop(void* client) {
    if (!client) {
        return RecordMod("XtfDebugMonitorStop", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it != st.clients.end()) {
        it->second.started = false;
    }
    return RecordMod("XtfDebugMonitorStop", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorWriteOutput(void* client, const char* text) {
    (void)text;
    if (!client) {
        return RecordMod("XtfDebugMonitorWriteOutput", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.clients.find(reinterpret_cast<uintptr_t>(client));
    if (it != st.clients.end()) {
        ++it->second.messages_written;
    }
    return RecordMod("XtfDebugMonitorWriteOutput", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorClose(void* client) {
    if (!client) {
        return RecordMod("XtfDebugMonitorClose", kHrHandle);
    }
    auto& st = GetDbgMon();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.clients.erase(reinterpret_cast<uintptr_t>(client));
    return RecordMod("XtfDebugMonitorClose", kHrOk);
}

OS_S7_API int32_t OS_S7_CALL XtfDebugMonitorIsAttached(int32_t* outAttached) {
    if (!outAttached) {
        return RecordMod("XtfDebugMonitorIsAttached", kHrPointer);
    }
    *outAttached = 0;
    return RecordMod("XtfDebugMonitorIsAttached", kHrOk);
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
#  error "Define one of OPENSCARLETT_S7_TARGET_{MFLNMHELP,MFHEVCDEC,GAMECORECLIENT,MINUSERX,XTFPIX,XTFDEBUGMONITOR}"
#endif
