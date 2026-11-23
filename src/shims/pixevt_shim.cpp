#define OPENSCARLETT_BUILDING_PIXEVT 1
#include "openscarlett/shims/secondary_shims_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kModName = "pixevt.dll";

struct PixEtwSession {
    uint64_t handle = 0u;
    std::wstring name;
    bool active = true;
    uint32_t enabled_provider_count = 0u;
    uint32_t flush_count = 0u;
    uint32_t rundown_count = 0u;
    std::vector<std::string> enabled_providers;
};

struct PixState {
    std::atomic<uint64_t> event_count{0};
    std::atomic<uint64_t> cpu_depth{0};
    std::atomic<uint64_t> cmdlist_depth{0};
    std::atomic<uint64_t> cmdqueue_depth{0};
    std::atomic<uint64_t> fence_wake_count{0};
    std::atomic<uint64_t> gpu_capture_requests{0};

    std::mutex mutex;
    std::string last_label;
    std::wstring last_capture_file;
    std::unordered_map<std::wstring, float> counters;
    uint64_t next_etw_handle = 0x91CE0001ull;
    std::unordered_map<uint64_t, PixEtwSession> etw_sessions;
};

PixState& GetPix() {
    static PixState* s = new PixState();
    return *s;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

void DecrementDepthClamped(std::atomic<uint64_t>& depth) {
    uint64_t cur = depth.load(std::memory_order_acquire);
    while (cur > 0u) {
        if (depth.compare_exchange_weak(cur, cur - 1u, std::memory_order_acq_rel)) {
            return;
        }
    }
}

void RecordLabel(const char* label) {
    if (!label) return;
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.last_label = label;
}

}  // namespace

extern "C" {

void OS_SHIM_CALL PIXBeginEventOnCpu(uint64_t /*color*/, const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cpu_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCpu");
}

void OS_SHIM_CALL PIXEndEventOnCpu(void) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cpu_depth);
    RecordOk("PIXEndEventOnCpu");
}

void OS_SHIM_CALL PIXSetMarkerOnCpu(uint64_t /*color*/, const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCpu");
}

void OS_SHIM_CALL PIXBeginEventOnCommandList(
    void* /*commandList*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cmdlist_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCommandList");
}

void OS_SHIM_CALL PIXEndEventOnCommandList(void* /*commandList*/) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cmdlist_depth);
    RecordOk("PIXEndEventOnCommandList");
}

void OS_SHIM_CALL PIXSetMarkerOnCommandList(
    void* /*commandList*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCommandList");
}

void OS_SHIM_CALL PIXBeginEventOnCommandQueue(
    void* /*commandQueue*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    st.cmdqueue_depth.fetch_add(1u, std::memory_order_acq_rel);
    RecordLabel(formatString);
    RecordOk("PIXBeginEventOnCommandQueue");
}

void OS_SHIM_CALL PIXEndEventOnCommandQueue(void* /*commandQueue*/) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    DecrementDepthClamped(st.cmdqueue_depth);
    RecordOk("PIXEndEventOnCommandQueue");
}

void OS_SHIM_CALL PIXSetMarkerOnCommandQueue(
    void* /*commandQueue*/,
    uint64_t /*color*/,
    const char* formatString) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordLabel(formatString);
    RecordOk("PIXSetMarkerOnCommandQueue");
}

void OS_SHIM_CALL PIXNotifyWakeFromFenceSignal(void* /*eventHandle*/) {
    auto& st = GetPix();
    st.fence_wake_count.fetch_add(1u, std::memory_order_relaxed);
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    RecordOk("PIXNotifyWakeFromFenceSignal");
}

void OS_SHIM_CALL PIXReportCounter(const wchar_t* name, float value) {
    auto& st = GetPix();
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    if (name && *name) {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.counters[std::wstring(name)] = value;
    }
    RecordOk("PIXReportCounter");
}

int32_t OS_SHIM_CALL PIXGpuCaptureNextFrames(const wchar_t* fileName, uint32_t numFrames) {
    if (numFrames == 0u) {
        return RecordHr("PIXGpuCaptureNextFrames", OS_SHIM_E_INVALIDARG);
    }
    auto& st = GetPix();
    st.gpu_capture_requests.fetch_add(1u, std::memory_order_relaxed);
    st.event_count.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        st.last_capture_file = (fileName && *fileName) ? std::wstring(fileName) : L"capture.wpix";
    }
    return RecordHr("PIXGpuCaptureNextFrames", OS_SHIM_S_OK);
}

uint64_t OS_SHIM_CALL PIXGetEventCountForTest(void) {
    RecordOk("PIXGetEventCountForTest");
    return GetPix().event_count.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// PIX In-Memory ETW Capture Session Helpers (non-admin local session tracking)
// ---------------------------------------------------------------------------
OS_PIXEVT_API uint32_t OS_SHIM_CALL PIXEtwStartSession(
    const wchar_t* sessionName,
    uint64_t* outSessionHandle) {
    if (!sessionName || !*sessionName || !outSessionHandle) {
        RecordHr("PIXEtwStartSession", OS_SHIM_E_INVALIDARG);
        return 87u;  // ERROR_INVALID_PARAMETER
    }
    *outSessionHandle = 0ull;
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    for (const auto& [h, s] : st.etw_sessions) {
        if (s.active && s.name == sessionName) {
            RecordHr("PIXEtwStartSession", OS_SHIM_E_FAIL);
            return 183u;  // ERROR_ALREADY_EXISTS
        }
    }
    const uint64_t h = st.next_etw_handle++;
    PixEtwSession sess{};
    sess.handle = h;
    sess.name = sessionName;
    sess.active = true;
    st.etw_sessions.emplace(h, std::move(sess));
    *outSessionHandle = h;
    RecordOk("PIXEtwStartSession");
    return 0u;
}

OS_PIXEVT_API uint32_t OS_SHIM_CALL PIXEtwEnableProvider(
    uint64_t sessionHandle,
    const void* providerGuid,
    uint32_t controlCode) {
    if (sessionHandle == 0ull || !providerGuid) {
        RecordHr("PIXEtwEnableProvider", OS_SHIM_E_INVALIDARG);
        return 87u;
    }
    std::string key(reinterpret_cast<const char*>(providerGuid), 16u);
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.etw_sessions.find(sessionHandle);
    if (it == st.etw_sessions.end() || !it->second.active) {
        RecordHr("PIXEtwEnableProvider", OS_SHIM_E_HANDLE);
        return 4201u;  // ERROR_WMI_INSTANCE_NOT_FOUND
    }
    auto& vec = it->second.enabled_providers;
    if (controlCode == 1u) {  // ENABLE_PROVIDER
        if (std::find(vec.begin(), vec.end(), key) == vec.end()) {
            vec.push_back(std::move(key));
        }
        it->second.enabled_provider_count = static_cast<uint32_t>(vec.size());
    } else if (controlCode == 0u) {  // DISABLE_PROVIDER
        vec.erase(std::remove(vec.begin(), vec.end(), key), vec.end());
        it->second.enabled_provider_count = static_cast<uint32_t>(vec.size());
    } else if (controlCode == 2u) {  // CAPTURE_STATE (rundown)
        ++it->second.rundown_count;
    } else {
        RecordHr("PIXEtwEnableProvider", OS_SHIM_E_INVALIDARG);
        return 87u;
    }
    RecordOk("PIXEtwEnableProvider");
    return 0u;
}

OS_PIXEVT_API uint32_t OS_SHIM_CALL PIXEtwFlushAndRundown(uint64_t sessionHandle) {
    if (sessionHandle == 0ull) {
        RecordHr("PIXEtwFlushAndRundown", OS_SHIM_E_INVALIDARG);
        return 87u;
    }
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.etw_sessions.find(sessionHandle);
    if (it == st.etw_sessions.end() || !it->second.active) {
        RecordHr("PIXEtwFlushAndRundown", OS_SHIM_E_HANDLE);
        return 4201u;
    }
    ++it->second.flush_count;
    ++it->second.rundown_count;
    RecordOk("PIXEtwFlushAndRundown");
    return 0u;
}

OS_PIXEVT_API uint32_t OS_SHIM_CALL PIXEtwStopSession(uint64_t sessionHandle) {
    if (sessionHandle == 0ull) {
        RecordHr("PIXEtwStopSession", OS_SHIM_E_INVALIDARG);
        return 87u;
    }
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.etw_sessions.find(sessionHandle);
    if (it == st.etw_sessions.end() || !it->second.active) {
        RecordHr("PIXEtwStopSession", OS_SHIM_E_HANDLE);
        return 4201u;
    }
    ++it->second.rundown_count;
    it->second.active = false;
    it->second.enabled_providers.clear();
    it->second.enabled_provider_count = 0u;
    RecordOk("PIXEtwStopSession");
    return 0u;
}

OS_PIXEVT_API int32_t OS_SHIM_CALL PIXEtwGetSessionStatsForTest(
    uint64_t sessionHandle,
    uint32_t* outActive,
    uint32_t* outEnabledProviders,
    uint32_t* outFlushCount,
    uint32_t* outRundownCount) {
    auto& st = GetPix();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.etw_sessions.find(sessionHandle);
    if (it == st.etw_sessions.end()) {
        return 0;
    }
    if (outActive) *outActive = it->second.active ? 1u : 0u;
    if (outEnabledProviders) *outEnabledProviders = it->second.enabled_provider_count;
    if (outFlushCount) *outFlushCount = it->second.flush_count;
    if (outRundownCount) *outRundownCount = it->second.rundown_count;
    return 1;
}

}  // extern "C"
