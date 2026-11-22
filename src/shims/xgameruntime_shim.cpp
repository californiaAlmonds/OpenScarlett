#define OPENSCARLETT_BUILDING_XGAMERUNTIME 1
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/kernel/console_lsa_policy.h"
#include "openscarlett/kernel/plm_notifications.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <dxgi1_6.h>
#  include <malloc.h>
#  include <windows.h>
#endif

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {

constexpr const char* kModName = "xgameruntime.dll";

inline XGameResult RecordHr(const char* sym, XGameResult hr) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

std::string GetEnvString(const char* name) {
#if defined(_WIN32)
    if (!name || !*name) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    const DWORD written = GetEnvironmentVariableA(name, out.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    out.resize(static_cast<size_t>(written));
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

// Canonical GDK E_GAMEUSER_NO_TOKEN_REQUIRED (0x89245105) returned by default when
// offline-first token policy is active so real titles immediately take offline paths
// instead of hanging on online Xbox Live auth handshakes.
constexpr XGameResult kHrGameUserNoTokenRequired = static_cast<XGameResult>(0x89245105u);

// Valid Microsoft GDK XErrorOptions 3-bit flag mask:
//   0x00 = XErrorOptions_None
//   0x01 = XErrorOptions_OutputDebugStringOnError
//   0x02 = XErrorOptions_DebugBreakOnError
//   0x04 = XErrorOptions_FailFastOnError
constexpr uint32_t kValidXErrorOptionsMask =
    static_cast<uint32_t>(XErrorOptions_OutputDebugStringOnError) |
    static_cast<uint32_t>(XErrorOptions_DebugBreakOnError) |
    static_cast<uint32_t>(XErrorOptions_FailFastOnError);

bool IsMockXstsEnabled() {
    std::string v = GetEnvString("OPENSCARLETT_MOCK_XSTS");
    if (v.empty()) {
        return false;
    }
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool HasDialogButtonText(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

bool IsDialogButtonIndexValid(
    uint32_t button_index,
    const char* first_text,
    const char* second_text,
    const char* third_text) noexcept {
    switch (button_index) {
        case 0u:
            return HasDialogButtonText(first_text);
        case 1u:
            return HasDialogButtonText(second_text);
        case 2u:
            return HasDialogButtonText(third_text);
        default:
            return false;
    }
}

bool ValidateMessageDialogButtons(
    uint32_t default_button,
    uint32_t cancel_button,
    const char* first_text,
    const char* second_text,
    const char* third_text) noexcept {
    return IsDialogButtonIndexValid(default_button, first_text, second_text, third_text) &&
           IsDialogButtonIndexValid(cancel_button, first_text, second_text, third_text);
}

fs::path ResolveSaveRoot() {
    std::string env_root = GetEnvString("OPENSCARLETT_SAVE_ROOT");
    if (!env_root.empty()) {
        return fs::path(env_root);
    }
    std::string env_temp = GetEnvString("TEMP");
    if (!env_temp.empty()) {
        return fs::path(env_temp) / "saves";
    }
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (!ec && !tmp.empty()) {
        return tmp / "openscarlett_saves";
    }
    return fs::path("openscarlett_saves");
}

std::string EncodePathToken(const std::string& raw) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            out.push_back(kHex[(c >> 4) & 0xFu]);
            out.push_back(kHex[c & 0xFu]);
        }
    }
    return out.empty() ? "default" : out;
}

std::string FormatXuidHex(uint64_t xuid) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(xuid));
    return std::string(buf);
}

std::string EscapeLine(const std::string& s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '%' || c == '\r' || c == '\n' || c == '\t' || c < 0x20u) {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xFu]);
            out.push_back(kHex[c & 0xFu]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string UnescapeLine(const std::string& s) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool WriteBinaryFileAtomic(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        if (size > 0 && data) {
            ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        ofs.flush();
        if (!ofs.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
            if (!ofs) return false;
            if (size > 0 && data) {
                ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            }
            ofs.flush();
            return ofs.good();
        }
    }
    return true;
}

bool ReadBinaryFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamoff sz = ifs.tellg();
    if (sz < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(out.data()), sz);
        if (!ifs) return false;
    }
    return true;
}

XGameResult CopyStringResult(
    const std::string& src,
    size_t buffer_size,
    char* buffer,
    size_t* used) {
    const size_t needed = src.size() + 1;
    if (used) {
        *used = needed;
    }
    if (!buffer) {
        return XGR_E_INVALIDARG;
    }
    if (buffer_size < needed) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buffer, src.c_str(), needed);
    return XGR_S_OK;
}

XGameResult CopyWideStringResult(
    const std::wstring& src,
    size_t buffer_count,
    wchar_t* buffer,
    size_t* used_count) {
    const size_t needed = src.size() + 1;
    if (used_count) {
        *used_count = needed;
    }
    if (!buffer) {
        return XGR_E_INVALIDARG;
    }
    if (buffer_count < needed) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buffer, src.c_str(), needed * sizeof(wchar_t));
    return XGR_S_OK;
}

// ===========================================================================
// XTaskQueue Implementation
// ===========================================================================
struct ScheduledCallback {
    SteadyClock::time_point ready_time;
    uint64_t seq = 0;
    XTaskQueueCallback callback = nullptr;
    void* context = nullptr;
};

struct MonitorEntry {
    uint64_t token = 0;
    XTaskQueueMonitorCallback callback = nullptr;
    void* context = nullptr;
};

struct WaiterEntry {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    std::weak_ptr<XTaskQueueObject> queue_weak;
    XTaskQueuePort port = XTaskQueuePort_Work;
    XTaskQueueCallback callback = nullptr;
    void* context = nullptr;
    std::atomic<bool> active{true};
#if defined(_WIN32)
    std::mutex wait_mutex;
    HANDLE wait_handle = nullptr;
    PTP_WAIT tp_wait = nullptr;
#endif
};

struct PortState {
    XTaskQueueDispatchMode mode = XTaskQueueDispatchMode_ThreadPool;
    XTaskQueuePort port_id = XTaskQueuePort_Work;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ScheduledCallback> items;
    uint64_t next_seq = 1;
    bool terminating = false;
    bool stop_workers = false;
    uint32_t active_callbacks = 0;
    std::vector<std::thread> workers;
};

}  // namespace

struct XTaskQueuePortObject {
    std::shared_ptr<PortState> state;
};

struct XTaskQueueObject {
    std::atomic<int32_t> ref_count{1};
    std::shared_ptr<PortState> ports[2];
    bool owns_port[2]{true, true};
    XTaskQueuePortObject port_handles[2];

    std::mutex monitor_mutex;
    uint64_t next_monitor_token = 1;
    std::vector<MonitorEntry> monitors;

    std::mutex waiter_mutex;
    uint64_t next_waiter_token = 1;
    std::vector<std::shared_ptr<WaiterEntry>> waiters;
    std::vector<std::shared_ptr<WaiterEntry>> retired_waiters;

    std::mutex terminate_mutex;
    std::atomic<bool> terminated{false};
    XTaskQueueTerminatedCallback terminated_cb = nullptr;
    void* terminated_ctx = nullptr;
    bool terminated_cb_fired = false;
};

namespace {

struct WaiterDispatchCtx {
    std::shared_ptr<WaiterEntry> entry;
};

#if defined(_WIN32)
thread_local PTP_WAIT g_current_tp_wait = nullptr;

void OPENSCARLETT_GDK_CALL WaiterDispatchTrampoline(void* context, bool canceled) {
    std::unique_ptr<WaiterDispatchCtx> holder(static_cast<WaiterDispatchCtx*>(context));
    auto entry = holder->entry;
    if (!entry) {
        return;
    }

    const bool still_active = entry->active.load(std::memory_order_acquire);
    if (entry->callback && (canceled || still_active)) {
        entry->callback(entry->context, canceled);
    }

    // Re-arm the one-shot threadpool wait AFTER the user callback finishes so both
    // auto-reset and manual-reset events fire cleanly on every signal without spin storms
    // or lost wakeups, until unregistered or queue termination.
    if (!canceled && entry->active.load(std::memory_order_acquire)) {
        auto q = entry->queue_weak.lock();
        if (q && !q->terminated.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(entry->wait_mutex);
            if (entry->active.load(std::memory_order_acquire) &&
                entry->tp_wait != nullptr &&
                entry->wait_handle != nullptr &&
                !q->terminated.load(std::memory_order_acquire)) {
                SetThreadpoolWait(entry->tp_wait, entry->wait_handle, nullptr);
            }
        }
    }
}

void CALLBACK WaiterTpCallback(
    PTP_CALLBACK_INSTANCE /*instance*/,
    PVOID param,
    PTP_WAIT wait,
    TP_WAIT_RESULT /*wait_result*/) {
    auto* raw_entry = static_cast<WaiterEntry*>(param);
    if (!raw_entry || !raw_entry->active.load(std::memory_order_acquire)) {
        return;
    }
    auto q = raw_entry->queue_weak.lock();
    if (!q || q->terminated.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<WaiterEntry> pinned;
    {
        std::lock_guard<std::mutex> lk(q->waiter_mutex);
        for (const auto& w : q->waiters) {
            if (w.get() == raw_entry) {
                pinned = w;
                break;
            }
        }
    }
    if (!pinned || !pinned->active.load(std::memory_order_acquire)) {
        return;
    }

    auto* dctx = new (std::nothrow) WaiterDispatchCtx{pinned};
    if (!dctx) {
        return;
    }

    PTP_WAIT prev_tp = g_current_tp_wait;
    g_current_tp_wait = wait;
    const XGameResult hr = XTaskQueueSubmitCallback(
        pinned->queue, pinned->port, dctx, &WaiterDispatchTrampoline);
    g_current_tp_wait = prev_tp;
    if (hr < 0) {
        delete dctx;
    }
}
#endif

void DisarmAndCloseWaiter(std::shared_ptr<WaiterEntry> w, XTaskQueueObject* obj) {
    if (!w) {
        return;
    }
    w->active.store(false, std::memory_order_release);
#if defined(_WIN32)
    PTP_WAIT tp = nullptr;
    {
        std::lock_guard<std::mutex> lk(w->wait_mutex);
        tp = w->tp_wait;
        w->tp_wait = nullptr;
        if (tp) {
            SetThreadpoolWait(tp, nullptr, nullptr);
        }
    }
    if (tp) {
        if (g_current_tp_wait == tp) {
            if (obj) {
                std::lock_guard<std::mutex> lk(obj->waiter_mutex);
                obj->retired_waiters.push_back(std::move(w));
            }
            CloseThreadpoolWait(tp);
        } else {
            WaitForThreadpoolWaitCallbacks(tp, TRUE);
            CloseThreadpoolWait(tp);
        }
    }
#else
    (void)obj;
#endif
}

size_t FindEarliestItemIndexLocked(const PortState& port) {
    size_t best = 0;
    for (size_t i = 1; i < port.items.size(); ++i) {
        if (port.items[i].ready_time < port.items[best].ready_time ||
            (port.items[i].ready_time == port.items[best].ready_time &&
             port.items[i].seq < port.items[best].seq)) {
            best = i;
        }
    }
    return best;
}

void NotifyMonitors(XTaskQueueObject* queue, XTaskQueuePort port) {
    std::vector<MonitorEntry> snapshot;
    {
        std::lock_guard<std::mutex> lk(queue->monitor_mutex);
        snapshot = queue->monitors;
    }
    for (const auto& m : snapshot) {
        if (m.callback) {
            m.callback(m.context, queue, port);
        }
    }
}

void PortWorkerLoop(std::shared_ptr<PortState> port_sp) {
    PortState& port = *port_sp;
    for (;;) {
        ScheduledCallback item{};
        bool canceled = false;
        {
            std::unique_lock<std::mutex> lk(port.mutex);
            for (;;) {
                if (port.terminating) {
                    if (port.items.empty()) {
                        return;
                    }
                    const size_t idx = FindEarliestItemIndexLocked(port);
                    item = port.items[idx];
                    port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = true;
                    ++port.active_callbacks;
                    break;
                }
                if (port.stop_workers) {
                    return;
                }
                if (port.items.empty()) {
                    port.cv.wait(lk);
                    continue;
                }
                const size_t idx = FindEarliestItemIndexLocked(port);
                const auto now = SteadyClock::now();
                if (port.items[idx].ready_time <= now) {
                    item = port.items[idx];
                    port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = false;
                    ++port.active_callbacks;
                    break;
                }
                port.cv.wait_until(lk, port.items[idx].ready_time);
            }
        }

        if (item.callback) {
            item.callback(item.context, canceled);
        }

        {
            std::lock_guard<std::mutex> lk(port.mutex);
            if (port.active_callbacks > 0) {
                --port.active_callbacks;
            }
            port.cv.notify_all();
        }
    }
}

void StartPortWorkersIfNeeded(XTaskQueueObject* queue, uint32_t port_idx) {
    if (!queue || !queue->owns_port[port_idx] || !queue->ports[port_idx]) {
        return;
    }
    PortState& port = *queue->ports[port_idx];
    uint32_t worker_count = 0;
    if (port.mode == XTaskQueueDispatchMode_ThreadPool) {
        worker_count = 2;
    } else if (port.mode == XTaskQueueDispatchMode_SerializedThreadPool) {
        worker_count = 1;
    }
    for (uint32_t i = 0; i < worker_count; ++i) {
        port.workers.emplace_back(&PortWorkerLoop, queue->ports[port_idx]);
    }
}

void ShutdownQueueWaiters(XTaskQueueObject* queue) {
    if (!queue) return;
    std::vector<std::shared_ptr<WaiterEntry>> active_waiters;
    {
        std::lock_guard<std::mutex> lk(queue->waiter_mutex);
        active_waiters.swap(queue->waiters);
    }
    for (auto& w : active_waiters) {
        DisarmAndCloseWaiter(std::move(w), queue);
    }
}

bool HasPendingManualPortItems(const XTaskQueueObject* queue) {
    if (!queue) return false;
    for (uint32_t p = 0; p < 2; ++p) {
        if (!queue->owns_port[p] || !queue->ports[p]) {
            continue;
        }
        PortState& port = *queue->ports[p];
        std::lock_guard<std::mutex> lk(port.mutex);
        if (port.workers.empty() && (!port.items.empty() || port.active_callbacks > 0)) {
            return true;
        }
    }
    return false;
}

void TryFireDeferredTerminatedCallback(XTaskQueueObject* queue) {
    if (!queue || !queue->terminated.load(std::memory_order_acquire)) {
        return;
    }
    if (HasPendingManualPortItems(queue)) {
        return;
    }
    XTaskQueueTerminatedCallback cb = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(queue->terminate_mutex);
        if (!queue->terminated_cb_fired && queue->terminated_cb != nullptr) {
            queue->terminated_cb_fired = true;
            cb = queue->terminated_cb;
            ctx = queue->terminated_ctx;
            queue->terminated_cb = nullptr;
            queue->terminated_ctx = nullptr;
        }
    }
    if (cb) {
        cb(ctx);
    }
}

void ShutdownQueueInternal(XTaskQueueObject* queue, bool wait_for_completion) {
    if (!queue) return;

    queue->terminated.store(true, std::memory_order_release);
    ShutdownQueueWaiters(queue);

    // When wait_for_completion is true (or during final handle destruction), synchronously
    // drain pending manual items as canceled. When wait_for_completion is false, leave
    // manual items queued so XTaskQueueDispatch drains them cleanly before returning false!
    std::vector<ScheduledCallback> manual_canceled;
    for (uint32_t p = 0; p < 2; ++p) {
        if (!queue->ports[p]) {
            continue;
        }
        PortState& port = *queue->ports[p];
        std::lock_guard<std::mutex> lk(port.mutex);
        if (queue->owns_port[p]) {
            port.terminating = true;
            if (wait_for_completion && port.workers.empty()) {
                while (!port.items.empty()) {
                    const size_t idx = FindEarliestItemIndexLocked(port);
                    manual_canceled.push_back(port.items[idx]);
                    port.items.erase(port.items.begin() + static_cast<std::ptrdiff_t>(idx));
                }
            }
        }
        port.cv.notify_all();
    }

    for (const auto& cb : manual_canceled) {
        if (cb.callback) {
            cb.callback(cb.context, true);
        }
    }

    if (wait_for_completion) {
        for (uint32_t p = 0; p < 2; ++p) {
            if (!queue->owns_port[p] || !queue->ports[p]) {
                continue;
            }
            PortState& port = *queue->ports[p];
            std::vector<std::thread> to_join;
            {
                std::lock_guard<std::mutex> lk(port.mutex);
                port.stop_workers = true;
                port.cv.notify_all();
                to_join.swap(port.workers);
            }
            for (auto& t : to_join) {
                if (t.joinable()) {
                    if (t.get_id() == std::this_thread::get_id()) {
                        t.detach();
                    } else {
                        t.join();
                    }
                }
            }
        }
    }

    TryFireDeferredTerminatedCallback(queue);
}

// ===========================================================================
// Global Runtime State (Queues, XAsync, XUser, XGameSave, XMem)
// ===========================================================================
struct AsyncRecord {
    uint64_t id = 0;
    XAsyncBlock* async_block = nullptr;
    XTaskQueueHandle queue = nullptr;
    void* provider_context = nullptr;
    const void* identity = nullptr;
    std::string identity_name;
    XAsyncProvider provider = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    bool result_ready = false;
    bool completed = false;
    std::thread::id callback_thread_id{};
    XGameResult result = XGR_E_PENDING;
    size_t required_buffer_size = 0;

    std::atomic<bool> complete_called{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> cleaned_up{false};
};

constexpr uint64_t kAsyncSlotMagic = 0x4F534153594E4331ULL;  // "OSASYNC1"

struct AsyncSlot {
    uint64_t magic;
    uint64_t id;
};
static_assert(sizeof(AsyncSlot) <= sizeof(XAsyncBlock::internal),
              "AsyncSlot must fit in XAsyncBlock::internal");

}  // namespace

struct XUser {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0;
    uint64_t local_id = 1;
    std::string gamertag_classic;
    std::string gamertag_modern;
    std::string gamertag_suffix;
    std::string gamertag_unique;
};

struct XGameSaveProvider {
    uint64_t xuid = 0;
    std::string scid;
    fs::path root_dir;
};

struct XGameSaveContainer {
    std::string name;
    fs::path container_dir;
};

struct XGameSaveUpdate {
    struct PendingOp {
        bool is_delete = false;
        std::string blob_name;
        std::vector<uint8_t> data;
    };

    std::string container_name;
    std::string display_name;
    fs::path container_dir;
    std::vector<PendingOp> ops;
};

struct XDisplayTimeoutDeferral {
    std::atomic<uint32_t> ref_count{1};
};

struct XSpeechSynthesizer {
    std::string voice_id = "openscarlett-en-us-aria";
    bool is_male = false;
};

struct XSpeechSynthesizerStream {
    std::vector<uint8_t> wav_bytes;
};

struct XAppCaptureScreenshotStream {
    std::vector<uint8_t> bytes;
};

struct XUserSignOutDeferral {
    std::atomic<uint32_t> active{1};
};

struct XStoreContext {
    uint64_t xuid = 0;
};

struct XStoreLicense {
    std::string identifier;
    bool valid = true;
};

struct XStoreProductBacking {
    std::string store_id;
    std::string title;
    std::string description;
    std::string language;
    std::string offer_token;
    std::string link_uri;
    std::string currency_code;
    std::string sku_id;
    std::string availability_id;
    XStoreAvailability availability{};
    XStoreSku sku{};
    XStoreProduct product{};
};

struct XStoreProductQuery {
    std::deque<XStoreProductBacking> products;
};

struct XPackageMountObject {
    std::string package_id;
    std::string mount_path;
};

struct XPackageInstallationMonitor {
    std::string package_id;
    uint64_t total_bytes = 1024ULL * 1024ULL;
};

namespace {

struct InviteListenerEntry {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XGameInviteEventCallback callback = nullptr;
};

enum RegFamily : uint32_t {
    kRegUserChange = 1,
    kRegUserAudioEndpoint = 2,
    kRegUserDeviceAssoc = 3,
    kRegStoreGameLicense = 4,
    kRegStorePackageLicense = 5,
    kRegNetConnectivity = 6,
    kRegNetUdpPort = 7,
    kRegPackageProgress = 8,
    kRegPackageInstalled = 9,
    kRegGameInvite = 10,
    kRegAppBroadcast = 11,
    kRegAppCaptureMetadataPurged = 12,
    kRegGameStreamingConnection = 13,
};

thread_local bool g_thread_time_sensitive = false;

struct UserChangeListenerEntry {
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XUserChangeEventCallback callback = nullptr;
};

struct RuntimeState {
    std::mutex mutex;
    std::atomic<int32_t> init_count{0};
    std::atomic<int32_t> api_impl_init_count{0};
    std::atomic<uint32_t> vtable_refs[16]{
        1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};

    // Task queues
    std::unordered_map<XTaskQueueObject*, std::shared_ptr<XTaskQueueObject>> live_queues;
    XTaskQueueHandle process_queue = nullptr;

    // XAsync records
    std::mutex async_mutex;
    uint64_t next_async_id = 1;
    std::unordered_map<uint64_t, std::shared_ptr<AsyncRecord>> async_records;

    // XUser handles
    std::mutex user_mutex;
    std::unordered_map<XUser*, std::shared_ptr<XUser>> live_users;

    // XGameSave handles
    std::mutex save_mutex;
    std::unordered_map<XGameSaveProvider*, std::shared_ptr<XGameSaveProvider>> live_providers;
    std::unordered_map<XGameSaveContainer*, std::shared_ptr<XGameSaveContainer>> live_containers;
    std::unordered_map<XGameSaveUpdate*, std::shared_ptr<XGameSaveUpdate>> live_updates;

    // XMem hooks
    std::mutex mem_mutex;
    XMemAllocFunction alloc_hook = nullptr;
    XMemFreeFunction free_hook = nullptr;

    // XDisplay timeout deferrals
    std::mutex display_mutex;
    std::unordered_set<XDisplayTimeoutDeferral*> live_deferrals;

    // XLauncher / XGameUI
    std::mutex ui_mutex;
    std::string last_launched_uri;
    XGameUINotificationPositionHint notification_pos =
        XGameUINotificationPositionHint_BottomCenter;

    // XGameInvite
    std::mutex invite_mutex;
    uint64_t next_invite_token = 1;
    std::vector<InviteListenerEntry> invite_listeners;

    // XAccessibility
    std::mutex accessibility_mutex;
    bool closed_captions_enabled = false;

    // XSpeechSynthesizer
    std::mutex speech_mutex;
    std::unordered_map<XSpeechSynthesizer*, std::shared_ptr<XSpeechSynthesizer>> live_synths;
    std::unordered_map<XSpeechSynthesizerStream*, std::shared_ptr<XSpeechSynthesizerStream>> live_streams;

    // Registration tokens (shared across XUser, XStore, XNetworking, XPackage)
    std::mutex reg_mutex;
    uint64_t next_reg_token = 1;
    std::unordered_map<uint64_t, uint32_t> live_reg_tokens;
    std::vector<UserChangeListenerEntry> user_change_listeners;

    // XStore handles
    std::mutex store_mutex;
    std::unordered_map<XStoreContext*, std::shared_ptr<XStoreContext>> live_store_contexts;
    std::unordered_map<XStoreLicense*, std::shared_ptr<XStoreLicense>> live_store_licenses;
    std::unordered_map<XStoreProductQuery*, std::shared_ptr<XStoreProductQuery>> live_store_queries;

    // XPackage handles
    std::mutex package_mutex;
    std::unordered_map<XPackageMountObject*, std::shared_ptr<XPackageMountObject>> live_package_mounts;
    std::unordered_map<XPackageInstallationMonitor*, std::shared_ptr<XPackageInstallationMonitor>>
        live_install_monitors;

    // XGameUi & XError state
    std::atomic<uint32_t> ui_notification_hint{
        static_cast<uint32_t>(XGameUiNotificationPositionHint_BottomCenter)};

    std::mutex error_mutex;
    XErrorCallback error_callback = nullptr;
    void* error_callback_ctx = nullptr;
    XErrorOptions error_opts_debugger = XErrorOptions_None;
    XErrorOptions error_opts_no_debugger = XErrorOptions_None;

    // XAppBroadcast / XAppCapture state
    std::mutex capture_mutex;
    std::atomic<bool> capture_record_enabled{true};
    uint64_t next_screenshot_id = 1;
    std::unordered_map<std::string, std::vector<uint8_t>> capture_screenshots;
    std::unordered_map<XAppCaptureScreenshotStream*, std::shared_ptr<XAppCaptureScreenshotStream>>
        live_capture_streams;
    std::unordered_map<std::string, std::string> capture_metadata_states;
    uint64_t capture_metadata_bytes_used = 0;

    // XSpeechToText state
    std::mutex stt_mutex;
    XSpeechToTextPositionHint stt_position_hint = XSpeechToTextPositionHint_BottomCenter;
    uint32_t next_stt_hypothesis_id = 1;

    // XGameStreaming state
    std::mutex streaming_mutex;
    uint32_t streaming_init_count = 0;
    std::string active_touch_layout;
    std::unordered_map<uint32_t, std::string> active_stt_hypotheses;
};

RuntimeState& GetRuntime() {
    static RuntimeState* s = new RuntimeState();
    return *s;
}

std::shared_ptr<XTaskQueueObject> LookupQueue(XTaskQueueHandle handle) {
    if (!handle) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.mutex);
    auto it = rt.live_queues.find(handle);
    return it == rt.live_queues.end() ? nullptr : it->second;
}

std::shared_ptr<AsyncRecord> LookupAsync(const XAsyncBlock* block) {
    if (!block) return nullptr;
    AsyncSlot slot{};
    std::memcpy(&slot, block->internal, sizeof(slot));
    if (slot.magic != kAsyncSlotMagic || slot.id == 0) {
        return nullptr;
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.async_mutex);
    auto it = rt.async_records.find(slot.id);
    return it == rt.async_records.end() ? nullptr : it->second;
}

void CleanupAsyncRecordOnce(const std::shared_ptr<AsyncRecord>& rec) {
    if (!rec) return;
    if (!rec->cleaned_up.exchange(true, std::memory_order_acq_rel)) {
        if (rec->provider) {
            XAsyncProviderData data{};
            data.async = rec->async_block;
            data.context = rec->provider_context;
            rec->provider(XAsyncOp_Cleanup, &data);
        }
        if (rec->queue) {
            XTaskQueueCloseHandle(rec->queue);
            rec->queue = nullptr;
        }
    }
}

struct AsyncTrampolineCtx {
    std::shared_ptr<AsyncRecord> rec;
};

void OPENSCARLETT_GDK_CALL AsyncCompletionTrampoline(void* context, bool /*canceled*/) {
    std::unique_ptr<AsyncTrampolineCtx> holder(static_cast<AsyncTrampolineCtx*>(context));
    auto rec = holder->rec;

    XAsyncCompletionRoutine cb = nullptr;
    XAsyncBlock* block = nullptr;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->result_ready = true;
        rec->callback_thread_id = std::this_thread::get_id();
        block = rec->async_block;
        cb = block ? block->callback : nullptr;
    }

    if (cb && block) {
        cb(block);
    }

    bool should_auto_cleanup = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->callback_thread_id = std::thread::id{};
        rec->completed = true;
        should_auto_cleanup = (rec->required_buffer_size == 0 || rec->result < 0);
    }
    rec->cv.notify_all();

    if (should_auto_cleanup) {
        CleanupAsyncRecordOnce(rec);
    }
}

void OPENSCARLETT_GDK_CALL AsyncWorkTrampoline(void* context, bool canceled) {
    std::unique_ptr<AsyncTrampolineCtx> holder(static_cast<AsyncTrampolineCtx*>(context));
    auto rec = holder->rec;

    if (canceled || rec->canceled.load(std::memory_order_acquire)) {
        XAsyncComplete(rec->async_block, XGR_E_ABORT, 0);
        return;
    }

    XAsyncProviderData data{};
    data.async = rec->async_block;
    data.context = rec->provider_context;
    const XGameResult hr = rec->provider ? rec->provider(XAsyncOp_DoWork, &data) : XGR_E_FAIL;
    if (hr != XGR_E_PENDING && !rec->complete_called.load(std::memory_order_acquire)) {
        XAsyncComplete(rec->async_block, hr, 0);
    }
}

// ===========================================================================
// XGameSave Metadata Helpers
// ===========================================================================
struct ContainerMeta {
    std::string name;
    std::string display_name;
    int64_t last_modified = 0;
    // original blob name -> filename on disk (relative to container_dir)
    std::map<std::string, std::string> blobs;
};

bool LoadContainerMeta(const fs::path& container_dir, ContainerMeta& out) {
    std::ifstream ifs(container_dir / "_meta.txt", std::ios::binary);
    if (!ifs) return false;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("name=", 0) == 0) {
            out.name = UnescapeLine(line.substr(5));
        } else if (line.rfind("display=", 0) == 0) {
            out.display_name = UnescapeLine(line.substr(8));
        } else if (line.rfind("mtime=", 0) == 0) {
            try {
                out.last_modified = std::stoll(line.substr(6));
            } catch (...) {
                out.last_modified = 0;
            }
        } else if (line.rfind("blob=", 0) == 0) {
            std::string rest = line.substr(5);
            const size_t tab = rest.find('\t');
            if (tab != std::string::npos) {
                std::string file_name = rest.substr(0, tab);
                std::string blob_name = UnescapeLine(rest.substr(tab + 1));
                out.blobs[blob_name] = file_name;
            }
        }
    }
    return !out.name.empty();
}

bool SaveContainerMeta(const fs::path& container_dir, const ContainerMeta& meta) {
    std::ostringstream oss;
    oss << "name=" << EscapeLine(meta.name) << "\n";
    oss << "display=" << EscapeLine(meta.display_name) << "\n";
    oss << "mtime=" << meta.last_modified << "\n";
    for (const auto& [blob_name, file_name] : meta.blobs) {
        oss << "blob=" << file_name << "\t" << EscapeLine(blob_name) << "\n";
    }
    const std::string text = oss.str();
    return WriteBinaryFileAtomic(
        container_dir / "_meta.txt",
        reinterpret_cast<const uint8_t*>(text.data()),
        text.size());
}

std::shared_ptr<XUser> LookupUser(XUserHandle handle) {
    if (!handle) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    auto it = rt.live_users.find(handle);
    return it == rt.live_users.end() ? nullptr : it->second;
}

uint64_t DefaultUserXuid() {
    std::string env = GetEnvString("OPENSCARLETT_XUID");
    if (!env.empty()) {
        try {
            return std::stoull(env, nullptr, 0);
        } catch (...) {
        }
    }
    return 0x0009000000001337ULL;
}

std::shared_ptr<XUser> CreateDefaultUserObj() {
    auto u = std::make_shared<XUser>();
    u->xuid = DefaultUserXuid();
    u->local_id = 1;
    std::string env_gt = GetEnvString("OPENSCARLETT_GAMERTAG");
    if (!env_gt.empty()) {
        u->gamertag_classic = env_gt;
        u->gamertag_modern = env_gt;
    } else {
        u->gamertag_classic = "ScarlettPlayer";
        u->gamertag_modern = "ScarlettPlayer";
    }
    u->gamertag_suffix = "1337";
    u->gamertag_unique = u->gamertag_modern + "#" + u->gamertag_suffix;
    return u;
}

std::string Base64UrlEncode(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((input.size() + 2u) / 3u) * 4u);
    size_t i = 0;
    while (i + 3u <= input.size()) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1u])) << 8) |
                           static_cast<uint32_t>(static_cast<unsigned char>(input[i + 2u]));
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
        out.push_back(kTable[(v >> 6) & 0x3Fu]);
        out.push_back(kTable[v & 0x3Fu]);
        i += 3u;
    }
    const size_t rem = input.size() - i;
    if (rem == 1u) {
        const uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16;
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
    } else if (rem == 2u) {
        const uint32_t v = (static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
                           (static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1u])) << 8);
        out.push_back(kTable[(v >> 18) & 0x3Fu]);
        out.push_back(kTable[(v >> 12) & 0x3Fu]);
        out.push_back(kTable[(v >> 6) & 0x3Fu]);
    }
    return out;
}

void BuildOfflineXstsTokenAndSig(
    const XUser& user,
    const std::string& method,
    const std::string& url,
    std::string& out_token,
    std::string& out_sig) {
    const int64_t now = NowUnixSeconds();
    const int64_t exp = now + 86400;
    const std::string hdr_json = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    std::ostringstream payload;
    payload << "{\"xuid\":\"" << user.xuid
            << "\",\"uhs\":\"0000000000000001\""
            << ",\"gtg\":\"" << user.gamertag_classic
            << "\",\"agg\":\"Adult\""
            << ",\"iat\":" << now
            << ",\"exp\":" << exp
            << ",\"iss\":\"https://xsts.auth.xboxlive.com\",\"offline\":true}";
    const std::string sig_material =
        "OpenScarlett-Offline-XSTS-Sig-V1:" + FormatXuidHex(user.xuid);
    const std::string jwt =
        Base64UrlEncode(hdr_json) + "." +
        Base64UrlEncode(payload.str()) + "." +
        Base64UrlEncode(sig_material);
    out_token = "XBL3.0 x=0000000000000001;" + jwt;
    out_sig = Base64UrlEncode("XBL3.0-SIG:" + method + ":" + url + ":" + FormatXuidHex(user.xuid));
}

// ===========================================================================
// Async Provider Helpers for XUserAddAsync & XGameSaveInitializeProviderAsync
// ===========================================================================
const char kIdUserAdd[] = "XUserAddAsync";
const char kIdGameSaveInit[] = "XGameSaveInitializeProviderAsync";

struct UserAddAsyncCtx {
    XUserHandle user = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL UserAddProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<UserAddAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            auto u = CreateDefaultUserObj();
            XUserHandle raw = u.get();
            {
                auto& rt = GetRuntime();
                std::lock_guard<std::mutex> lk(rt.user_mutex);
                rt.live_users[raw] = std::move(u);
            }
            ctx->user = raw;
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XUserHandle));
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XUserHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XUserHandle h = ctx->user;
            ctx->user = nullptr;
            std::memcpy(data->buffer, &h, sizeof(h));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->user) {
                XUserCloseHandle(ctx->user);
                ctx->user = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameSaveInitAsyncCtx {
    XUserHandle requesting_user = nullptr;
    std::string scid;
    bool sync_on_demand = false;
    XGameSaveProviderHandle provider = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveInitProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveInitAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            XGameSaveProviderHandle p = nullptr;
            XGameResult hr = XGameSaveInitializeProvider(
                ctx->requesting_user,
                ctx->scid.c_str(),
                ctx->sync_on_demand,
                &p);
            ctx->provider = p;
            XAsyncComplete(data->async, hr, hr >= 0 ? sizeof(XGameSaveProviderHandle) : 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XGameSaveProviderHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XGameSaveProviderHandle p = ctx->provider;
            ctx->provider = nullptr;
            std::memcpy(data->buffer, &p, sizeof(p));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->provider) {
                XGameSaveCloseProvider(ctx->provider);
                ctx->provider = nullptr;
            }
            if (ctx->requesting_user) {
                XUserCloseHandle(ctx->requesting_user);
                ctx->requesting_user = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

// ===========================================================================
// Async Provider Helpers for XAsyncRun, XUserGetGamerPictureAsync, XGameSave*Async
// ===========================================================================
const char kIdAsyncRun[] = "XAsyncRun";
const char kIdUserGamerPicture[] = "XUserGetGamerPictureAsync";
const char kIdGameSaveReadBlob[] = "XGameSaveReadBlobDataAsync";
const char kIdGameSaveSubmitUpdate[] = "XGameSaveSubmitUpdateAsync";
const char kIdGameSaveDeleteContainer[] = "XGameSaveDeleteContainerAsync";
const char kIdGameSaveQuota[] = "XGameSaveGetRemainingQuotaAsync";

struct AsyncRunCtx {
    XAsyncWork work = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL AsyncRunProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<AsyncRunCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            const XGameResult hr = (ctx && ctx->work) ? ctx->work(data->async) : XGR_E_INVALIDARG;
            XAsyncComplete(data->async, hr, 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

uint32_t PngCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    AppendBe32(out, static_cast<uint32_t>(payload.size()));
    const size_t type_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = PngCrc32(out.data() + type_start, 4u + payload.size());
    AppendBe32(out, crc);
}

std::vector<uint8_t> BuildDefaultGamerPicPng(uint32_t dim) {
    // Uncompressed zlib-stored RGBA PNG (100% valid RFC 2083 + RFC 1950).
    std::vector<uint8_t> png = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    std::vector<uint8_t> ihdr;
    AppendBe32(ihdr, dim);
    AppendBe32(ihdr, dim);
    ihdr.push_back(8u);  // bit depth
    ihdr.push_back(6u);  // RGBA
    ihdr.push_back(0u);  // compression
    ihdr.push_back(0u);  // filter
    ihdr.push_back(0u);  // interlace
    AppendPngChunk(png, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(dim) * (1u + static_cast<size_t>(dim) * 4u));
    for (uint32_t y = 0; y < dim; ++y) {
        raw.push_back(0u);  // filter type None
        for (uint32_t x = 0; x < dim; ++x) {
            raw.push_back(16u);
            raw.push_back(124u);
            raw.push_back(16u);
            raw.push_back(255u);
        }
    }

    uint32_t a = 1u, b = 0u;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    const uint32_t adler = (b << 16) | a;

    std::vector<uint8_t> idat;
    idat.push_back(0x78u);  // zlib CMF (deflate, 32K window)
    idat.push_back(0x01u);  // zlib FLG (check bits)
    const uint16_t len16 = static_cast<uint16_t>(raw.size());
    const uint16_t nlen16 = static_cast<uint16_t>(~len16);
    idat.push_back(0x01u);  // BFINAL=1, BTYPE=00 (stored)
    idat.push_back(static_cast<uint8_t>(len16 & 0xFFu));
    idat.push_back(static_cast<uint8_t>((len16 >> 8) & 0xFFu));
    idat.push_back(static_cast<uint8_t>(nlen16 & 0xFFu));
    idat.push_back(static_cast<uint8_t>((nlen16 >> 8) & 0xFFu));
    idat.insert(idat.end(), raw.begin(), raw.end());
    AppendBe32(idat, adler);
    AppendPngChunk(png, "IDAT", idat);

    AppendPngChunk(png, "IEND", {});
    return png;
}

struct UserGamerPicAsyncCtx {
    XUserGamerPictureSize size_enum = XUserGamerPictureSize_Medium;
    std::vector<uint8_t> png_bytes;
};

XGameResult OPENSCARLETT_GDK_CALL UserGamerPicProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<UserGamerPicAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            uint32_t dim = 8u;
            if (ctx->size_enum == XUserGamerPictureSize_Small) dim = 4u;
            else if (ctx->size_enum == XUserGamerPictureSize_Large) dim = 16u;
            else if (ctx->size_enum == XUserGamerPictureSize_ExtraLarge) dim = 16u;
            ctx->png_bytes = BuildDefaultGamerPicPng(dim);
            XAsyncComplete(data->async, XGR_S_OK, ctx->png_bytes.size());
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < ctx->png_bytes.size()) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            if (!ctx->png_bytes.empty()) {
                std::memcpy(data->buffer, ctx->png_bytes.data(), ctx->png_bytes.size());
            }
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct LoadedGameSaveBlob {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct GameSaveReadBlobAsyncCtx {
    fs::path container_dir;
    std::vector<std::string> requested_names;
    std::vector<LoadedGameSaveBlob> loaded;
    size_t required_bytes = 0;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveReadBlobProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveReadBlobAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            ContainerMeta meta{};
            if (!LoadContainerMeta(ctx->container_dir, meta)) {
                XAsyncComplete(data->async, XGR_E_GS_BLOB_NOT_FOUND, 0);
                return XGR_E_PENDING;
            }
            std::vector<std::string> names = ctx->requested_names;
            if (names.empty()) {
                for (const auto& [bname, _] : meta.blobs) {
                    names.push_back(bname);
                }
            }
            ctx->loaded.clear();
            ctx->loaded.reserve(names.size());
            size_t req = names.size() * sizeof(XGameSaveBlob);
            for (const auto& req_name : names) {
                auto mit = meta.blobs.find(req_name);
                if (mit == meta.blobs.end()) {
                    XAsyncComplete(data->async, XGR_E_GS_BLOB_NOT_FOUND, 0);
                    return XGR_E_PENDING;
                }
                LoadedGameSaveBlob lb{};
                lb.name = req_name;
                if (!ReadBinaryFile(ctx->container_dir / mit->second, lb.bytes)) {
                    XAsyncComplete(data->async, XGR_E_GS_BLOB_NOT_FOUND, 0);
                    return XGR_E_PENDING;
                }
                req += (lb.name.size() + 1) + lb.bytes.size();
                ctx->loaded.push_back(std::move(lb));
            }
            ctx->required_bytes = req;
            XAsyncComplete(data->async, XGR_S_OK, req);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < ctx->required_bytes) {
                return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
            }
            auto* blob_data = reinterpret_cast<XGameSaveBlob*>(data->buffer);
            auto* base = reinterpret_cast<uint8_t*>(data->buffer);
            size_t cursor = ctx->loaded.size() * sizeof(XGameSaveBlob);
            for (size_t i = 0; i < ctx->loaded.size(); ++i) {
                char* name_dst = reinterpret_cast<char*>(base + cursor);
                std::memcpy(name_dst, ctx->loaded[i].name.c_str(), ctx->loaded[i].name.size() + 1);
                cursor += ctx->loaded[i].name.size() + 1;

                uint8_t* data_dst = base + cursor;
                if (!ctx->loaded[i].bytes.empty()) {
                    std::memcpy(data_dst, ctx->loaded[i].bytes.data(), ctx->loaded[i].bytes.size());
                    cursor += ctx->loaded[i].bytes.size();
                }

                blob_data[i].info.name = name_dst;
                blob_data[i].info.size = static_cast<uint32_t>(ctx->loaded[i].bytes.size());
                blob_data[i].data = ctx->loaded[i].bytes.empty() ? nullptr : data_dst;
            }
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameSaveSubmitUpdateAsyncCtx {
    XGameSaveUpdateHandle update = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveSubmitUpdateProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveSubmitUpdateAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            const XGameResult hr = XGameSaveSubmitUpdate(ctx->update);
            XAsyncComplete(data->async, hr, 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameSaveDeleteContainerAsyncCtx {
    XGameSaveProviderHandle provider = nullptr;
    std::string name;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveDeleteContainerProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveDeleteContainerAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            const XGameResult hr = XGameSaveDeleteContainer(ctx->provider, ctx->name.c_str());
            XAsyncComplete(data->async, hr, 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameSaveQuotaAsyncCtx {
    XGameSaveProviderHandle provider = nullptr;
    int64_t quota = 0;
};

XGameResult OPENSCARLETT_GDK_CALL GameSaveQuotaProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameSaveQuotaAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            const XGameResult hr = XGameSaveGetRemainingQuota(ctx->provider, &ctx->quota);
            XAsyncComplete(data->async, hr, hr >= 0 ? sizeof(int64_t) : 0);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(int64_t)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &ctx->quota, sizeof(int64_t));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

// ===========================================================================
// Shared Registration Token, Package & Store Helpers
// ===========================================================================
XTaskQueueRegistrationToken AllocateRegToken(uint32_t family) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.reg_mutex);
    const uint64_t tok = rt.next_reg_token++;
    rt.live_reg_tokens[tok] = family;
    return XTaskQueueRegistrationToken{tok};
}

bool ReleaseRegToken(uint32_t family, XTaskQueueRegistrationToken token) {
    if (token.token == 0) {
        return false;
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.reg_mutex);
    auto it = rt.live_reg_tokens.find(token.token);
    if (it == rt.live_reg_tokens.end() || it->second != family) {
        return false;
    }
    rt.live_reg_tokens.erase(it);
    return true;
}

std::string CurrentPackageIdentifier() {
    std::string pkg_id = GetEnvString("OPENSCARLETT_PACKAGE_ID");
    if (!pkg_id.empty()) {
        return pkg_id;
    }
    const auto lsa_pkg = openscarlett::ConsoleLsaPolicy::Global().GetPackageIdentity();
    if (!lsa_pkg.package_full_name.empty() &&
        lsa_pkg.package_name != "OpenScarlett.DefaultTitle") {
        return lsa_pkg.package_full_name;
    }
    return "OpenScarlett.Title_1.0.0.0_x64__openscarlett0";
}

std::string CurrentPackageMountPath() {
    std::string env_root = GetEnvString("OPENSCARLETT_PACKAGE_ROOT");
    if (!env_root.empty()) {
        return env_root;
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty()) {
        return cwd.string();
    }
    return "E:\\OpenScarlett";
}

constexpr const char* kDefaultStoreId = "9OS000000001";
constexpr const char* kDefaultSkuStoreId = "9OS000000001/0001";

struct GameConfigMetadata {
    bool found = false;
    bool has_title_id = false;
    uint32_t title_id = 0;
    std::string store_id;
    std::string identity_name;
    std::string display_name;
};

std::string TrimAscii(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string StripXmlComments(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    size_t pos = 0;
    while (pos < xml.size()) {
        const size_t start = xml.find("<!--", pos);
        if (start == std::string::npos) {
            out.append(xml, pos, std::string::npos);
            break;
        }
        out.append(xml, pos, start - pos);
        const size_t end = xml.find("-->", start + 4);
        if (end == std::string::npos) {
            break;
        }
        pos = end + 3;
    }
    return out;
}

std::string ExtractXmlElementText(const std::string& xml, const char* tag) {
    const std::string open_prefix = std::string("<") + tag;
    const std::string close_tag = std::string("</") + tag + ">";
    std::string lower_xml = xml;
    for (char& c : lower_xml) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string lower_open = open_prefix;
    for (char& c : lower_open) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string lower_close = close_tag;
    for (char& c : lower_close) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    size_t pos = 0;
    while ((pos = lower_xml.find(lower_open, pos)) != std::string::npos) {
        const size_t after = pos + lower_open.size();
        if (after < lower_xml.size() &&
            lower_xml[after] != '>' && lower_xml[after] != ' ' &&
            lower_xml[after] != '\t' && lower_xml[after] != '\r' &&
            lower_xml[after] != '\n' && lower_xml[after] != '/') {
            pos = after;
            continue;
        }
        const size_t gt = lower_xml.find('>', after);
        if (gt == std::string::npos) break;
        if (gt > pos && lower_xml[gt - 1] == '/') {
            pos = gt + 1;
            continue;
        }
        const size_t close_pos = lower_xml.find(lower_close, gt + 1);
        if (close_pos == std::string::npos) break;
        return TrimAscii(xml.substr(gt + 1, close_pos - (gt + 1)));
    }
    return {};
}

std::string ExtractXmlAttribute(const std::string& xml, const char* tag, const char* attr) {
    std::string lower_xml = xml;
    for (char& c : lower_xml) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string lower_open = std::string("<") + tag;
    for (char& c : lower_open) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string lower_attr = std::string(attr) + "=\"";
    for (char& c : lower_attr) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const size_t pos = lower_xml.find(lower_open);
    if (pos == std::string::npos) return {};
    const size_t gt = lower_xml.find('>', pos);
    if (gt == std::string::npos) return {};
    const size_t ap = lower_xml.find(lower_attr, pos);
    if (ap == std::string::npos || ap >= gt) return {};
    const size_t val_start = ap + lower_attr.size();
    const size_t val_end = xml.find('"', val_start);
    if (val_end == std::string::npos || val_end > gt) return {};
    return TrimAscii(xml.substr(val_start, val_end - val_start));
}

bool TryFindGameConfigInDir(const fs::path& dir, fs::path& out_path) {
    if (dir.empty()) return false;
    std::error_code ec;
    const fs::path p1 = dir / "MicrosoftGame.Config";
    if (fs::is_regular_file(p1, ec)) {
        out_path = p1;
        return true;
    }
    ec.clear();
    const fs::path p2 = dir / "MicrosoftGame.config";
    if (fs::is_regular_file(p2, ec)) {
        out_path = p2;
        return true;
    }
    return false;
}

GameConfigMetadata LoadGameConfigMetadata() {
    GameConfigMetadata meta{};
    fs::path cfg_path;
    std::error_code ec;

    const std::string env_cfg = TrimAscii(GetEnvString("OPENSCARLETT_GAME_CONFIG"));
    if (!env_cfg.empty()) {
        const fs::path p(env_cfg);
        if (fs::is_regular_file(p, ec)) {
            cfg_path = p;
        } else {
            ec.clear();
            if (fs::is_directory(p, ec)) {
                (void)TryFindGameConfigInDir(p, cfg_path);
            }
        }
    }

    if (cfg_path.empty()) {
        const fs::path cwd = fs::current_path(ec);
        if (!ec && !cwd.empty()) {
            (void)TryFindGameConfigInDir(cwd, cfg_path);
        }
    }

#if defined(_WIN32)
    if (cfg_path.empty()) {
        wchar_t exe_buf[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            fs::path cur = fs::path(exe_buf).parent_path();
            for (int depth = 0; depth < 3 && !cur.empty(); ++depth) {
                if (TryFindGameConfigInDir(cur, cfg_path)) {
                    break;
                }
                const fs::path parent = cur.parent_path();
                if (parent == cur) break;
                cur = parent;
            }
        }
    }
#endif

    if (cfg_path.empty()) {
        return meta;
    }

    std::ifstream ifs(cfg_path, std::ios::binary);
    if (!ifs) {
        return meta;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    const std::string stripped = StripXmlComments(oss.str());
    meta.found = true;

    const std::string tid_str = ExtractXmlElementText(stripped, "TitleId");
    if (!tid_str.empty() && tid_str[0] != '-') {
        char* end_ptr = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(tid_str.c_str(), &end_ptr, 16);
        if (errno == 0 && end_ptr && *end_ptr == '\0' && parsed <= 0xFFFFFFFFULL) {
            meta.has_title_id = true;
            meta.title_id = static_cast<uint32_t>(parsed);
        }
    }

    meta.store_id = ExtractXmlElementText(stripped, "StoreId");
    meta.identity_name = ExtractXmlAttribute(stripped, "Identity", "Name");
    meta.display_name = ExtractXmlAttribute(stripped, "ShellVisuals", "DefaultDisplayName");
    return meta;
}

void SyncConsoleLsaPolicyFromConfig() {
    fs::path cfg_path;
    std::error_code ec;

    const std::string env_cfg = TrimAscii(GetEnvString("OPENSCARLETT_GAME_CONFIG"));
    if (!env_cfg.empty()) {
        const fs::path p(env_cfg);
        if (fs::is_regular_file(p, ec)) {
            cfg_path = p;
        } else {
            ec.clear();
            if (fs::is_directory(p, ec)) {
                (void)TryFindGameConfigInDir(p, cfg_path);
            }
        }
    }
    if (cfg_path.empty()) {
        const fs::path cwd = fs::current_path(ec);
        if (!ec && !cwd.empty()) {
            (void)TryFindGameConfigInDir(cwd, cfg_path);
        }
    }
#if defined(_WIN32)
    if (cfg_path.empty()) {
        wchar_t exe_buf[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            fs::path cur = fs::path(exe_buf).parent_path();
            for (int depth = 0; depth < 3 && !cur.empty(); ++depth) {
                if (TryFindGameConfigInDir(cur, cfg_path)) {
                    break;
                }
                const fs::path parent = cur.parent_path();
                if (parent == cur) break;
                cur = parent;
            }
        }
    }
#endif

    auto& lsa = openscarlett::ConsoleLsaPolicy::Global();
    if (!cfg_path.empty()) {
        (void)lsa.LoadFromMicrosoftGameConfigFile(cfg_path.string());
    }
    const std::string env_sandbox = TrimAscii(GetEnvString("OPENSCARLETT_SANDBOX_ID"));
    if (!env_sandbox.empty()) {
        lsa.SetSandboxId(env_sandbox);
    }
    const std::string env_store = TrimAscii(GetEnvString("OPENSCARLETT_STORE_ID"));
    if (!env_store.empty()) {
        lsa.SetStoreId(env_store);
    }
    const std::string env_content = TrimAscii(GetEnvString("OPENSCARLETT_CONTENT_ID"));
    if (!env_content.empty()) {
        lsa.SetContentId(env_content);
    }
    const std::string env_tid = TrimAscii(GetEnvString("OPENSCARLETT_TITLE_ID"));
    if (!env_tid.empty() && env_tid[0] != '-') {
        char* end_ptr = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(env_tid.c_str(), &end_ptr, 16);
        if (errno == 0 && end_ptr && *end_ptr == '\0' && parsed <= 0xFFFFFFFFULL) {
            lsa.SetTitleId(static_cast<uint32_t>(parsed));
        }
    }
}

std::string ToLowerAsciiStr(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::vector<std::string> SplitDelimitedTokens(std::string_view input, char delim) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= input.size()) {
        size_t sep = input.find(delim, pos);
        if (sep == std::string_view::npos) {
            sep = input.size();
        }
        std::string tok = TrimAscii(std::string(input.substr(pos, sep - pos)));
        if (!tok.empty()) {
            out.push_back(std::move(tok));
        }
        if (sep == input.size()) {
            break;
        }
        pos = sep + 1;
    }
    return out;
}

std::vector<std::string> ActiveInstalledLanguagesLower() {
    std::vector<std::string> langs;
    const std::string env_langs = GetEnvString("OPENSCARLETT_INSTALLED_LANGUAGES");
    if (!env_langs.empty()) {
        std::string normalized = env_langs;
        for (char& c : normalized) {
            if (c == ',') c = ';';
        }
        for (const auto& tok : SplitDelimitedTokens(normalized, ';')) {
            langs.push_back(ToLowerAsciiStr(tok));
        }
    }
    if (langs.empty()) {
        std::string loc = TrimAscii(GetEnvString("OPENSCARLETT_LOCALE"));
        if (loc.empty()) {
            loc = "en-US";
        }
        langs.push_back(ToLowerAsciiStr(loc));
    }
    return langs;
}

bool LanguageTokenMatchesInstalled(std::string_view token,
                                   const std::vector<std::string>& installed_lower) {
    const std::string tlo = ToLowerAsciiStr(TrimAscii(std::string(token)));
    if (tlo.empty()) {
        return false;
    }
    const size_t t_dash = tlo.find('-');
    const std::string t_primary = (t_dash == std::string::npos) ? tlo : tlo.substr(0, t_dash);

    for (const auto& inst : installed_lower) {
        if (inst == tlo) {
            return true;
        }
        // Match primary language tag when either side specifies base language only (e.g. "en" <-> "en-us").
        const size_t i_dash = inst.find('-');
        const std::string i_primary = (i_dash == std::string::npos) ? inst : inst.substr(0, i_dash);
        if ((t_dash == std::string::npos || i_dash == std::string::npos) && t_primary == i_primary) {
            return true;
        }
    }
    return false;
}

XGameResult ValidateAndEvaluateSelectors(uint32_t selectorCount,
                                         const XPackageChunkSelector* selectors,
                                         XPackageChunkAvailability& out_availability) {
    out_availability = XPackageChunkAvailability_Ready;
    if (selectorCount > 0u && !selectors) {
        return XGR_E_INVALIDARG;
    }
    if (selectorCount == 0u || !selectors) {
        return XGR_S_OK;
    }

    const std::vector<std::string> installed = ActiveInstalledLanguagesLower();
    bool any_unavailable = false;
    for (uint32_t i = 0u; i < selectorCount; ++i) {
        const XPackageChunkSelector& sel = selectors[i];
        switch (sel.type) {
            case XPackageChunkSelectorType_Language: {
                if (!sel.language || !*sel.language) {
                    return XGR_E_INVALIDARG;
                }
                const auto tokens = SplitDelimitedTokens(sel.language, ';');
                if (tokens.empty()) {
                    return XGR_E_INVALIDARG;
                }
                bool any_lang_matched = false;
                for (const auto& tok : tokens) {
                    if (LanguageTokenMatchesInstalled(tok, installed)) {
                        any_lang_matched = true;
                        break;
                    }
                }
                if (!any_lang_matched) {
                    any_unavailable = true;
                }
                break;
            }
            case XPackageChunkSelectorType_Tag:
                if (!sel.tag || !*sel.tag) {
                    return XGR_E_INVALIDARG;
                }
                break;
            case XPackageChunkSelectorType_Feature:
                if (!sel.feature || !*sel.feature) {
                    return XGR_E_INVALIDARG;
                }
                break;
            case XPackageChunkSelectorType_Chunk:
                break;
            default:
                return XGR_E_INVALIDARG;
        }
    }

    out_availability = any_unavailable ? XPackageChunkAvailability_Unavailable
                                       : XPackageChunkAvailability_Ready;
    return XGR_S_OK;
}

bool IsValidOfflineIdentifier(const char* raw, size_t max_len) {
    if (!raw || !*raw) {
        return false;
    }
    const std::string trimmed = TrimAscii(raw);
    if (trimmed.empty() || trimmed.size() > max_len) {
        return false;
    }
    for (char c : trimmed) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20u || uc >= 0x7Fu) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> ConfiguredOwnedPackages() {
    std::vector<std::string> out;
    const std::string env = GetEnvString("OPENSCARLETT_STORE_OWNED_PACKAGES");
    if (env.empty()) {
        return out;
    }
    std::string norm = env;
    for (char& c : norm) {
        if (c == ',') c = ';';
    }
    return SplitDelimitedTokens(norm, ';');
}

std::string CurrentStoreId() {
    const std::string env = TrimAscii(GetEnvString("OPENSCARLETT_STORE_ID"));
    if (!env.empty()) {
        return env;
    }
    const GameConfigMetadata cfg = LoadGameConfigMetadata();
    if (!cfg.store_id.empty()) {
        return cfg.store_id;
    }
    return kDefaultStoreId;
}

std::string CurrentSkuStoreId() {
    return CurrentStoreId() + "/0001";
}

std::vector<std::string> ConfiguredAddOnStoreIds() {
    std::vector<std::string> out;
    const std::string env = GetEnvString("OPENSCARLETT_STORE_ADDON_IDS");
    if (env.empty()) return out;
    size_t pos = 0;
    while (pos < env.size()) {
        const size_t comma = env.find(',', pos);
        const std::string tok = TrimAscii(
            comma == std::string::npos ? env.substr(pos) : env.substr(pos, comma - pos));
        if (!tok.empty()) {
            out.push_back(tok);
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

std::shared_ptr<XStoreContext> LookupStoreContext(XStoreContextHandle h) {
    if (!h) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    auto it = rt.live_store_contexts.find(h);
    return it == rt.live_store_contexts.end() ? nullptr : it->second;
}

std::shared_ptr<XStoreLicense> LookupStoreLicense(XStoreLicenseHandle h) {
    if (!h) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    auto it = rt.live_store_licenses.find(h);
    return it == rt.live_store_licenses.end() ? nullptr : it->second;
}

std::shared_ptr<XStoreProductQuery> LookupStoreQuery(XStoreProductQueryHandle h) {
    if (!h) return nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    auto it = rt.live_store_queries.find(h);
    return it == rt.live_store_queries.end() ? nullptr : it->second;
}

XStoreProductQueryHandle CreateStoreProductQueryHandle(bool include_current_game) {
    auto q = std::make_shared<XStoreProductQuery>();
    if (include_current_game) {
        const std::string cur_store_id = CurrentStoreId();
        const GameConfigMetadata cfg = LoadGameConfigMetadata();
        q->products.emplace_back();
        XStoreProductBacking& b = q->products.back();
        b.store_id = cur_store_id;
        b.title = cfg.display_name.empty() ? "OpenScarlett Title" : cfg.display_name;
        b.description = "Local OpenScarlett title";
        b.language = "en-US";
        b.offer_token = "";
        b.link_uri = "ms-windows-store://pdp/?productid=" + cur_store_id;
        b.currency_code = "USD";
        b.sku_id = "0001";
        b.availability_id = "AVAIL0000001";

        XStorePrice price{};
        price.basePrice = 0.0f;
        price.price = 0.0f;
        price.recurrencePrice = 0.0f;
        price.currencyCode = b.currency_code.c_str();
        std::snprintf(price.formattedBasePrice, sizeof(price.formattedBasePrice), "%s", "$0.00");
        std::snprintf(price.formattedPrice, sizeof(price.formattedPrice), "%s", "$0.00");
        std::snprintf(price.formattedRecurrencePrice, sizeof(price.formattedRecurrencePrice), "%s", "$0.00");
        price.isOnSale = false;
        price.saleEndDate = 0;

        b.availability.availabilityId = b.availability_id.c_str();
        b.availability.price = price;
        b.availability.endDate = 0;

        b.sku.skuId = b.sku_id.c_str();
        b.sku.title = b.title.c_str();
        b.sku.description = b.description.c_str();
        b.sku.language = b.language.c_str();
        b.sku.price = price;
        b.sku.isTrial = false;
        b.sku.isInUserCollection = true;
        b.sku.collectionData.acquiredDate = 0;
        b.sku.collectionData.startDate = 0;
        b.sku.collectionData.endDate = 0;
        b.sku.collectionData.isTrial = false;
        b.sku.collectionData.trialTimeRemainingInSeconds = 0;
        b.sku.collectionData.quantity = 1;
        b.sku.collectionData.campaignId = "";
        b.sku.collectionData.developerOfferId = "";
        b.sku.isSubscription = false;
        b.sku.availabilitiesCount = 1;
        b.sku.availabilities = &b.availability;

        b.product.storeId = b.store_id.c_str();
        b.product.title = b.title.c_str();
        b.product.description = b.description.c_str();
        b.product.language = b.language.c_str();
        b.product.inAppOfferToken = b.offer_token.c_str();
        b.product.linkUri = b.link_uri.data();
        b.product.productKind = XStoreProductKind_Game;
        b.product.price = price;
        b.product.hasDigitalDownload = true;
        b.product.isInUserCollection = true;
        b.product.skusCount = 1;
        b.product.skus = &b.sku;
    }

    XStoreProductQueryHandle raw = q.get();
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    rt.live_store_queries[raw] = std::move(q);
    return raw;
}

XStoreLicenseHandle CreateStoreLicenseHandle(const std::string& ident) {
    auto lic = std::make_shared<XStoreLicense>();
    lic->identifier = ident.empty() ? CurrentPackageIdentifier() : ident;
    lic->valid = true;
    XStoreLicenseHandle raw = lic.get();
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    rt.live_store_licenses[raw] = std::move(lic);
    return raw;
}

XPackageInstallationMonitorHandle CreateInstallMonitorHandle(const char* pkg_id) {
    auto mon = std::make_shared<XPackageInstallationMonitor>();
    mon->package_id = (pkg_id && *pkg_id) ? pkg_id : CurrentPackageIdentifier();
    mon->total_bytes = 1024ULL * 1024ULL;
    XPackageInstallationMonitorHandle raw = mon.get();
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.package_mutex);
    rt.live_install_monitors[raw] = std::move(mon);
    return raw;
}

// ===========================================================================
// Reusable Async Providers
// ===========================================================================
struct VoidAsyncCtx {
    XGameResult result = XGR_S_OK;
};

XGameResult OPENSCARLETT_GDK_CALL VoidAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<VoidAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, ctx ? ctx->result : XGR_S_OK, 0);
            return XGR_E_PENDING;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult StartVoidAsync(
    XAsyncBlock* async,
    const void* id,
    const char* id_name,
    XGameResult result_hr = XGR_S_OK) {
    if (!async) return XGR_E_INVALIDARG;
    auto* ctx = new (std::nothrow) VoidAsyncCtx{result_hr};
    if (!ctx) return XGR_E_OUTOFMEMORY;
    XGameResult hr = XAsyncBegin(async, ctx, id, id_name, &VoidAsyncProvider);
    if (hr < 0) {
        delete ctx;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return hr;
}

template <typename T>
struct PodAsyncCtx {
    T value{};
};

template <typename T>
XGameResult OPENSCARLETT_GDK_CALL PodAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<PodAsyncCtx<T>*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, sizeof(T));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(T)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &ctx->value, sizeof(T));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

template <typename T>
XGameResult StartPodAsync(
    XAsyncBlock* async,
    const void* id,
    const char* id_name,
    const T& value) {
    if (!async) return XGR_E_INVALIDARG;
    auto* ctx = new (std::nothrow) PodAsyncCtx<T>{value};
    if (!ctx) return XGR_E_OUTOFMEMORY;
    XGameResult hr = XAsyncBegin(async, ctx, id, id_name, &PodAsyncProvider<T>);
    if (hr < 0) {
        delete ctx;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return hr;
}

struct StringAsyncCtx {
    std::string value;
};

XGameResult OPENSCARLETT_GDK_CALL StringAsyncProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<StringAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, ctx->value.size() + 1);
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            const size_t needed = ctx->value.size() + 1;
            if (!data->buffer || data->bufferSize < needed) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, ctx->value.c_str(), needed);
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult StartStringAsync(
    XAsyncBlock* async,
    const void* id,
    const char* id_name,
    std::string value) {
    if (!async) return XGR_E_INVALIDARG;
    auto* ctx = new (std::nothrow) StringAsyncCtx{std::move(value)};
    if (!ctx) return XGR_E_OUTOFMEMORY;
    XGameResult hr = XAsyncBegin(async, ctx, id, id_name, &StringAsyncProvider);
    if (hr < 0) {
        delete ctx;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return hr;
}

// Identity tags for new async operations
const char kIdUserTokenSig[] = "XUserGetTokenAndSignatureAsync";
const char kIdUserTokenSigUtf16[] = "XUserGetTokenAndSignatureUtf16Async";
const char kIdUserResolveIssue[] = "XUserResolveIssueWithUiAsync";
const char kIdUserResolveIssueUtf16[] = "XUserResolveIssueWithUiUtf16Async";
const char kIdUserResolvePriv[] = "XUserResolvePrivilegeWithUiAsync";

const char kIdStoreGameLicense[] = "XStoreQueryGameLicenseAsync";
const char kIdStoreAddOnLicenses[] = "XStoreQueryAddOnLicensesAsync";
const char kIdStoreProdCurrentGame[] = "XStoreQueryProductForCurrentGameAsync";
const char kIdStoreProdPackage[] = "XStoreQueryProductForPackageAsync";
const char kIdStoreAssocProds[] = "XStoreQueryAssociatedProductsAsync";
const char kIdStoreEntitledProds[] = "XStoreQueryEntitledProductsAsync";
const char kIdStoreQueryProds[] = "XStoreQueryProductsAsync";
const char kIdStoreNextPage[] = "XStoreProductsQueryNextPageAsync";
const char kIdStoreCanAcquirePkg[] = "XStoreCanAcquireLicenseForPackageAsync";
const char kIdStoreCanAcquireStoreId[] = "XStoreCanAcquireLicenseForStoreIdAsync";
const char kIdStoreAcquirePkg[] = "XStoreAcquireLicenseForPackageAsync";
const char kIdStoreAcquireDurable[] = "XStoreAcquireLicenseForDurablesAsync";
const char kIdStoreConsumableBal[] = "XStoreQueryConsumableBalanceRemainingAsync";
const char kIdStoreConsumableFulfill[] = "XStoreReportConsumableFulfillmentAsync";
const char kIdStorePkgUpdates[] = "XStoreQueryGameAndDlcPackageUpdatesAsync";
const char kIdStoreDownloadUpdates[] = "XStoreDownloadPackageUpdatesAsync";
const char kIdStoreDownloadInstallUpdates[] = "XStoreDownloadAndInstallPackageUpdatesAsync";
const char kIdStoreDownloadInstallPkgs[] = "XStoreDownloadAndInstallPackagesAsync";
const char kIdStoreCollectionsId[] = "XStoreGetUserCollectionsIdAsync";
const char kIdStorePurchaseId[] = "XStoreGetUserPurchaseIdAsync";
const char kIdStoreLicenseToken[] = "XStoreQueryLicenseTokenAsync";
const char kIdStoreShowAssocUi[] = "XStoreShowAssociatedProductsUIAsync";
const char kIdStoreShowProdUi[] = "XStoreShowProductPageUIAsync";
const char kIdStoreShowPurchaseUi[] = "XStoreShowPurchaseUIAsync";
const char kIdStoreShowRateUi[] = "XStoreShowRateAndReviewUIAsync";
const char kIdStoreShowRedeemUi[] = "XStoreShowRedeemTokenUIAsync";

const char kIdUiMessageDialog[] = "XGameUiShowMessageDialogAsync";
const char kIdUiErrorDialog[] = "XGameUiShowErrorDialogAsync";
const char kIdUiTextEntry[] = "XGameUiShowTextEntryAsync";
const char kIdUiAchievements[] = "XGameUiShowAchievementsAsync";
const char kIdUiPlayerPicker[] = "XGameUiShowPlayerPickerAsync";
const char kIdUiProfileCard[] = "XGameUiShowPlayerProfileCardAsync";
const char kIdUiSendInvite[] = "XGameUiShowSendGameInviteAsync";
const char kIdUiWebAuth[] = "XGameUiShowWebAuthenticationAsync";

const char kIdNetUdpPort[] = "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync";
const char kIdNetSecInfo[] = "XNetworkingQuerySecurityInformationForUrlAsync";
const char kIdNetSecInfoUtf16[] = "XNetworkingQuerySecurityInformationForUrlUtf16Async";

const char kIdPackageInstallChunks[] = "XPackageInstallChunksAsync";

// Specialized async providers
struct TokenSigAsyncCtx {
    XGameResult result_hr = XGR_S_OK;
    std::string token;
    std::string signature;
};

XGameResult OPENSCARLETT_GDK_CALL TokenSigProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<TokenSigAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (ctx->result_hr < 0) {
                XAsyncComplete(data->async, ctx->result_hr, 0);
                return XGR_E_PENDING;
            }
            const size_t req = sizeof(XUserGetTokenAndSignatureData) +
                               (ctx->token.size() + 1) +
                               (ctx->signature.size() + 1);
            XAsyncComplete(data->async, XGR_S_OK, req);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (ctx->result_hr < 0) {
                return ctx->result_hr;
            }
            const size_t tok_sz = ctx->token.size() + 1;
            const size_t sig_sz = ctx->signature.size() + 1;
            const size_t req = sizeof(XUserGetTokenAndSignatureData) + tok_sz + sig_sz;
            if (!data->buffer || data->bufferSize < req) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            auto* out = static_cast<XUserGetTokenAndSignatureData*>(data->buffer);
            auto* base = static_cast<uint8_t*>(data->buffer);
            char* tok_dst = reinterpret_cast<char*>(base + sizeof(XUserGetTokenAndSignatureData));
            char* sig_dst = tok_dst + tok_sz;
            std::memcpy(tok_dst, ctx->token.c_str(), tok_sz);
            std::memcpy(sig_dst, ctx->signature.c_str(), sig_sz);
            out->tokenSize = tok_sz;
            out->signatureSize = sig_sz;
            out->token = tok_dst;
            out->signature = sig_dst;
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct TokenSigUtf16AsyncCtx {
    XGameResult result_hr = XGR_S_OK;
    std::wstring token;
    std::wstring signature;
};

XGameResult OPENSCARLETT_GDK_CALL TokenSigUtf16Provider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<TokenSigUtf16AsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            if (ctx->result_hr < 0) {
                XAsyncComplete(data->async, ctx->result_hr, 0);
                return XGR_E_PENDING;
            }
            const size_t req = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                               (ctx->token.size() + 1) * sizeof(wchar_t) +
                               (ctx->signature.size() + 1) * sizeof(wchar_t);
            XAsyncComplete(data->async, XGR_S_OK, req);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            if (ctx->result_hr < 0) {
                return ctx->result_hr;
            }
            const size_t tok_cnt = ctx->token.size() + 1;
            const size_t sig_cnt = ctx->signature.size() + 1;
            const size_t req = sizeof(XUserGetTokenAndSignatureUtf16Data) +
                               tok_cnt * sizeof(wchar_t) +
                               sig_cnt * sizeof(wchar_t);
            if (!data->buffer || data->bufferSize < req) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            auto* out = static_cast<XUserGetTokenAndSignatureUtf16Data*>(data->buffer);
            auto* base = static_cast<uint8_t*>(data->buffer);
            wchar_t* tok_dst = reinterpret_cast<wchar_t*>(
                base + sizeof(XUserGetTokenAndSignatureUtf16Data));
            wchar_t* sig_dst = tok_dst + tok_cnt;
            std::memcpy(tok_dst, ctx->token.c_str(), tok_cnt * sizeof(wchar_t));
            std::memcpy(sig_dst, ctx->signature.c_str(), sig_cnt * sizeof(wchar_t));
            out->tokenCount = tok_cnt;
            out->signatureCount = sig_cnt;
            out->token = tok_dst;
            out->signature = sig_dst;
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct StoreProductQueryAsyncCtx {
    bool include_current_game = false;
    XStoreProductQueryHandle query_handle = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL StoreProductQueryProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<StoreProductQueryAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->query_handle = CreateStoreProductQueryHandle(ctx->include_current_game);
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XStoreProductQueryHandle));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XStoreProductQueryHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XStoreProductQueryHandle h = ctx->query_handle;
            ctx->query_handle = nullptr;
            std::memcpy(data->buffer, &h, sizeof(h));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->query_handle) {
                XStoreCloseProductsQueryHandle(ctx->query_handle);
                ctx->query_handle = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

XGameResult StartStoreProductQueryAsync(
    XAsyncBlock* async,
    const void* id,
    const char* id_name,
    bool include_current_game) {
    if (!async) return XGR_E_INVALIDARG;
    auto* ctx = new (std::nothrow) StoreProductQueryAsyncCtx{include_current_game, nullptr};
    if (!ctx) return XGR_E_OUTOFMEMORY;
    XGameResult hr = XAsyncBegin(async, ctx, id, id_name, &StoreProductQueryProvider);
    if (hr < 0) {
        delete ctx;
        return hr;
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return hr;
}

struct StoreAcquireLicenseAsyncCtx {
    std::string identifier;
    XStoreLicenseHandle license_handle = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL StoreAcquireLicenseProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<StoreAcquireLicenseAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->license_handle = CreateStoreLicenseHandle(ctx->identifier);
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XStoreLicenseHandle));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XStoreLicenseHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XStoreLicenseHandle h = ctx->license_handle;
            ctx->license_handle = nullptr;
            std::memcpy(data->buffer, &h, sizeof(h));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->license_handle) {
                XStoreCloseLicenseHandle(ctx->license_handle);
                ctx->license_handle = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct UiPlayerPickerAsyncCtx {
    std::vector<uint64_t> players;
};

XGameResult OPENSCARLETT_GDK_CALL UiPlayerPickerProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<UiPlayerPickerAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, ctx->players.size() * sizeof(uint64_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            const size_t req = ctx->players.size() * sizeof(uint64_t);
            if (req == 0) return XGR_S_OK;
            if (!data->buffer || data->bufferSize < req) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, ctx->players.data(), req);
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct UiWebAuthAsyncCtx {
    std::string completion_uri;
};

XGameResult OPENSCARLETT_GDK_CALL UiWebAuthProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<UiWebAuthAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork: {
            const size_t req =
                sizeof(XGameUiWebAuthenticationResultData) + (ctx->completion_uri.size() + 1);
            XAsyncComplete(data->async, XGR_S_OK, req);
            return XGR_E_PENDING;
        }
        case XAsyncOp_GetResult: {
            const size_t uri_sz = ctx->completion_uri.size() + 1;
            const size_t req = sizeof(XGameUiWebAuthenticationResultData) + uri_sz;
            if (!data->buffer || data->bufferSize < req) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            auto* out = static_cast<XGameUiWebAuthenticationResultData*>(data->buffer);
            auto* base = static_cast<uint8_t*>(data->buffer);
            char* uri_dst =
                reinterpret_cast<char*>(base + sizeof(XGameUiWebAuthenticationResultData));
            std::memcpy(uri_dst, ctx->completion_uri.c_str(), uri_sz);
            out->responseStatus = XGR_E_ABORT;
            out->responseCompletionUriSize = uri_sz;
            out->responseCompletionUri = uri_dst;
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct PackageInstallChunksAsyncCtx {
    std::string package_id;
    XPackageInstallationMonitorHandle monitor = nullptr;
};

XGameResult OPENSCARLETT_GDK_CALL PackageInstallChunksProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<PackageInstallChunksAsyncCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            ctx->monitor = CreateInstallMonitorHandle(ctx->package_id.c_str());
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XPackageInstallationMonitorHandle));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            if (!data->buffer || data->bufferSize < sizeof(XPackageInstallationMonitorHandle)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            XPackageInstallationMonitorHandle h = ctx->monitor;
            ctx->monitor = nullptr;
            std::memcpy(data->buffer, &h, sizeof(h));
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            if (ctx->monitor) {
                XPackageCloseInstallationMonitorHandle(ctx->monitor);
                ctx->monitor = nullptr;
            }
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

}  // namespace

extern "C" {

// ===========================================================================
// Game Runtime Lifecycle
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameRuntimeInitialize(void) {
    auto& rt = GetRuntime();
    rt.init_count.fetch_add(1, std::memory_order_acq_rel);
    SyncConsoleLsaPolicyFromConfig();
    return RecordHr("XGameRuntimeInitialize", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameRuntimeUninitialize(void) {
    auto& rt = GetRuntime();
    const int32_t prev = rt.init_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        rt.init_count.store(0, std::memory_order_release);
        XTaskQueueHandle proc_q = nullptr;
        {
            std::lock_guard<std::mutex> lk(rt.mutex);
            proc_q = rt.process_queue;
            rt.process_queue = nullptr;
        }
        if (proc_q) {
            XTaskQueueTerminate(proc_q, true, nullptr, nullptr);
            XTaskQueueCloseHandle(proc_q);
        }
        // Clean up any unclaimed async records.
        std::vector<std::shared_ptr<AsyncRecord>> records;
        {
            std::lock_guard<std::mutex> lk(rt.async_mutex);
            for (auto& [_, r] : rt.async_records) {
                records.push_back(r);
            }
            rt.async_records.clear();
        }
        for (auto& r : records) {
            CleanupAsyncRecordOnce(r);
        }
    }
    RecordOk("XGameRuntimeUninitialize");
}

bool OPENSCARLETT_GDK_CALL XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature feature) {
    RecordOk("XGameRuntimeIsFeatureAvailable");
    return static_cast<uint32_t>(feature) <= static_cast<uint32_t>(XGameRuntimeFeature_XGameStreaming);
}

XGameResult OPENSCARLETT_GDK_CALL XGameGetXboxTitleId(uint32_t* titleId) {
    if (!titleId) {
        return RecordHr("XGameGetXboxTitleId", XGR_E_INVALIDARG);
    }
    uint32_t tid = 0x5CA00001u;
    bool from_env = false;
    std::string env = GetEnvString("OPENSCARLETT_TITLE_ID");
    // Trim leading/trailing ASCII whitespace.
    const size_t first = env.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        const size_t last = env.find_last_not_of(" \t\r\n");
        const std::string trimmed = env.substr(first, last - first + 1);
        if (!trimmed.empty() && trimmed[0] != '-') {
            bool parsed_ok = false;
            unsigned long long parsed = 0;
            if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
                char* end_ptr = nullptr;
                errno = 0;
                parsed = std::strtoull(trimmed.c_str(), &end_ptr, 16);
                parsed_ok = (errno == 0 && end_ptr && *end_ptr == '\0' && parsed <= 0xFFFFFFFFULL);
            } else {
                char* end_ptr = nullptr;
                errno = 0;
                parsed = std::strtoull(trimmed.c_str(), &end_ptr, 10);
                if (errno == 0 && end_ptr && *end_ptr == '\0' && parsed <= 0xFFFFFFFFULL) {
                    parsed_ok = true;
                } else if (trimmed.size() <= 8) {
                    // Unprefixed Xbox MicrosoftGame.Config hex TitleId (e.g. "5CA02002", "7e750470")
                    end_ptr = nullptr;
                    errno = 0;
                    parsed = std::strtoull(trimmed.c_str(), &end_ptr, 16);
                    parsed_ok = (errno == 0 && end_ptr && *end_ptr == '\0' && parsed <= 0xFFFFFFFFULL);
                }
            }
            if (parsed_ok) {
                tid = static_cast<uint32_t>(parsed);
                from_env = true;
            }
        }
    }
    if (!from_env) {
        const GameConfigMetadata cfg = LoadGameConfigMetadata();
        if (cfg.has_title_id) {
            tid = cfg.title_id;
        }
    }
    *titleId = tid;
    return RecordHr("XGameGetXboxTitleId", XGR_S_OK);
}

// ===========================================================================
// XTaskQueue
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) {
    if (!queue) {
        return RecordHr("XTaskQueueCreate", XGR_E_INVALIDARG);
    }
    *queue = nullptr;

    auto obj = std::make_shared<XTaskQueueObject>();
    obj->ports[0] = std::make_shared<PortState>();
    obj->ports[0]->mode = workDispatchMode;
    obj->ports[0]->port_id = XTaskQueuePort_Work;
    obj->ports[1] = std::make_shared<PortState>();
    obj->ports[1]->mode = completionDispatchMode;
    obj->ports[1]->port_id = XTaskQueuePort_Completion;
    obj->owns_port[0] = true;
    obj->owns_port[1] = true;
    obj->port_handles[0].state = obj->ports[0];
    obj->port_handles[1].state = obj->ports[1];

    XTaskQueueHandle raw = obj.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        rt.live_queues[raw] = obj;
    }

    StartPortWorkersIfNeeded(raw, 0);
    StartPortWorkersIfNeeded(raw, 1);

    *queue = raw;
    return RecordHr("XTaskQueueCreate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle) {
    if (!queueHandle || !duplicatedHandle) {
        return RecordHr("XTaskQueueDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto obj = LookupQueue(queueHandle);
    if (!obj) {
        return RecordHr("XTaskQueueDuplicateHandle", XGR_E_HANDLE);
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = queueHandle;
    return RecordHr("XTaskQueueDuplicateHandle", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    RecordOk("XTaskQueueCloseHandle");
    if (!queue) return;

    std::shared_ptr<XTaskQueueObject> dropped;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        auto it = rt.live_queues.find(queue);
        if (it == rt.live_queues.end()) {
            return;
        }
        if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dropped = std::move(it->second);
            rt.live_queues.erase(it);
        }
    }
    if (dropped) {
        ShutdownQueueInternal(dropped.get(), true);
    }
}

bool OPENSCARLETT_GDK_CALL XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (!queue) {
        RecordHr("XTaskQueueGetCurrentProcessTaskQueue", XGR_E_INVALIDARG);
        return false;
    }
    *queue = nullptr;

    XTaskQueueHandle existing = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        existing = rt.process_queue;
    }
    if (!existing) {
        XTaskQueueHandle created = nullptr;
        if (XTaskQueueCreate(
                XTaskQueueDispatchMode_ThreadPool,
                XTaskQueueDispatchMode_ThreadPool,
                &created) < 0) {
            RecordHr("XTaskQueueGetCurrentProcessTaskQueue", XGR_E_FAIL);
            return false;
        }
        XTaskQueueHandle discard = nullptr;
        {
            auto& rt = GetRuntime();
            std::lock_guard<std::mutex> lk(rt.mutex);
            if (!rt.process_queue) {
                rt.process_queue = created;
                existing = created;
            } else {
                existing = rt.process_queue;
                discard = created;
            }
        }
        if (discard) {
            XTaskQueueCloseHandle(discard);
        }
    }

    const XGameResult hr = XTaskQueueDuplicateHandle(existing, queue);
    RecordHr("XTaskQueueGetCurrentProcessTaskQueue", hr);
    return hr >= 0;
}

void OPENSCARLETT_GDK_CALL XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    XTaskQueueHandle dup = nullptr;
    if (queue) {
        if (XTaskQueueDuplicateHandle(queue, &dup) < 0) {
            RecordHr("XTaskQueueSetCurrentProcessTaskQueue", XGR_E_HANDLE);
            return;
        }
    }
    XTaskQueueHandle old_q = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        old_q = rt.process_queue;
        rt.process_queue = dup;
    }
    if (old_q) {
        XTaskQueueCloseHandle(old_q);
    }
    RecordOk("XTaskQueueSetCurrentProcessTaskQueue");
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback callback) {
    if (!queue || !callback || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_HANDLE);
    }

    {
        std::lock_guard<std::mutex> tlk(obj->terminate_mutex);
        if (obj->terminated) {
            return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_ABORT);
        }
    }

    PortState& p = *obj->ports[static_cast<uint32_t>(port)];

    if (p.mode == XTaskQueueDispatchMode_Immediate) {
        {
            std::lock_guard<std::mutex> lk(p.mutex);
            if (p.terminating) {
                return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_ABORT);
            }
        }
        NotifyMonitors(obj.get(), port);
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        callback(callbackContext, false);
        return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_S_OK);
    }

    {
        std::lock_guard<std::mutex> lk(p.mutex);
        if (p.terminating) {
            return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_E_ABORT);
        }
        ScheduledCallback item{};
        item.ready_time = SteadyClock::now() + std::chrono::milliseconds(delayMs);
        item.seq = p.next_seq++;
        item.callback = callback;
        item.context = callbackContext;
        p.items.push_back(item);
        p.cv.notify_all();
    }

    NotifyMonitors(obj.get(), port);
    return RecordHr("XTaskQueueSubmitDelayedCallback", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback callback) {
    const XGameResult hr = XTaskQueueSubmitDelayedCallback(queue, port, 0, callbackContext, callback);
    return RecordHr("XTaskQueueSubmitCallback", hr);
}

bool OPENSCARLETT_GDK_CALL XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) {
    if (!queue || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        RecordHr("XTaskQueueDispatch", XGR_E_INVALIDARG);
        return false;
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        RecordHr("XTaskQueueDispatch", XGR_E_HANDLE);
        return false;
    }

    PortState& p = *obj->ports[static_cast<uint32_t>(port)];
    const bool infinite_wait = (timeoutInMs == 0xFFFFFFFFu);
    const auto deadline = infinite_wait
                              ? SteadyClock::time_point::max()
                              : (SteadyClock::now() + std::chrono::milliseconds(timeoutInMs));

    ScheduledCallback item{};
    bool has_item = false;
    bool canceled = false;

    {
        std::unique_lock<std::mutex> lk(p.mutex);
        for (;;) {
            const bool is_terminated =
                p.terminating || obj->terminated.load(std::memory_order_acquire);
            if (!p.items.empty()) {
                const size_t idx = FindEarliestItemIndexLocked(p);
                const auto now = SteadyClock::now();
                if (is_terminated || p.items[idx].ready_time <= now) {
                    item = p.items[idx];
                    p.items.erase(p.items.begin() + static_cast<std::ptrdiff_t>(idx));
                    canceled = is_terminated;
                    has_item = true;
                    ++p.active_callbacks;
                    break;
                }
                if (timeoutInMs == 0 || (!infinite_wait && now >= deadline)) {
                    break;
                }
                const auto wait_until_tp =
                    infinite_wait ? p.items[idx].ready_time
                                  : std::min(deadline, p.items[idx].ready_time);
                p.cv.wait_until(lk, wait_until_tp);
                continue;
            }
            // Once terminated and completely drained (zero remaining queued items),
            // return false immediately even when timeoutInMs is INFINITE!
            if (is_terminated || timeoutInMs == 0 ||
                (!infinite_wait && SteadyClock::now() >= deadline)) {
                break;
            }
            if (infinite_wait) {
                p.cv.wait(lk);
            } else {
                p.cv.wait_until(lk, deadline);
            }
        }
    }

    if (has_item && item.callback) {
        item.callback(item.context, canceled);
    }
    if (has_item) {
        {
            std::lock_guard<std::mutex> lk(p.mutex);
            if (p.active_callbacks > 0) {
                --p.active_callbacks;
            }
            p.cv.notify_all();
        }
        if (obj->terminated.load(std::memory_order_acquire)) {
            TryFireDeferredTerminatedCallback(obj.get());
        }
    }

    RecordOk("XTaskQueueDispatch");
    return has_item;
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback callback) {
    if (!queue) {
        return RecordHr("XTaskQueueTerminate", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueTerminate", XGR_E_HANDLE);
    }

    {
        std::lock_guard<std::mutex> lk(obj->terminate_mutex);
        obj->terminated.store(true, std::memory_order_release);
        if (callback && !obj->terminated_cb_fired) {
            obj->terminated_cb = callback;
            obj->terminated_ctx = callbackContext;
        }
    }

    ShutdownQueueInternal(obj.get(), wait);

    if (wait) {
        XTaskQueueTerminatedCallback pending_cb = nullptr;
        void* pending_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lk(obj->terminate_mutex);
            if (!obj->terminated_cb_fired && obj->terminated_cb != nullptr) {
                obj->terminated_cb_fired = true;
                pending_cb = obj->terminated_cb;
                pending_ctx = obj->terminated_ctx;
                obj->terminated_cb = nullptr;
                obj->terminated_ctx = nullptr;
            }
        }
        if (pending_cb) {
            pending_cb(pending_ctx);
        }
    }
    return RecordHr("XTaskQueueTerminate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!queue || !callback || !token) {
        return RecordHr("XTaskQueueRegisterMonitor", XGR_E_INVALIDARG);
    }
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueRegisterMonitor", XGR_E_HANDLE);
    }
    std::lock_guard<std::mutex> lk(obj->monitor_mutex);
    const uint64_t tok = obj->next_monitor_token++;
    obj->monitors.push_back(MonitorEntry{tok, callback, callbackContext});
    token->token = tok;
    return RecordHr("XTaskQueueRegisterMonitor", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) {
    RecordOk("XTaskQueueUnregisterMonitor");
    if (!queue || token.token == 0) return;
    auto obj = LookupQueue(queue);
    if (!obj) return;
    std::lock_guard<std::mutex> lk(obj->monitor_mutex);
    auto& m = obj->monitors;
    m.erase(
        std::remove_if(m.begin(), m.end(),
                       [&](const MonitorEntry& e) { return e.token == token.token; }),
        m.end());
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle) {
    if (!queue || !portHandle || (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        return RecordHr("XTaskQueueGetPort", XGR_E_INVALIDARG);
    }
    *portHandle = nullptr;
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueGetPort", XGR_E_HANDLE);
    }
    *portHandle = &obj->port_handles[static_cast<uint32_t>(port)];
    return RecordHr("XTaskQueueGetPort", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue) {
    if (!workPort || !completionPort || !workPort->state || !completionPort->state || !queue) {
        return RecordHr("XTaskQueueCreateComposite", XGR_E_INVALIDARG);
    }
    *queue = nullptr;

    auto obj = std::make_shared<XTaskQueueObject>();
    obj->ports[0] = workPort->state;
    obj->ports[1] = completionPort->state;
    obj->owns_port[0] = false;
    obj->owns_port[1] = false;
    obj->port_handles[0].state = obj->ports[0];
    obj->port_handles[1].state = obj->ports[1];

    XTaskQueueHandle raw = obj.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mutex);
        rt.live_queues[raw] = obj;
    }

    *queue = raw;
    return RecordHr("XTaskQueueCreateComposite", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XTaskQueueRegisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* waitHandle,
    void* callbackContext,
    XTaskQueueCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!queue || !waitHandle || !callback || !token ||
        (port != XTaskQueuePort_Work && port != XTaskQueuePort_Completion)) {
        return RecordHr("XTaskQueueRegisterWaiter", XGR_E_INVALIDARG);
    }
    token->token = 0;
    auto obj = LookupQueue(queue);
    if (!obj) {
        return RecordHr("XTaskQueueRegisterWaiter", XGR_E_HANDLE);
    }
    if (obj->terminated.load(std::memory_order_acquire)) {
        return RecordHr("XTaskQueueRegisterWaiter", XGR_E_ABORT);
    }

    auto entry = std::make_shared<WaiterEntry>();
    entry->queue = queue;
    entry->queue_weak = obj;
    entry->port = port;
    entry->callback = callback;
    entry->context = callbackContext;

#if defined(_WIN32)
    PTP_WAIT tp_wait = CreateThreadpoolWait(&WaiterTpCallback, entry.get(), nullptr);
    if (!tp_wait) {
        return RecordHr("XTaskQueueRegisterWaiter", XGR_E_FAIL);
    }
    entry->wait_handle = static_cast<HANDLE>(waitHandle);
    entry->tp_wait = tp_wait;
#endif

    {
        std::lock_guard<std::mutex> lk(obj->waiter_mutex);
        if (obj->terminated.load(std::memory_order_acquire)) {
#if defined(_WIN32)
            CloseThreadpoolWait(tp_wait);
            entry->tp_wait = nullptr;
#endif
            return RecordHr("XTaskQueueRegisterWaiter", XGR_E_ABORT);
        }
        entry->token = obj->next_waiter_token++;
        token->token = entry->token;
        obj->waiters.push_back(entry);
    }

#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> wlk(entry->wait_mutex);
        if (entry->active.load(std::memory_order_acquire) &&
            entry->tp_wait != nullptr &&
            !obj->terminated.load(std::memory_order_acquire)) {
            SetThreadpoolWait(entry->tp_wait, entry->wait_handle, nullptr);
        }
    }
#endif
    return RecordHr("XTaskQueueRegisterWaiter", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XTaskQueueUnregisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) {
    RecordOk("XTaskQueueUnregisterWaiter");
    if (!queue || token.token == 0) return;
    auto obj = LookupQueue(queue);
    if (!obj) return;

    std::shared_ptr<WaiterEntry> removed;
    {
        std::lock_guard<std::mutex> lk(obj->waiter_mutex);
        auto& w = obj->waiters;
        for (auto it = w.begin(); it != w.end(); ++it) {
            if ((*it)->token == token.token) {
                removed = std::move(*it);
                w.erase(it);
                break;
            }
        }
    }
    if (removed) {
        DisarmAndCloseWaiter(std::move(removed), obj.get());
    }
}

// ===========================================================================
// XAsync
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider provider) {
    if (!asyncBlock || !provider) {
        return RecordHr("XAsyncBegin", XGR_E_INVALIDARG);
    }

    // Acquire queue handle for this operation.
    XTaskQueueHandle q = nullptr;
    if (asyncBlock->queue) {
        if (XTaskQueueDuplicateHandle(asyncBlock->queue, &q) < 0) {
            return RecordHr("XAsyncBegin", XGR_E_HANDLE);
        }
    } else {
        if (!XTaskQueueGetCurrentProcessTaskQueue(&q)) {
            return RecordHr("XAsyncBegin", XGR_E_FAIL);
        }
    }

    auto rec = std::make_shared<AsyncRecord>();
    rec->async_block = asyncBlock;
    rec->queue = q;
    rec->provider_context = context;
    rec->identity = identity;
    rec->identity_name = identityName ? identityName : "";
    rec->provider = provider;

    // Register record & write slot into asyncBlock->internal.
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.async_mutex);
        rec->id = rt.next_async_id++;
        rt.async_records[rec->id] = rec;
    }

    AsyncSlot slot{kAsyncSlotMagic, rec->id};
    std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
    std::memcpy(asyncBlock->internal, &slot, sizeof(slot));

    XAsyncProviderData data{};
    data.async = asyncBlock;
    data.context = context;
    const XGameResult hr = provider(XAsyncOp_Begin, &data);
    if (hr < 0) {
        {
            auto& rt = GetRuntime();
            std::lock_guard<std::mutex> lk(rt.async_mutex);
            rt.async_records.erase(rec->id);
        }
        if (rec->queue) {
            XTaskQueueCloseHandle(rec->queue);
            rec->queue = nullptr;
        }
        std::memset(asyncBlock->internal, 0, sizeof(asyncBlock->internal));
        return RecordHr("XAsyncBegin", hr);
    }

    return RecordHr("XAsyncBegin", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncSchedule(
    XAsyncBlock* asyncBlock,
    uint32_t delayInMs) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec || !rec->queue) {
        return RecordHr("XAsyncSchedule", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) AsyncTrampolineCtx{rec};
    if (!ctx) {
        return RecordHr("XAsyncSchedule", XGR_E_OUTOFMEMORY);
    }
    const XGameResult hr = XTaskQueueSubmitDelayedCallback(
        rec->queue, XTaskQueuePort_Work, delayInMs, ctx, &AsyncWorkTrampoline);
    if (hr < 0) {
        delete ctx;
    }
    return RecordHr("XAsyncSchedule", hr);
}

void OPENSCARLETT_GDK_CALL XAsyncComplete(
    XAsyncBlock* asyncBlock,
    XGameResult result,
    size_t requiredBufferSize) {
    RecordOk("XAsyncComplete");
    auto rec = LookupAsync(asyncBlock);
    if (!rec) return;

    if (rec->complete_called.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    XTaskQueueHandle q = nullptr;
    bool has_callback = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->result = result;
        rec->required_buffer_size = requiredBufferSize;
        has_callback = (rec->async_block && rec->async_block->callback != nullptr);
        q = rec->queue;
    }

    if (has_callback && q) {
        auto* ctx = new (std::nothrow) AsyncTrampolineCtx{rec};
        if (ctx) {
            if (XTaskQueueSubmitCallback(
                    q, XTaskQueuePort_Completion, ctx, &AsyncCompletionTrampoline) >= 0) {
                return;
            }
            delete ctx;
        }
    }

    bool should_auto_cleanup = false;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        rec->result_ready = true;
        rec->completed = true;
        should_auto_cleanup = (rec->required_buffer_size == 0 || rec->result < 0);
    }
    rec->cv.notify_all();

    if (should_auto_cleanup) {
        CleanupAsyncRecordOnce(rec);
    }
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetStatus(
    XAsyncBlock* asyncBlock,
    bool wait) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetStatus", XGR_E_INVALIDARG);
    }

    std::unique_lock<std::mutex> lk(rec->mutex);
    if (rec->result_ready && rec->callback_thread_id == std::this_thread::get_id()) {
        return RecordHr("XAsyncGetStatus", rec->result);
    }
    if (wait) {
        rec->cv.wait(lk, [&]() { return rec->completed; });
    }
    const XGameResult hr = rec->completed ? rec->result : XGR_E_PENDING;
    return RecordHr("XAsyncGetStatus", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResultSize(
    XAsyncBlock* asyncBlock,
    size_t* bufferSize) {
    if (!asyncBlock || !bufferSize) {
        return RecordHr("XAsyncGetResultSize", XGR_E_INVALIDARG);
    }
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetResultSize", XGR_E_INVALIDARG);
    }
    std::lock_guard<std::mutex> lk(rec->mutex);
    if (!rec->result_ready && !rec->completed) {
        return RecordHr("XAsyncGetResultSize", XGR_E_PENDING);
    }
    if (rec->result < 0) {
        return RecordHr("XAsyncGetResultSize", rec->result);
    }
    *bufferSize = rec->required_buffer_size;
    return RecordHr("XAsyncGetResultSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) {
    auto rec = LookupAsync(asyncBlock);
    if (!rec) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }

    XGameResult status = XGR_S_OK;
    size_t req_size = 0;
    {
        std::lock_guard<std::mutex> lk(rec->mutex);
        if (!rec->result_ready && !rec->completed) {
            return RecordHr("XAsyncGetResult", XGR_E_PENDING);
        }
        status = rec->result;
        req_size = rec->required_buffer_size;
    }

    if (identity != nullptr && rec->identity != identity) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }
    if (status < 0) {
        CleanupAsyncRecordOnce(rec);
        return RecordHr("XAsyncGetResult", status);
    }

    if (req_size == 0) {
        if (bufferUsed) *bufferUsed = 0;
        CleanupAsyncRecordOnce(rec);
        return RecordHr("XAsyncGetResult", XGR_S_OK);
    }

    if (!buffer) {
        return RecordHr("XAsyncGetResult", XGR_E_INVALIDARG);
    }
    if (bufferSize < req_size) {
        return RecordHr("XAsyncGetResult", XGR_E_NOT_SUFFICIENT_BUFFER);
    }

    XAsyncProviderData data{};
    data.async = asyncBlock;
    data.buffer = buffer;
    data.bufferSize = bufferSize;
    data.context = rec->provider_context;
    const XGameResult hr = rec->provider ? rec->provider(XAsyncOp_GetResult, &data) : XGR_E_FAIL;
    if (hr >= 0 && bufferUsed) {
        *bufferUsed = req_size;
    }
    if (hr >= 0) {
        CleanupAsyncRecordOnce(rec);
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.async_mutex);
        rt.async_records.erase(rec->id);
    }
    return RecordHr("XAsyncGetResult", hr);
}

void OPENSCARLETT_GDK_CALL XAsyncCancel(XAsyncBlock* asyncBlock) {
    RecordOk("XAsyncCancel");
    auto rec = LookupAsync(asyncBlock);
    if (!rec) return;
    if (rec->canceled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (rec->complete_called.load(std::memory_order_acquire) ||
        rec->cleaned_up.load(std::memory_order_acquire)) {
        return;
    }
    if (rec->provider) {
        XAsyncProviderData data{};
        data.async = asyncBlock;
        data.context = rec->provider_context;
        rec->provider(XAsyncOp_Cancel, &data);
    }
}

XGameResult OPENSCARLETT_GDK_CALL XAsyncRun(
    XAsyncBlock* asyncBlock,
    XAsyncWork work) {
    if (!asyncBlock || !work) {
        return RecordHr("XAsyncRun", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) AsyncRunCtx{work};
    if (!ctx) {
        return RecordHr("XAsyncRun", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(asyncBlock, ctx, kIdAsyncRun, kIdAsyncRun, &AsyncRunProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XAsyncRun", hr);
    }
    hr = XAsyncSchedule(asyncBlock, 0);
    if (hr < 0) {
        XAsyncComplete(asyncBlock, hr, 0);
    }
    return RecordHr("XAsyncRun", hr);
}

// ===========================================================================
// XUser
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XUserAddAsync(
    XUserAddOptions /*options*/,
    XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XUserAddAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) UserAddAsyncCtx();
    if (!ctx) {
        return RecordHr("XUserAddAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(async, ctx, kIdUserAdd, kIdUserAdd, &UserAddProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XUserAddAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XUserAddAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser) {
    if (!async || !newUser) {
        return RecordHr("XUserAddResult", XGR_E_INVALIDARG);
    }
    *newUser = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserAddResult", hr);
    }
    hr = XAsyncGetResult(async, kIdUserAdd, sizeof(XUserHandle), newUser, nullptr);
    return RecordHr("XUserAddResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return RecordHr("XUserDuplicateHandle", XGR_E_INVALIDARG);
    }
    *duplicatedHandle = nullptr;
    auto u = LookupUser(handle);
    if (!u) {
        return RecordHr("XUserDuplicateHandle", XGR_E_HANDLE);
    }
    u->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return RecordHr("XUserDuplicateHandle", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XUserCloseHandle(XUserHandle user) {
    RecordOk("XUserCloseHandle");
    if (!user) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    auto it = rt.live_users.find(user);
    if (it == rt.live_users.end()) return;
    if (it->second->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        rt.live_users.erase(it);
    }
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) {
        return RecordHr("XUserGetId", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetId", XGR_E_HANDLE);
    }
    *userId = u->xuid;
    return RecordHr("XUserGetId", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) {
        return RecordHr("XUserGetLocalId", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetLocalId", XGR_E_HANDLE);
    }
    userLocalId->value = u->local_id;
    return RecordHr("XUserGetLocalId", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) {
    if (!user) {
        return RecordHr("XUserGetGamertag", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetGamertag", XGR_E_HANDLE);
    }
    const std::string* src = &u->gamertag_classic;
    switch (gamertagComponent) {
        case XUserGamertagComponent_Classic: src = &u->gamertag_classic; break;
        case XUserGamertagComponent_Modern: src = &u->gamertag_modern; break;
        case XUserGamertagComponent_ModernSuffix: src = &u->gamertag_suffix; break;
        case XUserGamertagComponent_UniqueModern: src = &u->gamertag_unique; break;
        default: return RecordHr("XUserGetGamertag", XGR_E_INVALIDARG);
    }
    return RecordHr("XUserGetGamertag", CopyStringResult(*src, gamertagSize, gamertag, gamertagUsed));
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) {
        return RecordHr("XUserGetState", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetState", XGR_E_HANDLE);
    }
    *state = XUserState_SignedIn;
    return RecordHr("XUserGetState", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup) {
    if (!user || !ageGroup) {
        return RecordHr("XUserGetAgeGroup", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetAgeGroup", XGR_E_HANDLE);
    }
    *ageGroup = XUserAgeGroup_Adult;
    return RecordHr("XUserGetAgeGroup", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) {
    if (!user || !hasPrivilege) {
        return RecordHr("XUserCheckPrivilege", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserCheckPrivilege", XGR_E_HANDLE);
    }
    *hasPrivilege = true;
    if (reason) {
        *reason = XUserPrivilegeDenyReason_None;
    }
    return RecordHr("XUserCheckPrivilege", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetMaxUsers(uint32_t* maxUsers) {
    if (!maxUsers) {
        return RecordHr("XUserGetMaxUsers", XGR_E_INVALIDARG);
    }
    *maxUsers = 16u;
    return RecordHr("XUserGetMaxUsers", XGR_S_OK);
}

int32_t OPENSCARLETT_GDK_CALL XUserCompare(XUserHandle user1, XUserHandle user2) {
    RecordOk("XUserCompare");
    if (user1 == user2) return 0;
    auto u1 = LookupUser(user1);
    auto u2 = LookupUser(user2);
    const uint64_t id1 = u1 ? u1->xuid : 0;
    const uint64_t id2 = u2 ? u2->xuid : 0;
    if (id1 < id2) return -1;
    if (id1 > id2) return 1;
    return 0;
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async) {
    if (!user || !async) {
        return RecordHr("XUserGetGamerPictureAsync", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetGamerPictureAsync", XGR_E_HANDLE);
    }
    auto* ctx = new (std::nothrow) UserGamerPicAsyncCtx();
    if (!ctx) {
        return RecordHr("XUserGetGamerPictureAsync", XGR_E_OUTOFMEMORY);
    }
    ctx->size_enum = pictureSize;
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdUserGamerPicture, kIdUserGamerPicture, &UserGamerPicProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XUserGetGamerPictureAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XUserGetGamerPictureAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return RecordHr("XUserGetGamerPictureResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetGamerPictureResultSize", hr);
    }
    hr = XAsyncGetResultSize(async, bufferSize);
    return RecordHr("XUserGetGamerPictureResultSize", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    uint8_t* buffer,
    size_t* bufferUsed) {
    if (!async || !buffer) {
        return RecordHr("XUserGetGamerPictureResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetGamerPictureResult", hr);
    }
    hr = XAsyncGetResult(async, kIdUserGamerPicture, bufferSize, buffer, bufferUsed);
    return RecordHr("XUserGetGamerPictureResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetIsGuest(XUserHandle user, bool* isGuest) {
    if (!user || !isGuest) {
        return RecordHr("XUserGetIsGuest", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserGetIsGuest", XGR_E_HANDLE);
    }
    *isGuest = false;
    return RecordHr("XUserGetIsGuest", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserFindUserById(uint64_t userId, XUserHandle* handle) {
    if (!handle) {
        return RecordHr("XUserFindUserById", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    for (auto& [raw, u] : rt.live_users) {
        if (u && u->xuid == userId) {
            u->ref_count.fetch_add(1, std::memory_order_relaxed);
            *handle = raw;
            return RecordHr("XUserFindUserById", XGR_S_OK);
        }
    }
    return RecordHr("XUserFindUserById", XGR_E_NOT_FOUND);
}

XGameResult OPENSCARLETT_GDK_CALL XUserFindUserByLocalId(
    XUserLocalId userLocalId,
    XUserHandle* handle) {
    if (!handle) {
        return RecordHr("XUserFindUserByLocalId", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    for (auto& [raw, u] : rt.live_users) {
        if (u && u->local_id == userLocalId.value) {
            u->ref_count.fetch_add(1, std::memory_order_relaxed);
            *handle = raw;
            return RecordHr("XUserFindUserByLocalId", XGR_S_OK);
        }
    }
    return RecordHr("XUserFindUserByLocalId", XGR_E_NOT_FOUND);
}

XGameResult OPENSCARLETT_GDK_CALL XUserFindForDevice(
    const XUserAppLocalDeviceId* deviceId,
    XUserHandle* handle) {
    if (!deviceId || !handle) {
        return RecordHr("XUserFindForDevice", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.user_mutex);
    if (!rt.live_users.empty()) {
        auto it = rt.live_users.begin();
        it->second->ref_count.fetch_add(1, std::memory_order_relaxed);
        *handle = it->first;
        return RecordHr("XUserFindForDevice", XGR_S_OK);
    }
    auto u = CreateDefaultUserObj();
    XUserHandle raw = u.get();
    rt.live_users[raw] = std::move(u);
    *handle = raw;
    return RecordHr("XUserFindForDevice", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const char* method,
    const char* url,
    size_t /*headerCount*/,
    const XUserGetTokenAndSignatureHttpHeader* /*headers*/,
    size_t /*bodySize*/,
    const void* /*bodyBuffer*/,
    XAsyncBlock* async) {
    if (!user || !method || !url || !async) {
        return RecordHr("XUserGetTokenAndSignatureAsync", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetTokenAndSignatureAsync", XGR_E_HANDLE);
    }
    auto* ctx = new (std::nothrow) TokenSigAsyncCtx();
    if (!ctx) {
        return RecordHr("XUserGetTokenAndSignatureAsync", XGR_E_OUTOFMEMORY);
    }
    // Offline-first Xbox Live token policy: default to clean E_GAMEUSER_NO_TOKEN_REQUIRED
    // completion so real titles immediately follow offline paths instead of hanging on live
    // Xbox Live handshakes. Opt in to synthetic XBL3.0 JWT tokens via OPENSCARLETT_MOCK_XSTS=1.
    if (IsMockXstsEnabled()) {
        ctx->result_hr = XGR_S_OK;
        BuildOfflineXstsTokenAndSig(*u, method, url, ctx->token, ctx->signature);
    } else {
        ctx->result_hr = kHrGameUserNoTokenRequired;
    }
    XGameResult hr = XAsyncBegin(async, ctx, kIdUserTokenSig, kIdUserTokenSig, &TokenSigProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XUserGetTokenAndSignatureAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XUserGetTokenAndSignatureAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return RecordHr("XUserGetTokenAndSignatureResultSize", XGR_E_INVALIDARG);
    }
    *bufferSize = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetTokenAndSignatureResultSize", hr);
    }
    hr = XAsyncGetResultSize(async, bufferSize);
    return RecordHr("XUserGetTokenAndSignatureResultSize", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XUserGetTokenAndSignatureResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    if (bufferUsed) {
        *bufferUsed = 0;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetTokenAndSignatureResult", hr);
    }
    hr = XAsyncGetResult(async, kIdUserTokenSig, bufferSize, buffer, bufferUsed);
    if (hr >= 0) {
        *ptrToBuffer = static_cast<XUserGetTokenAndSignatureData*>(buffer);
    }
    return RecordHr("XUserGetTokenAndSignatureResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions /*options*/,
    const wchar_t* method,
    const wchar_t* url,
    size_t /*headerCount*/,
    const XUserGetTokenAndSignatureUtf16HttpHeader* /*headers*/,
    size_t /*bodySize*/,
    const void* /*bodyBuffer*/,
    XAsyncBlock* async) {
    if (!user || !method || !url || !async) {
        return RecordHr("XUserGetTokenAndSignatureUtf16Async", XGR_E_INVALIDARG);
    }
    auto u = LookupUser(user);
    if (!u) {
        return RecordHr("XUserGetTokenAndSignatureUtf16Async", XGR_E_HANDLE);
    }
    auto* ctx = new (std::nothrow) TokenSigUtf16AsyncCtx();
    if (!ctx) {
        return RecordHr("XUserGetTokenAndSignatureUtf16Async", XGR_E_OUTOFMEMORY);
    }
    if (IsMockXstsEnabled()) {
        ctx->result_hr = XGR_S_OK;
        std::string narrow_method;
        for (const wchar_t* p = method; *p; ++p) {
            narrow_method.push_back((*p < 0x80) ? static_cast<char>(*p) : '?');
        }
        std::string narrow_url;
        for (const wchar_t* p = url; *p; ++p) {
            narrow_url.push_back((*p < 0x80) ? static_cast<char>(*p) : '?');
        }
        std::string tok_utf8;
        std::string sig_utf8;
        BuildOfflineXstsTokenAndSig(*u, narrow_method, narrow_url, tok_utf8, sig_utf8);
        ctx->token.assign(tok_utf8.begin(), tok_utf8.end());
        ctx->signature.assign(sig_utf8.begin(), sig_utf8.end());
    } else {
        ctx->result_hr = kHrGameUserNoTokenRequired;
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdUserTokenSigUtf16, kIdUserTokenSigUtf16, &TokenSigUtf16Provider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XUserGetTokenAndSignatureUtf16Async", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XUserGetTokenAndSignatureUtf16Async", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return RecordHr("XUserGetTokenAndSignatureUtf16ResultSize", XGR_E_INVALIDARG);
    }
    *bufferSize = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetTokenAndSignatureUtf16ResultSize", hr);
    }
    hr = XAsyncGetResultSize(async, bufferSize);
    return RecordHr("XUserGetTokenAndSignatureUtf16ResultSize", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XUserGetTokenAndSignatureUtf16Result", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    if (bufferUsed) {
        *bufferUsed = 0;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XUserGetTokenAndSignatureUtf16Result", hr);
    }
    hr = XAsyncGetResult(async, kIdUserTokenSigUtf16, bufferSize, buffer, bufferUsed);
    if (hr >= 0) {
        *ptrToBuffer = static_cast<XUserGetTokenAndSignatureUtf16Data*>(buffer);
    }
    return RecordHr("XUserGetTokenAndSignatureUtf16Result", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetDefaultAudioEndpointUtf16(
    XUserLocalId /*user*/,
    XUserDefaultAudioEndpointKind defaultAudioEndpointKind,
    size_t endpointIdUtf16Count,
    wchar_t* endpointIdUtf16,
    size_t* endpointIdUtf16Used) {
    const std::wstring ep =
        (defaultAudioEndpointKind == XUserDefaultAudioEndpointKind_CommunicationCapture)
            ? L"{0.0.1.00000000}.{openscarlett-audio-capture}"
            : L"{0.0.0.00000000}.{openscarlett-audio-render}";
    return RecordHr(
        "XUserGetDefaultAudioEndpointUtf16",
        CopyWideStringResult(ep, endpointIdUtf16Count, endpointIdUtf16, endpointIdUtf16Used));
}

XGameResult OPENSCARLETT_GDK_CALL XUserGetSignOutDeferral(XUserSignOutDeferralHandle* deferral) {
    if (!deferral) {
        return RecordHr("XUserGetSignOutDeferral", XGR_E_INVALIDARG);
    }
    auto* d = new (std::nothrow) XUserSignOutDeferral();
    if (!d) {
        *deferral = nullptr;
        return RecordHr("XUserGetSignOutDeferral", XGR_E_OUTOFMEMORY);
    }
    *deferral = d;
    return RecordHr("XUserGetSignOutDeferral", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XUserCloseSignOutDeferralHandle(XUserSignOutDeferralHandle deferral) {
    RecordOk("XUserCloseSignOutDeferralHandle");
    delete deferral;
}

XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForChangeEvent(
    XTaskQueueHandle queue,
    void* context,
    XUserChangeEventCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (token) {
        token->token = 0ull;
    }
    if (!callback || !token) {
        return RecordHr("XUserRegisterForChangeEvent", XGR_E_INVALIDARG);
    }
    if (queue != nullptr && !LookupQueue(queue)) {
        return RecordHr("XUserRegisterForChangeEvent", XGR_E_HANDLE);
    }
    const XTaskQueueRegistrationToken reg = AllocateRegToken(kRegUserChange);
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.reg_mutex);
        rt.user_change_listeners.push_back(UserChangeListenerEntry{reg.token, queue, context, callback});
    }
    *token = reg;
    return RecordHr("XUserRegisterForChangeEvent", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XUserUnregisterForChangeEvent(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XUserUnregisterForChangeEvent");
    const bool removed = ReleaseRegToken(kRegUserChange, token);
    if (removed) {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.reg_mutex);
        rt.user_change_listeners.erase(
            std::remove_if(rt.user_change_listeners.begin(), rt.user_change_listeners.end(),
                           [&](const UserChangeListenerEntry& e) { return e.token == token.token; }),
            rt.user_change_listeners.end());
    }
    return removed;
}

OPENSCARLETT_GDK_API bool OPENSCARLETT_GDK_CALL XUserIsStoreUser(XUserHandle user) {
    RecordOk("XUserIsStoreUser");
    if (!user) {
        return false;
    }
    return LookupUser(user) != nullptr;
}

XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueHandle queue,
    void* /*context*/,
    XUserDefaultAudioEndpointUtf16ChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (token) {
        token->token = 0ull;
    }
    if (!callback || !token) {
        return RecordHr("XUserRegisterForDefaultAudioEndpointUtf16Changed", XGR_E_INVALIDARG);
    }
    if (queue != nullptr && !LookupQueue(queue)) {
        return RecordHr("XUserRegisterForDefaultAudioEndpointUtf16Changed", XGR_E_HANDLE);
    }
    *token = AllocateRegToken(kRegUserAudioEndpoint);
    return RecordHr("XUserRegisterForDefaultAudioEndpointUtf16Changed", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XUserUnregisterForDefaultAudioEndpointUtf16Changed(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XUserUnregisterForDefaultAudioEndpointUtf16Changed");
    return ReleaseRegToken(kRegUserAudioEndpoint, token);
}

XGameResult OPENSCARLETT_GDK_CALL XUserRegisterForDeviceAssociationChanged(
    XTaskQueueHandle queue,
    void* /*context*/,
    XUserDeviceAssociationChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (token) {
        token->token = 0ull;
    }
    if (!callback || !token) {
        return RecordHr("XUserRegisterForDeviceAssociationChanged", XGR_E_INVALIDARG);
    }
    if (queue != nullptr && !LookupQueue(queue)) {
        return RecordHr("XUserRegisterForDeviceAssociationChanged", XGR_E_HANDLE);
    }
    *token = AllocateRegToken(kRegUserDeviceAssoc);
    return RecordHr("XUserRegisterForDeviceAssociationChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XUserUnregisterForDeviceAssociationChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XUserUnregisterForDeviceAssociationChanged");
    return ReleaseRegToken(kRegUserDeviceAssoc, token);
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiAsync(
    XUserHandle user,
    const char* /*url*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return RecordHr("XUserResolveIssueWithUiAsync", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserResolveIssueWithUiAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XUserResolveIssueWithUiAsync",
        StartVoidAsync(async, kIdUserResolveIssue, kIdUserResolveIssue, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XUserResolveIssueWithUiResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XUserResolveIssueWithUiResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* /*url*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return RecordHr("XUserResolveIssueWithUiUtf16Async", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserResolveIssueWithUiUtf16Async", XGR_E_HANDLE);
    }
    return RecordHr(
        "XUserResolveIssueWithUiUtf16Async",
        StartVoidAsync(async, kIdUserResolveIssueUtf16, kIdUserResolveIssueUtf16, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolveIssueWithUiUtf16Result(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XUserResolveIssueWithUiUtf16Result", XGR_E_INVALIDARG);
    }
    return RecordHr("XUserResolveIssueWithUiUtf16Result", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    XAsyncBlock* async) {
    if (!user || !async) {
        return RecordHr("XUserResolvePrivilegeWithUiAsync", XGR_E_INVALIDARG);
    }
    if (!LookupUser(user)) {
        return RecordHr("XUserResolvePrivilegeWithUiAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XUserResolvePrivilegeWithUiAsync",
        StartVoidAsync(async, kIdUserResolvePriv, kIdUserResolvePriv, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XUserResolvePrivilegeWithUiResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XUserResolvePrivilegeWithUiResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XUserResolvePrivilegeWithUiResult", XAsyncGetStatus(async, true));
}

// ===========================================================================
// XGameSave (persistent disk storage under OPENSCARLETT_SAVE_ROOT or E:\)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XGameSaveProviderHandle* provider) {
    if (!provider || !configurationId || !*configurationId) {
        return RecordHr("XGameSaveInitializeProvider", XGR_E_INVALIDARG);
    }
    *provider = nullptr;

    uint64_t xuid = DefaultUserXuid();
    if (requestingUser) {
        auto u = LookupUser(requestingUser);
        if (!u) {
            return RecordHr("XGameSaveInitializeProvider", XGR_E_HANDLE);
        }
        xuid = u->xuid;
    }

    auto p = std::make_shared<XGameSaveProvider>();
    p->xuid = xuid;
    p->scid = configurationId;
    p->root_dir = ResolveSaveRoot() / "xgamesave" / EncodePathToken(p->scid) / FormatXuidHex(xuid);

    std::error_code ec;
    fs::create_directories(p->root_dir, ec);
    if (ec) {
        return RecordHr("XGameSaveInitializeProvider", XGR_E_FAIL);
    }

    XGameSaveProviderHandle raw = p.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_providers[raw] = std::move(p);
    }
    *provider = raw;
    return RecordHr("XGameSaveInitializeProvider", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) {
    if (!async || !configurationId || !*configurationId) {
        return RecordHr("XGameSaveInitializeProviderAsync", XGR_E_INVALIDARG);
    }
    XUserHandle dup_user = nullptr;
    if (requestingUser) {
        XGameResult hr = XUserDuplicateHandle(requestingUser, &dup_user);
        if (hr < 0) {
            return RecordHr("XGameSaveInitializeProviderAsync", hr);
        }
    }
    auto* ctx = new (std::nothrow) GameSaveInitAsyncCtx();
    if (!ctx) {
        if (dup_user) XUserCloseHandle(dup_user);
        return RecordHr("XGameSaveInitializeProviderAsync", XGR_E_OUTOFMEMORY);
    }
    ctx->requesting_user = dup_user;
    ctx->scid = configurationId;
    ctx->sync_on_demand = syncOnDemand;

    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveInit, kIdGameSaveInit, &GameSaveInitProvider);
    if (hr < 0) {
        if (ctx->requesting_user) XUserCloseHandle(ctx->requesting_user);
        delete ctx;
        return RecordHr("XGameSaveInitializeProviderAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveInitializeProviderAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) {
    if (!async || !provider) {
        return RecordHr("XGameSaveInitializeProviderResult", XGR_E_INVALIDARG);
    }
    *provider = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameSaveInitializeProviderResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdGameSaveInit, sizeof(XGameSaveProviderHandle), provider, nullptr);
    return RecordHr("XGameSaveInitializeProviderResult", hr);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    RecordOk("XGameSaveCloseProvider");
    if (!provider) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_providers.erase(provider);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* result) {
    if (!provider || !name || !*name || !result) {
        return RecordHr("XGameSaveCreateContainer", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    std::shared_ptr<XGameSaveProvider> prov;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveCreateContainer", XGR_E_HANDLE);
        }
        prov = it->second;
    }

    auto c = std::make_shared<XGameSaveContainer>();
    c->name = name;
    c->container_dir = prov->root_dir / EncodePathToken(c->name);

    XGameSaveContainerHandle raw = c.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_containers[raw] = std::move(c);
    }
    *result = raw;
    return RecordHr("XGameSaveCreateContainer", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    RecordOk("XGameSaveCloseContainer");
    if (!context) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_containers.erase(context);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* containerDisplayName,
    XGameSaveUpdateHandle* result) {
    if (!container || !result) {
        return RecordHr("XGameSaveCreateUpdate", XGR_E_INVALIDARG);
    }
    *result = nullptr;
    std::shared_ptr<XGameSaveContainer> c;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveCreateUpdate", XGR_E_HANDLE);
        }
        c = it->second;
    }

    auto upd = std::make_shared<XGameSaveUpdate>();
    upd->container_name = c->name;
    upd->display_name = (containerDisplayName && *containerDisplayName)
                            ? containerDisplayName
                            : c->name;
    upd->container_dir = c->container_dir;

    XGameSaveUpdateHandle raw = upd.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        rt.live_updates[raw] = std::move(upd);
    }
    *result = raw;
    return RecordHr("XGameSaveCreateUpdate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount) {
    if (!updateContext || !blobName || !*blobName || (byteCount > 0 && !data)) {
        return RecordHr("XGameSaveSubmitBlobWrite", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitBlobWrite", XGR_E_HANDLE);
    }
    XGameSaveUpdate::PendingOp op{};
    op.is_delete = false;
    op.blob_name = blobName;
    if (byteCount > 0) {
        op.data.assign(data, data + byteCount);
    }
    it->second->ops.push_back(std::move(op));
    return RecordHr("XGameSaveSubmitBlobWrite", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) {
    if (!updateContext || !blobName || !*blobName) {
        return RecordHr("XGameSaveSubmitBlobDelete", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitBlobDelete", XGR_E_HANDLE);
    }
    XGameSaveUpdate::PendingOp op{};
    op.is_delete = true;
    op.blob_name = blobName;
    it->second->ops.push_back(std::move(op));
    return RecordHr("XGameSaveSubmitBlobDelete", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (!updateContext) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    auto it = rt.live_updates.find(updateContext);
    if (it == rt.live_updates.end()) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_HANDLE);
    }
    auto& upd = *it->second;

    std::error_code ec;
    fs::create_directories(upd.container_dir, ec);
    if (ec) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(upd.container_dir, meta)) {
        meta.name = upd.container_name;
    }
    meta.display_name = upd.display_name;
    meta.last_modified = NowUnixSeconds();

    for (const auto& op : upd.ops) {
        const std::string file_name = EncodePathToken(op.blob_name) + ".blob";
        const fs::path blob_path = upd.container_dir / file_name;
        if (op.is_delete) {
            fs::remove(blob_path, ec);
            meta.blobs.erase(op.blob_name);
        } else {
            if (!WriteBinaryFileAtomic(blob_path, op.data.data(), op.data.size())) {
                return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
            }
            meta.blobs[op.blob_name] = file_name;
        }
    }

    if (!SaveContainerMeta(upd.container_dir, meta)) {
        return RecordHr("XGameSaveSubmitUpdate", XGR_E_FAIL);
    }
    return RecordHr("XGameSaveSubmitUpdate", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameSaveCloseUpdate(XGameSaveUpdateHandle updateContext) {
    RecordOk("XGameSaveCloseUpdate");
    if (!updateContext) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.save_mutex);
    rt.live_updates.erase(updateContext);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) {
    if (!container || !countOfBlobs || !blobData) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(dir, meta)) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
    }

    std::vector<std::string> requested;
    if (blobNames && *countOfBlobs > 0) {
        requested.reserve(*countOfBlobs);
        for (uint32_t i = 0; i < *countOfBlobs; ++i) {
            if (!blobNames[i] || !*blobNames[i]) {
                return RecordHr("XGameSaveReadBlobData", XGR_E_INVALIDARG);
            }
            requested.emplace_back(blobNames[i]);
        }
    } else {
        for (const auto& [bname, _] : meta.blobs) {
            requested.push_back(bname);
        }
    }

    struct LoadedBlob {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    std::vector<LoadedBlob> loaded;
    loaded.reserve(requested.size());

    size_t required_bytes = requested.size() * sizeof(XGameSaveBlob);
    for (const auto& req_name : requested) {
        auto mit = meta.blobs.find(req_name);
        if (mit == meta.blobs.end()) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
        }
        LoadedBlob lb{};
        lb.name = req_name;
        if (!ReadBinaryFile(dir / mit->second, lb.bytes)) {
            return RecordHr("XGameSaveReadBlobData", XGR_E_GS_BLOB_NOT_FOUND);
        }
        required_bytes += (lb.name.size() + 1) + lb.bytes.size();
        loaded.push_back(std::move(lb));
    }

    if (blobsSize < required_bytes) {
        return RecordHr("XGameSaveReadBlobData", XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL);
    }

    auto* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = loaded.size() * sizeof(XGameSaveBlob);
    for (size_t i = 0; i < loaded.size(); ++i) {
        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, loaded[i].name.c_str(), loaded[i].name.size() + 1);
        cursor += loaded[i].name.size() + 1;

        uint8_t* data_dst = base + cursor;
        if (!loaded[i].bytes.empty()) {
            std::memcpy(data_dst, loaded[i].bytes.data(), loaded[i].bytes.size());
            cursor += loaded[i].bytes.size();
        }

        blobData[i].info.name = name_dst;
        blobData[i].info.size = static_cast<uint32_t>(loaded[i].bytes.size());
        blobData[i].data = loaded[i].bytes.empty() ? nullptr : data_dst;
    }

    *countOfBlobs = static_cast<uint32_t>(loaded.size());
    return RecordHr("XGameSaveReadBlobData", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback callback) {
    if (!provider || !callback) {
        return RecordHr("XGameSaveEnumerateContainerInfo", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveEnumerateContainerInfo", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    std::error_code ec;
    if (!fs::exists(root_dir, ec)) {
        return RecordHr("XGameSaveEnumerateContainerInfo", XGR_S_OK);
    }

    for (const auto& entry : fs::directory_iterator(root_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        ContainerMeta meta{};
        if (!LoadContainerMeta(entry.path(), meta)) continue;

        uint64_t total_size = 0;
        for (const auto& [_, file_name] : meta.blobs) {
            std::error_code fec;
            const auto sz = fs::file_size(entry.path() / file_name, fec);
            if (!fec) total_size += sz;
        }

        XGameSaveContainerInfo info{};
        info.name = meta.name.c_str();
        info.displayName = meta.display_name.c_str();
        info.blobCount = static_cast<uint32_t>(meta.blobs.size());
        info.totalSize = total_size;
        info.lastModifiedTime = meta.last_modified;
        info.needsSync = false;

        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateContainerInfo", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback callback) {
    if (!container || !callback) {
        return RecordHr("XGameSaveEnumerateBlobInfo", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveEnumerateBlobInfo", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(dir, meta)) {
        return RecordHr("XGameSaveEnumerateBlobInfo", XGR_S_OK);
    }
    for (const auto& [blob_name, file_name] : meta.blobs) {
        std::error_code ec;
        const auto sz = fs::file_size(dir / file_name, ec);
        XGameSaveBlobInfo info{};
        info.name = blob_name.c_str();
        info.size = ec ? 0u : static_cast<uint32_t>(sz);
        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateBlobInfo", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name) {
    if (!provider || !name || !*name) {
        return RecordHr("XGameSaveDeleteContainer", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveDeleteContainer", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    const fs::path cdir = root_dir / EncodePathToken(name);
    std::error_code ec;
    fs::remove_all(cdir, ec);
    return RecordHr("XGameSaveDeleteContainer", ec ? XGR_E_FAIL : XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) {
    if (!provider || !remainingQuota) {
        return RecordHr("XGameSaveGetRemainingQuota", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveGetRemainingQuota", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    constexpr int64_t kQuotaBytes = 256LL * 1024LL * 1024LL;
    int64_t used = 0;
    std::error_code ec;
    if (fs::exists(root_dir, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(root_dir, ec)) {
            if (entry.is_regular_file(ec)) {
                used += static_cast<int64_t>(entry.file_size(ec));
            }
        }
    }
    *remainingQuota = std::max<int64_t>(0, kQuotaBytes - used);
    return RecordHr("XGameSaveGetRemainingQuota", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateContainerInfoByName(
    XGameSaveProviderHandle provider,
    const char* containerNamePrefix,
    void* context,
    XGameSaveContainerInfoCallback callback) {
    if (!provider || !callback) {
        return RecordHr("XGameSaveEnumerateContainerInfoByName", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveEnumerateContainerInfoByName", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    const std::string prefix = containerNamePrefix ? containerNamePrefix : "";
    std::error_code ec;
    if (!fs::exists(root_dir, ec)) {
        return RecordHr("XGameSaveEnumerateContainerInfoByName", XGR_S_OK);
    }

    for (const auto& entry : fs::directory_iterator(root_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        ContainerMeta meta{};
        if (!LoadContainerMeta(entry.path(), meta)) continue;
        if (!prefix.empty() && meta.name.rfind(prefix, 0) != 0) {
            continue;
        }

        uint64_t total_size = 0;
        for (const auto& [_, file_name] : meta.blobs) {
            std::error_code fec;
            const auto sz = fs::file_size(entry.path() / file_name, fec);
            if (!fec) total_size += sz;
        }

        XGameSaveContainerInfo info{};
        info.name = meta.name.c_str();
        info.displayName = meta.display_name.c_str();
        info.blobCount = static_cast<uint32_t>(meta.blobs.size());
        info.totalSize = total_size;
        info.lastModifiedTime = meta.last_modified;
        info.needsSync = false;

        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateContainerInfoByName", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveEnumerateBlobInfoByName(
    XGameSaveContainerHandle container,
    const char* blobNamePrefix,
    void* context,
    XGameSaveBlobInfoCallback callback) {
    if (!container || !callback) {
        return RecordHr("XGameSaveEnumerateBlobInfoByName", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveEnumerateBlobInfoByName", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    ContainerMeta meta{};
    if (!LoadContainerMeta(dir, meta)) {
        return RecordHr("XGameSaveEnumerateBlobInfoByName", XGR_S_OK);
    }
    const std::string prefix = blobNamePrefix ? blobNamePrefix : "";
    for (const auto& [blob_name, file_name] : meta.blobs) {
        if (!prefix.empty() && blob_name.rfind(prefix, 0) != 0) {
            continue;
        }
        std::error_code ec;
        const auto sz = fs::file_size(dir / file_name, ec);
        XGameSaveBlobInfo info{};
        info.name = blob_name.c_str();
        info.size = ec ? 0u : static_cast<uint32_t>(sz);
        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XGameSaveEnumerateBlobInfoByName", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetContainerInfo(
    XGameSaveProviderHandle provider,
    const char* containerName,
    void* context,
    XGameSaveContainerInfoCallback callback) {
    if (!provider || !containerName || !*containerName || !callback) {
        return RecordHr("XGameSaveGetContainerInfo", XGR_E_INVALIDARG);
    }
    fs::path root_dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_providers.find(provider);
        if (it == rt.live_providers.end()) {
            return RecordHr("XGameSaveGetContainerInfo", XGR_E_HANDLE);
        }
        root_dir = it->second->root_dir;
    }

    const fs::path cdir = root_dir / EncodePathToken(containerName);
    ContainerMeta meta{};
    if (!LoadContainerMeta(cdir, meta)) {
        return RecordHr("XGameSaveGetContainerInfo", XGR_E_GS_BLOB_NOT_FOUND);
    }

    uint64_t total_size = 0;
    for (const auto& [_, file_name] : meta.blobs) {
        std::error_code fec;
        const auto sz = fs::file_size(cdir / file_name, fec);
        if (!fec) total_size += sz;
    }

    XGameSaveContainerInfo info{};
    info.name = meta.name.c_str();
    info.displayName = meta.display_name.c_str();
    info.blobCount = static_cast<uint32_t>(meta.blobs.size());
    info.totalSize = total_size;
    info.lastModifiedTime = meta.last_modified;
    info.needsSync = false;
    (void)callback(&info, context);
    return RecordHr("XGameSaveGetContainerInfo", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) {
    if (!container || !async) {
        return RecordHr("XGameSaveReadBlobDataAsync", XGR_E_INVALIDARG);
    }
    fs::path dir;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.save_mutex);
        auto it = rt.live_containers.find(container);
        if (it == rt.live_containers.end()) {
            return RecordHr("XGameSaveReadBlobDataAsync", XGR_E_HANDLE);
        }
        dir = it->second->container_dir;
    }

    auto* ctx = new (std::nothrow) GameSaveReadBlobAsyncCtx();
    if (!ctx) {
        return RecordHr("XGameSaveReadBlobDataAsync", XGR_E_OUTOFMEMORY);
    }
    ctx->container_dir = std::move(dir);
    if (blobNames && countOfBlobs > 0) {
        ctx->requested_names.reserve(countOfBlobs);
        for (uint32_t i = 0; i < countOfBlobs; ++i) {
            if (!blobNames[i] || !*blobNames[i]) {
                delete ctx;
                return RecordHr("XGameSaveReadBlobDataAsync", XGR_E_INVALIDARG);
            }
            ctx->requested_names.emplace_back(blobNames[i]);
        }
    }

    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveReadBlob, kIdGameSaveReadBlob, &GameSaveReadBlobProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameSaveReadBlobDataAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveReadBlobDataAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) {
    if (!async || !blobData || !countOfBlobs) {
        return RecordHr("XGameSaveReadBlobDataResult", XGR_E_INVALIDARG);
    }
    *countOfBlobs = 0;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameSaveReadBlobDataResult", hr);
    }
    auto rec = LookupAsync(async);
    if (!rec || rec->identity != kIdGameSaveReadBlob) {
        return RecordHr("XGameSaveReadBlobDataResult", XGR_E_INVALIDARG);
    }
    auto* ctx = static_cast<GameSaveReadBlobAsyncCtx*>(rec->provider_context);
    if (!ctx) {
        return RecordHr("XGameSaveReadBlobDataResult", XGR_E_FAIL);
    }
    if (blobsSize < ctx->required_bytes) {
        return RecordHr("XGameSaveReadBlobDataResult", XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL);
    }
    const uint32_t blob_count = static_cast<uint32_t>(ctx->loaded.size());
    hr = XAsyncGetResult(async, kIdGameSaveReadBlob, blobsSize, blobData, nullptr);
    if (hr >= 0) {
        *countOfBlobs = blob_count;
    }
    return RecordHr("XGameSaveReadBlobDataResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) {
    if (!updateContext || !async) {
        return RecordHr("XGameSaveSubmitUpdateAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) GameSaveSubmitUpdateAsyncCtx{updateContext};
    if (!ctx) {
        return RecordHr("XGameSaveSubmitUpdateAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveSubmitUpdate, kIdGameSaveSubmitUpdate, &GameSaveSubmitUpdateProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameSaveSubmitUpdateAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveSubmitUpdateAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameSaveSubmitUpdateResult", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    return RecordHr("XGameSaveSubmitUpdateResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerAsync(
    XGameSaveProviderHandle provider,
    const char* name,
    XAsyncBlock* async) {
    if (!provider || !name || !*name || !async) {
        return RecordHr("XGameSaveDeleteContainerAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) GameSaveDeleteContainerAsyncCtx{provider, name};
    if (!ctx) {
        return RecordHr("XGameSaveDeleteContainerAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveDeleteContainer, kIdGameSaveDeleteContainer,
        &GameSaveDeleteContainerProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameSaveDeleteContainerAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveDeleteContainerAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveDeleteContainerResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameSaveDeleteContainerResult", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    return RecordHr("XGameSaveDeleteContainerResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaAsync(
    XGameSaveProviderHandle provider,
    XAsyncBlock* async) {
    if (!provider || !async) {
        return RecordHr("XGameSaveGetRemainingQuotaAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) GameSaveQuotaAsyncCtx{provider, 0};
    if (!ctx) {
        return RecordHr("XGameSaveGetRemainingQuotaAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameSaveQuota, kIdGameSaveQuota, &GameSaveQuotaProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameSaveGetRemainingQuotaAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameSaveGetRemainingQuotaAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameSaveGetRemainingQuotaResult(
    XAsyncBlock* async,
    int64_t* remainingQuota) {
    if (!async || !remainingQuota) {
        return RecordHr("XGameSaveGetRemainingQuotaResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameSaveGetRemainingQuotaResult", hr);
    }
    hr = XAsyncGetResult(async, kIdGameSaveQuota, sizeof(int64_t), remainingQuota, nullptr);
    return RecordHr("XGameSaveGetRemainingQuotaResult", hr);
}

// ===========================================================================
// XSystem
// ===========================================================================
XSystemDeviceType OPENSCARLETT_GDK_CALL XSystemGetDeviceType(void) {
    RecordOk("XSystemGetDeviceType");
    return XSystemDeviceType_XboxScarlettAnaconda;
}

XGameResult OPENSCARLETT_GDK_CALL XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) {
    const std::string cid = "OS-SCARLETT-0001.0001.0001.0001";
    return RecordHr("XSystemGetConsoleId",
                    CopyStringResult(cid, consoleIdSize, consoleId, consoleIdUsed));
}

XGameResult OPENSCARLETT_GDK_CALL XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) {
    std::string sb = GetEnvString("OPENSCARLETT_SANDBOX_ID");
    if (sb.empty()) sb = "RETAIL";
    return RecordHr("XSystemGetXboxLiveSandboxId",
                    CopyStringResult(sb, sandboxIdSize, sandboxId, sandboxIdUsed));
}

XSystemAnalyticsInfo OPENSCARLETT_GDK_CALL XSystemGetAnalyticsInfo(void) {
    RecordOk("XSystemGetAnalyticsInfo");
    XSystemAnalyticsInfo info{};
    info.osVersion.major = 10;
    info.osVersion.minor = 0;
    info.osVersion.build = 26100;
    info.osVersion.revision = 2499;
    info.hostingOsVersion = info.osVersion;
    std::snprintf(info.family, sizeof(info.family), "%s", "Windows.Xbox");
    std::snprintf(info.form, sizeof(info.form), "%s", "Xbox Series X");
    return info;
}

XSystemRuntimeInfo OPENSCARLETT_GDK_CALL XSystemGetRuntimeInfo(void) {
    RecordOk("XSystemGetRuntimeInfo");
    XSystemRuntimeInfo info{};
    info.runtimeVersion.major = 10;
    info.runtimeVersion.minor = 0;
    info.runtimeVersion.build = 26100;
    info.runtimeVersion.revision = 2499;
    info.availableVersion = info.runtimeVersion;
    return info;
}

XGameResult OPENSCARLETT_GDK_CALL XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed) {
    // Deterministic 44-character base64 app-specific device ID (+ null = 45 bytes).
    const std::string asid = "T3BlblNjYXJsZXR0QXBwU3BlY2lmaWNEZXZpY2VJZA==";
    return RecordHr("XSystemGetAppSpecificDeviceId",
                    CopyStringResult(asid, appSpecificDeviceIdSize, appSpecificDeviceId,
                                     appSpecificDeviceIdUsed));
}

bool OPENSCARLETT_GDK_CALL XSystemIsHandleValid(void* handle) {
    RecordOk("XSystemIsHandleValid");
    return handle != nullptr && handle != reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ===========================================================================
// XGameStreaming (Local-PC Compatibility Surface)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameStreamingInitialize(void) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.streaming_mutex);
    ++rt.streaming_init_count;
    return RecordHr("XGameStreamingInitialize", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XGameStreamingUninitialize(void) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.streaming_mutex);
    if (rt.streaming_init_count > 0u) {
        --rt.streaming_init_count;
    }
    if (rt.streaming_init_count == 0u) {
        rt.active_touch_layout.clear();
    }
    RecordOk("XGameStreamingUninitialize");
}

bool OPENSCARLETT_GDK_CALL XGameStreamingIsStreaming(void) {
    RecordOk("XGameStreamingIsStreaming");
    return false;
}

uint32_t OPENSCARLETT_GDK_CALL XGameStreamingGetClientCount(void) {
    RecordOk("XGameStreamingGetClientCount");
    return 0u;
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetClients(
    uint32_t clientCount,
    XGameStreamingClientId* clients,
    uint32_t* clientsUsed) {
    if (!clientsUsed || (clientCount > 0u && !clients)) {
        return RecordHr("XGameStreamingGetClients", XGR_E_INVALIDARG);
    }
    *clientsUsed = 0u;
    return RecordHr("XGameStreamingGetClients", XGR_S_OK);
}

XGameStreamingConnectionState OPENSCARLETT_GDK_CALL XGameStreamingGetConnectionState(
    XGameStreamingClientId /*client*/) {
    RecordOk("XGameStreamingGetConnectionState");
    return XGameStreamingConnectionState_Disconnected;
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingRegisterConnectionStateChanged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XGameStreamingConnectionStateChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr("XGameStreamingRegisterConnectionStateChanged", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.reg_mutex);
    const uint64_t tok = rt.next_reg_token++;
    rt.live_reg_tokens[tok] = kRegGameStreamingConnection;
    token->token = tok;
    return RecordHr("XGameStreamingRegisterConnectionStateChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XGameStreamingUnregisterConnectionStateChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XGameStreamingUnregisterConnectionStateChanged");
    if (token.token == 0ull) {
        return false;
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.reg_mutex);
    auto it = rt.live_reg_tokens.find(token.token);
    if (it == rt.live_reg_tokens.end() || it->second != kRegGameStreamingConnection) {
        return false;
    }
    rt.live_reg_tokens.erase(it);
    return true;
}

void OPENSCARLETT_GDK_CALL XGameStreamingHideTouchControls(void) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.streaming_mutex);
    rt.active_touch_layout.clear();
    RecordOk("XGameStreamingHideTouchControls");
}

void OPENSCARLETT_GDK_CALL XGameStreamingShowTouchControlLayout(const char* layout) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.streaming_mutex);
    rt.active_touch_layout = layout ? std::string(layout) : std::string();
    RecordOk("XGameStreamingShowTouchControlLayout");
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetStreamPhysicalDimensions(
    XGameStreamingClientId /*client*/,
    uint32_t* horizontalMm,
    uint32_t* verticalMm) {
    if (!horizontalMm || !verticalMm) {
        return RecordHr("XGameStreamingGetStreamPhysicalDimensions", XGR_E_INVALIDARG);
    }
    *horizontalMm = 0u;
    *verticalMm = 0u;
    return RecordHr("XGameStreamingGetStreamPhysicalDimensions", XGR_S_OK);
}

size_t OPENSCARLETT_GDK_CALL XGameStreamingGetServerLocationNameSize(void) {
    RecordOk("XGameStreamingGetServerLocationNameSize");
    return sizeof("LocalPC");
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetServerLocationName(
    size_t serverLocationNameSize,
    char* serverLocationName) {
    const std::string loc = "LocalPC";
    return RecordHr("XGameStreamingGetServerLocationName",
                    CopyStringResult(loc, serverLocationNameSize, serverLocationName, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingGetLastFrameDisplayed(
    XGameStreamingClientId /*client*/,
    uint64_t* frameId) {
    if (!frameId) {
        return RecordHr("XGameStreamingGetLastFrameDisplayed", XGR_E_INVALIDARG);
    }
    *frameId = 0ull;
    return RecordHr("XGameStreamingGetLastFrameDisplayed", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameStreamingIsTouchInputEnabledOnClient(
    XGameStreamingClientId /*client*/,
    bool* touchInputEnabled) {
    if (!touchInputEnabled) {
        return RecordHr("XGameStreamingIsTouchInputEnabledOnClient", XGR_E_INVALIDARG);
    }
    *touchInputEnabled = false;
    return RecordHr("XGameStreamingIsTouchInputEnabledOnClient", XGR_S_OK);
}

// ===========================================================================
// XMem
// ===========================================================================
void* OPENSCARLETT_GDK_CALL XMemAllocDefault(size_t size, uint64_t /*attributes*/) {
    RecordOk("XMemAllocDefault");
    const size_t alloc_size = size == 0 ? 16u : size;
#if defined(_WIN32)
    return _aligned_malloc(alloc_size, 16u);
#else
    return std::malloc(alloc_size);
#endif
}

void OPENSCARLETT_GDK_CALL XMemFreeDefault(void* pointer, uint64_t /*attributes*/) {
    RecordOk("XMemFreeDefault");
    if (!pointer) return;
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* OPENSCARLETT_GDK_CALL XMemAlloc(size_t size, uint64_t attributes) {
    XMemAllocFunction hook = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mem_mutex);
        hook = rt.alloc_hook;
    }
    RecordOk("XMemAlloc");
    if (hook) {
        return hook(size, attributes);
    }
    return XMemAllocDefault(size, attributes);
}

void OPENSCARLETT_GDK_CALL XMemFree(void* pointer, uint64_t attributes) {
    XMemFreeFunction hook = nullptr;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.mem_mutex);
        hook = rt.free_hook;
    }
    RecordOk("XMemFree");
    if (hook) {
        hook(pointer, attributes);
        return;
    }
    XMemFreeDefault(pointer, attributes);
}

XGameResult OPENSCARLETT_GDK_CALL XMemSetAllocationHooks(
    XMemAllocFunction allocHook,
    XMemFreeFunction freeHook) {
    if ((allocHook != nullptr) != (freeHook != nullptr)) {
        return RecordHr("XMemSetAllocationHooks", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.mem_mutex);
    rt.alloc_hook = allocHook;
    rt.free_hook = freeHook;
    return RecordHr("XMemSetAllocationHooks", XGR_S_OK);
}

// ===========================================================================
// XPackage
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XPackageGetCurrentProcessPackageIdentifier(
    size_t bufferSize,
    char* buffer) {
    const std::string pkg_id = CurrentPackageIdentifier();
    return RecordHr("XPackageGetCurrentProcessPackageIdentifier",
                    CopyStringResult(pkg_id, bufferSize, buffer, nullptr));
}

bool OPENSCARLETT_GDK_CALL XPackageIsPackagedProcess(void) {
    RecordOk("XPackageIsPackagedProcess");
    return true;
}

XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    void* context,
    XPackageChunkAvailabilityCallback callback) {
    if (!callback || (selectorCount > 0u && !selectors)) {
        return RecordHr("XPackageEnumerateChunkAvailability", XGR_E_INVALIDARG);
    }
    (void)packageIdentifier;
    if (selectors && selectorCount > 0u) {
        XPackageChunkAvailability overall = XPackageChunkAvailability_Ready;
        const XGameResult val_hr = ValidateAndEvaluateSelectors(selectorCount, selectors, overall);
        if (val_hr < 0) {
            return RecordHr("XPackageEnumerateChunkAvailability", val_hr);
        }
        const std::vector<std::string> installed = ActiveInstalledLanguagesLower();
        for (uint32_t i = 0u; i < selectorCount; ++i) {
            const XPackageChunkSelector& sel = selectors[i];
            if (sel.type == XPackageChunkSelectorType_Language) {
                const auto langs = SplitDelimitedTokens(sel.language, ';');
                bool keep_going = true;
                for (const auto& lang : langs) {
                    XPackageChunkSelector single{};
                    single.type = XPackageChunkSelectorType_Language;
                    single.language = lang.c_str();
                    const XPackageChunkAvailability avail =
                        LanguageTokenMatchesInstalled(lang, installed)
                            ? XPackageChunkAvailability_Ready
                            : XPackageChunkAvailability_Unavailable;
                    if (!callback(context, &single, avail)) {
                        keep_going = false;
                        break;
                    }
                }
                if (!keep_going) {
                    break;
                }
            } else {
                if (!callback(context, &sel, XPackageChunkAvailability_Ready)) {
                    break;
                }
            }
        }
    } else {
        XPackageChunkSelector default_chunk{};
        default_chunk.type = XPackageChunkSelectorType_Chunk;
        default_chunk.chunkId = 1u;
        callback(context, &default_chunk, XPackageChunkAvailability_Ready);
    }
    return RecordHr("XPackageEnumerateChunkAvailability", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageMount(
    const char* packageIdentifier,
    XPackageMountHandle* mount) {
    if (!packageIdentifier || !*packageIdentifier || !mount) {
        return RecordHr("XPackageMount", XGR_E_INVALIDARG);
    }
    *mount = nullptr;
    auto m = std::make_shared<XPackageMountObject>();
    m->package_id = packageIdentifier;
    m->mount_path = CurrentPackageMountPath();
    XPackageMountHandle raw = m.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.package_mutex);
        rt.live_package_mounts[raw] = std::move(m);
    }
    *mount = raw;
    return RecordHr("XPackageMount", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XPackageCloseMountHandle(XPackageMountHandle mount) {
    RecordOk("XPackageCloseMountHandle");
    if (!mount) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.package_mutex);
    rt.live_package_mounts.erase(mount);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageGetMountPathSize(
    XPackageMountHandle mount,
    size_t* pathSize) {
    if (!mount || !pathSize) {
        return RecordHr("XPackageGetMountPathSize", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.package_mutex);
    auto it = rt.live_package_mounts.find(mount);
    if (it == rt.live_package_mounts.end()) {
        return RecordHr("XPackageGetMountPathSize", XGR_E_HANDLE);
    }
    *pathSize = it->second->mount_path.size() + 1;
    return RecordHr("XPackageGetMountPathSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageGetMountPath(
    XPackageMountHandle mount,
    size_t pathSize,
    char* path) {
    if (!mount || !path) {
        return RecordHr("XPackageGetMountPath", XGR_E_INVALIDARG);
    }
    std::string mp;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.package_mutex);
        auto it = rt.live_package_mounts.find(mount);
        if (it == rt.live_package_mounts.end()) {
            return RecordHr("XPackageGetMountPath", XGR_E_HANDLE);
        }
        mp = it->second->mount_path;
    }
    return RecordHr("XPackageGetMountPath", CopyStringResult(mp, pathSize, path, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XPackageGetUserLocale(size_t localeSize, char* locale) {
    std::string loc = GetEnvString("OPENSCARLETT_LOCALE");
    if (loc.empty()) loc = "en-US";
    return RecordHr("XPackageGetUserLocale", CopyStringResult(loc, localeSize, locale, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XPackageGetWriteStats(XPackageWriteStats* writeStats) {
    if (!writeStats) {
        return RecordHr("XPackageGetWriteStats", XGR_E_INVALIDARG);
    }
    writeStats->interval = 300000ULL;
    writeStats->budget = 1024ULL * 1024ULL * 1024ULL;
    writeStats->elapsed = 1000ULL;
    writeStats->bytesWritten = 0ULL;
    return RecordHr("XPackageGetWriteStats", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t /*selectorCount*/,
    const XPackageChunkSelector* /*selectors*/,
    uint32_t /*minimumUpdateIntervalMs*/,
    XTaskQueueHandle /*queue*/,
    XPackageInstallationMonitorHandle* installationMonitor) {
    if (!packageIdentifier || !*packageIdentifier || !installationMonitor) {
        return RecordHr("XPackageCreateInstallationMonitor", XGR_E_INVALIDARG);
    }
    *installationMonitor = CreateInstallMonitorHandle(packageIdentifier);
    return RecordHr("XPackageCreateInstallationMonitor", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor) {
    RecordOk("XPackageCloseInstallationMonitorHandle");
    if (!installationMonitor) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.package_mutex);
    rt.live_install_monitors.erase(installationMonitor);
}

void OPENSCARLETT_GDK_CALL XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress) {
    RecordOk("XPackageGetInstallationProgress");
    if (!progress) return;
    uint64_t total = 1024ULL * 1024ULL;
    if (installationMonitor) {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.package_mutex);
        auto it = rt.live_install_monitors.find(installationMonitor);
        if (it != rt.live_install_monitors.end()) {
            total = it->second->total_bytes;
        }
    }
    progress->totalBytes = total;
    progress->installedBytes = total;
    progress->launchBytes = total;
    progress->launchable = true;
    progress->completed = true;
}

bool OPENSCARLETT_GDK_CALL XPackageUpdateInstallationMonitor(
    XPackageInstallationMonitorHandle /*installationMonitor*/) {
    RecordOk("XPackageUpdateInstallationMonitor");
    return false;
}

XGameResult OPENSCARLETT_GDK_CALL XPackageRegisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    void* /*context*/,
    XPackageInstallationProgressCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!installationMonitor || !callback || !token) {
        return RecordHr("XPackageRegisterInstallationProgressChanged", XGR_E_INVALIDARG);
    }
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.package_mutex);
        if (rt.live_install_monitors.find(installationMonitor) == rt.live_install_monitors.end()) {
            return RecordHr("XPackageRegisterInstallationProgressChanged", XGR_E_HANDLE);
        }
    }
    *token = AllocateRegToken(kRegPackageProgress);
    return RecordHr("XPackageRegisterInstallationProgressChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XPackageUnregisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle /*installationMonitor*/,
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XPackageUnregisterInstallationProgressChanged");
    return ReleaseRegToken(kRegPackageProgress, token);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageRegisterPackageInstalled(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XPackageInstalledCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr("XPackageRegisterPackageInstalled", XGR_E_INVALIDARG);
    }
    *token = AllocateRegToken(kRegPackageInstalled);
    return RecordHr("XPackageRegisterPackageInstalled", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XPackageUnregisterPackageInstalled(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XPackageUnregisterPackageInstalled");
    return ReleaseRegToken(kRegPackageInstalled, token);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope /*scope*/,
    void* context,
    XPackageEnumerationCallback callback) {
    if (!callback) {
        return RecordHr("XPackageEnumeratePackages", XGR_E_INVALIDARG);
    }
    if (kind == XPackageKind_Game) {
        const std::string pkg_id = CurrentPackageIdentifier();
        const std::string cur_store_id = CurrentStoreId();
        const GameConfigMetadata cfg = LoadGameConfigMetadata();
        const std::string disp_name =
            cfg.display_name.empty() ? "OpenScarlett Title" : cfg.display_name;
        uint32_t tid = 0x5CA00001u;
        (void)XGameGetXboxTitleId(&tid);
        char tid_buf[16]{};
        std::snprintf(tid_buf, sizeof(tid_buf), "%08X", tid);

        XPackageDetails details{};
        details.packageIdentifier = pkg_id.c_str();
        details.version.major = 1;
        details.version.minor = 0;
        details.version.build = 0;
        details.version.revision = 0;
        details.kind = XPackageKind_Game;
        details.displayName = disp_name.c_str();
        details.description = "Local OpenScarlett title";
        details.publisher = "OpenScarlett";
        details.storeId = cur_store_id.c_str();
        details.installing = false;
        details.index = 0;
        details.count = 1;
        details.ageRestricted = false;
        details.titleID = tid_buf;
        (void)callback(context, &details);
    }
    return RecordHr("XPackageEnumeratePackages", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageEnumerateFeatures(
    const char* packageIdentifier,
    void* context,
    XPackageFeatureEnumerationCallback callback) {
    if (!packageIdentifier || !*packageIdentifier || !callback) {
        return RecordHr("XPackageEnumerateFeatures", XGR_E_INVALIDARG);
    }
    XPackageFeature feat{};
    feat.id = "BaseGame";
    feat.displayName = "Base Game";
    feat.tags = "default";
    feat.hidden = false;
    feat.storeIdCount = 0;
    feat.storeIds = nullptr;
    (void)callback(context, &feat);
    return RecordHr("XPackageEnumerateFeatures", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageFindChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability) {
    if (!packageIdentifier || !*packageIdentifier || !availability) {
        return RecordHr("XPackageFindChunkAvailability", XGR_E_INVALIDARG);
    }
    XPackageChunkAvailability computed = XPackageChunkAvailability_Ready;
    const XGameResult hr = ValidateAndEvaluateSelectors(selectorCount, selectors, computed);
    if (hr < 0) {
        return RecordHr("XPackageFindChunkAvailability", hr);
    }
    *availability = computed;
    return RecordHr("XPackageFindChunkAvailability", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageEstimateDownloadSize(
    const char* packageIdentifier,
    uint32_t selectorCount,
    const XPackageChunkSelector* selectors,
    uint64_t* downloadSize,
    bool* shouldPresentUserConfirmation) {
    if (!packageIdentifier || !*packageIdentifier || !downloadSize) {
        return RecordHr("XPackageEstimateDownloadSize", XGR_E_INVALIDARG);
    }
    XPackageChunkAvailability avail = XPackageChunkAvailability_Ready;
    const XGameResult hr = ValidateAndEvaluateSelectors(selectorCount, selectors, avail);
    if (hr < 0) {
        return RecordHr("XPackageEstimateDownloadSize", hr);
    }
    *downloadSize = 0ULL;
    if (shouldPresentUserConfirmation) {
        *shouldPresentUserConfirmation = false;
    }
    return RecordHr("XPackageEstimateDownloadSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageChangeChunkInstallOrder(
    const char* packageIdentifier,
    uint32_t /*chunkCount*/,
    const uint32_t* /*chunkIds*/) {
    if (!packageIdentifier || !*packageIdentifier) {
        return RecordHr("XPackageChangeChunkInstallOrder", XGR_E_INVALIDARG);
    }
    return RecordHr("XPackageChangeChunkInstallOrder", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunks(
    const char* packageIdentifier,
    uint32_t /*selectorCount*/,
    const XPackageChunkSelector* /*selectors*/,
    uint32_t /*minimumUpdateIntervalMs*/,
    bool /*suppressUserConfirmation*/,
    XTaskQueueHandle /*queue*/,
    XPackageInstallationMonitorHandle* installationMonitor) {
    if (!packageIdentifier || !*packageIdentifier || !installationMonitor) {
        return RecordHr("XPackageInstallChunks", XGR_E_INVALIDARG);
    }
    *installationMonitor = CreateInstallMonitorHandle(packageIdentifier);
    return RecordHr("XPackageInstallChunks", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunksAsync(
    const char* packageIdentifier,
    uint32_t /*selectorCount*/,
    const XPackageChunkSelector* /*selectors*/,
    uint32_t /*minimumUpdateIntervalMs*/,
    bool /*suppressUserConfirmation*/,
    XAsyncBlock* async) {
    if (!packageIdentifier || !*packageIdentifier || !async) {
        return RecordHr("XPackageInstallChunksAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) PackageInstallChunksAsyncCtx{packageIdentifier, nullptr};
    if (!ctx) {
        return RecordHr("XPackageInstallChunksAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdPackageInstallChunks, kIdPackageInstallChunks,
        &PackageInstallChunksProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XPackageInstallChunksAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XPackageInstallChunksAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageInstallChunksResult(
    XAsyncBlock* async,
    XPackageInstallationMonitorHandle* installationMonitor) {
    if (!async || !installationMonitor) {
        return RecordHr("XPackageInstallChunksResult", XGR_E_INVALIDARG);
    }
    *installationMonitor = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XPackageInstallChunksResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdPackageInstallChunks, sizeof(XPackageInstallationMonitorHandle),
        installationMonitor, nullptr);
    return RecordHr("XPackageInstallChunksResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageUninstallChunks(
    const char* packageIdentifier,
    uint32_t /*selectorCount*/,
    const XPackageChunkSelector* /*selectors*/) {
    if (!packageIdentifier || !*packageIdentifier) {
        return RecordHr("XPackageUninstallChunks", XGR_E_INVALIDARG);
    }
    return RecordHr("XPackageUninstallChunks", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPackageUninstallUWPInstance(const char* packageName) {
    if (!packageName || !*packageName) {
        return RecordHr("XPackageUninstallUWPInstance", XGR_E_INVALIDARG);
    }
    return RecordHr("XPackageUninstallUWPInstance", XGR_S_OK);
}

// ===========================================================================
// XDisplay
// ===========================================================================
}  // extern "C"

namespace {

bool ParseEnvBoolToken(const std::string& raw, bool& out_val) {
    if (raw.empty()) return false;
    std::string lo = raw;
    for (char& c : lo) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lo == "1" || lo == "true" || lo == "yes" || lo == "on" || lo == "hdr") {
        out_val = true;
        return true;
    }
    if (lo == "0" || lo == "false" || lo == "no" || lo == "off" || lo == "sdr") {
        out_val = false;
        return true;
    }
    return false;
}

bool ProbeHostDxgiHdrDisplay(XDisplayHdrModeInfo& out_info) {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        !factory) {
        return false;
    }
    bool hdr_enabled = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) == S_OK && !hdr_enabled; ++ai) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) == S_OK && !hdr_enabled; ++oi) {
            IDXGIOutput6* out6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6),
                                                 reinterpret_cast<void**>(&out6))) &&
                out6) {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(out6->GetDesc1(&desc1)) &&
                    desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    hdr_enabled = true;
                    out_info.minToneMapLuminance =
                        (desc1.MinLuminance > 0.0f) ? desc1.MinLuminance : 0.005f;
                    out_info.maxToneMapLuminance =
                        (desc1.MaxLuminance > 0.0f) ? desc1.MaxLuminance : 1000.0f;
                    out_info.maxFullFrameToneMapLuminance =
                        (desc1.MaxFullFrameLuminance > 0.0f) ? desc1.MaxFullFrameLuminance : 400.0f;
                }
                out6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return hdr_enabled;
#else
    (void)out_info;
    return false;
#endif
}

// ===========================================================================
// XGameUI Async Providers
// ===========================================================================
const char kIdGameUiMessageDialog[] = "XGameUIShowMessageDialogAsync";
const char kIdGameUiTextEntry[] = "XGameUIShowTextEntryAsync";
const char kIdGameUiPlayerPicker[] = "XGameUIShowPlayerPickerAsync";
const char kIdGameUiErrorDialog[] = "XGameUIShowErrorDialogAsync";

struct GameUiMessageDialogCtx {
    XGameUIMessageDialogButton result_button = XGameUIMessageDialogButton_First;
};

XGameResult OPENSCARLETT_GDK_CALL GameUiMessageDialogProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameUiMessageDialogCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, sizeof(XGameUIMessageDialogButton));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult:
            if (!data->buffer || data->bufferSize < sizeof(XGameUIMessageDialogButton)) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, &ctx->result_button, sizeof(XGameUIMessageDialogButton));
            return XGR_S_OK;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameUiTextEntryCtx {
    std::string text;
};

XGameResult OPENSCARLETT_GDK_CALL GameUiTextEntryProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameUiTextEntryCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, ctx->text.size() + 1u);
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            const size_t needed = ctx->text.size() + 1u;
            if (!data->buffer || data->bufferSize < needed) {
                return XGR_E_NOT_SUFFICIENT_BUFFER;
            }
            std::memcpy(data->buffer, ctx->text.c_str(), needed);
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameUiPlayerPickerCtx {
    std::vector<uint64_t> selected_players;
};

XGameResult OPENSCARLETT_GDK_CALL GameUiPlayerPickerProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameUiPlayerPickerCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, ctx->selected_players.size() * sizeof(uint64_t));
            return XGR_E_PENDING;
        case XAsyncOp_GetResult: {
            const size_t needed = ctx->selected_players.size() * sizeof(uint64_t);
            if (needed > 0u) {
                if (!data->buffer || data->bufferSize < needed) {
                    return XGR_E_NOT_SUFFICIENT_BUFFER;
                }
                std::memcpy(data->buffer, ctx->selected_players.data(), needed);
            }
            return XGR_S_OK;
        }
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

struct GameUiErrorDialogCtx {
    XGameResult error_code = XGR_S_OK;
};

XGameResult OPENSCARLETT_GDK_CALL GameUiErrorDialogProvider(
    XAsyncOp op,
    const XAsyncProviderData* data) {
    auto* ctx = static_cast<GameUiErrorDialogCtx*>(data->context);
    switch (op) {
        case XAsyncOp_Begin:
            return XGR_S_OK;
        case XAsyncOp_DoWork:
            XAsyncComplete(data->async, XGR_S_OK, 0);
            return XGR_E_PENDING;
        case XAsyncOp_Cleanup:
            delete ctx;
            return XGR_S_OK;
        default:
            return XGR_S_OK;
    }
}

// ===========================================================================
// XGameInvite Dispatch Trampoline
// ===========================================================================
struct InviteDispatchCtx {
    XGameInviteEventCallback callback = nullptr;
    void* user_context = nullptr;
    std::string uri;
};

void OPENSCARLETT_GDK_CALL InviteDispatchTrampoline(void* context, bool canceled) {
    std::unique_ptr<InviteDispatchCtx> holder(static_cast<InviteDispatchCtx*>(context));
    if (!canceled && holder && holder->callback) {
        holder->callback(holder->user_context, holder->uri.c_str());
    }
}

// ===========================================================================
// XSpeechSynthesizer Deterministic PCM WAV Synthesizer
// ===========================================================================
struct BuiltinVoice {
    const char* voice_id;
    const char* display_name;
    const char* description;
    const char* language;
    XSpeechSynthesizerVoiceGender gender;
};

static constexpr BuiltinVoice kBuiltinVoices[] = {
    {"openscarlett-en-us-aria", "OpenScarlett Aria",
     "OpenScarlett Aria - English (United States)", "en-US",
     XSpeechSynthesizerVoiceGender_Female},
    {"openscarlett-en-us-guy", "OpenScarlett Guy",
     "OpenScarlett Guy - English (United States)", "en-US",
     XSpeechSynthesizerVoiceGender_Male},
};

void AppendLe16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void AppendLe32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

std::vector<uint8_t> SynthesizeDeterministicWav(const char* text, bool is_male) {
    constexpr uint32_t kSampleRate = 22050u;
    constexpr uint16_t kChannels = 1u;
    constexpr uint16_t kBitsPerSample = 16u;
    constexpr uint32_t kSamplesPerChar = 441u;  // 20 ms per character
    constexpr size_t kMaxChars = 200u;          // cap at 4 seconds
    constexpr double kTwoPi = 6.28318530717958647692;

    const std::string s = text ? std::string(text) : std::string{};
    const size_t char_count = std::min(s.size(), kMaxChars);
    const uint32_t total_samples =
        (char_count > 0u) ? static_cast<uint32_t>(char_count * kSamplesPerChar) : kSamplesPerChar;
    const uint32_t data_bytes = total_samples * sizeof(int16_t);

    std::vector<uint8_t> wav;
    wav.reserve(44u + data_bytes);

    // RIFF header
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    AppendLe32(wav, 36u + data_bytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});

    // fmt chunk
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    AppendLe32(wav, 16u);                                           // PCM chunk size
    AppendLe16(wav, 1u);                                            // WAVE_FORMAT_PCM
    AppendLe16(wav, kChannels);
    AppendLe32(wav, kSampleRate);
    AppendLe32(wav, kSampleRate * kChannels * (kBitsPerSample / 8u));
    AppendLe16(wav, static_cast<uint16_t>(kChannels * (kBitsPerSample / 8u)));
    AppendLe16(wav, kBitsPerSample);

    // data chunk
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    AppendLe32(wav, data_bytes);

    const double base_hz = is_male ? 135.0 : 235.0;
    if (char_count == 0u) {
        wav.resize(44u + data_bytes, 0u);
        return wav;
    }

    for (size_t ci = 0; ci < char_count; ++ci) {
        const unsigned char ch = static_cast<unsigned char>(s[ci]);
        const bool is_pause = (std::isspace(ch) != 0);
        const double freq1 = base_hz + static_cast<double>(ch % 37u) * 8.5;
        const double freq2 = freq1 * 2.15;
        for (uint32_t si = 0; si < kSamplesPerChar; ++si) {
            int16_t pcm = 0;
            if (!is_pause) {
                const double t = static_cast<double>(si) / static_cast<double>(kSampleRate);
                const double env =
                    std::sin(3.14159265358979323846 * static_cast<double>(si) /
                             static_cast<double>(kSamplesPerChar));
                const double sig =
                    0.65 * std::sin(kTwoPi * freq1 * t) + 0.35 * std::sin(kTwoPi * freq2 * t);
                const double val = std::clamp(sig * env * 0.45, -1.0, 1.0);
                pcm = static_cast<int16_t>(std::lround(val * 32767.0));
            }
            AppendLe16(wav, static_cast<uint16_t>(pcm));
        }
    }
    return wav;
}

}  // namespace

extern "C" {

XDisplayHdrModeResult OPENSCARLETT_GDK_CALL XDisplayTryEnableHdrMode(
    XDisplayHdrModePreference preferredMode,
    XDisplayHdrModeInfo* displayHdrModeInfo) {
    (void)preferredMode;
    RecordOk("XDisplayTryEnableHdrMode");

    bool force_hdr = false;
    const std::string env_hdr = GetEnvString("OPENSCARLETT_FORCE_HDR");
    if (ParseEnvBoolToken(env_hdr, force_hdr)) {
        if (force_hdr) {
            if (displayHdrModeInfo) {
                displayHdrModeInfo->minToneMapLuminance = 0.005f;
                displayHdrModeInfo->maxToneMapLuminance = 1000.0f;
                displayHdrModeInfo->maxFullFrameToneMapLuminance = 400.0f;
            }
            return XDisplayHdrModeResult_Enabled;
        }
        if (displayHdrModeInfo) {
            displayHdrModeInfo->minToneMapLuminance = 0.05f;
            displayHdrModeInfo->maxToneMapLuminance = 300.0f;
            displayHdrModeInfo->maxFullFrameToneMapLuminance = 250.0f;
        }
        return XDisplayHdrModeResult_Disabled;
    }

    XDisplayHdrModeInfo probed{};
    if (ProbeHostDxgiHdrDisplay(probed)) {
        if (displayHdrModeInfo) {
            *displayHdrModeInfo = probed;
        }
        return XDisplayHdrModeResult_Enabled;
    }

    if (displayHdrModeInfo) {
        displayHdrModeInfo->minToneMapLuminance = 0.05f;
        displayHdrModeInfo->maxToneMapLuminance = 300.0f;
        displayHdrModeInfo->maxFullFrameToneMapLuminance = 250.0f;
    }
    return XDisplayHdrModeResult_Disabled;
}

XGameResult OPENSCARLETT_GDK_CALL XDisplayAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle) {
    if (!handle) {
        return RecordHr("XDisplayAcquireTimeoutDeferral", XGR_E_INVALIDARG);
    }
    auto* d = new (std::nothrow) XDisplayTimeoutDeferral();
    if (!d) {
        *handle = nullptr;
        return RecordHr("XDisplayAcquireTimeoutDeferral", XGR_E_OUTOFMEMORY);
    }
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.display_mutex);
        rt.live_deferrals.insert(d);
    }
    *handle = d;
    return RecordHr("XDisplayAcquireTimeoutDeferral", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XDisplayTryAcquireTimeoutDeferral(
    XDisplayTimeoutDeferralHandle* handle) {
    const XGameResult hr = XDisplayAcquireTimeoutDeferral(handle);
    return RecordHr("XDisplayTryAcquireTimeoutDeferral", hr);
}

void OPENSCARLETT_GDK_CALL XDisplayCloseTimeoutDeferralHandle(
    XDisplayTimeoutDeferralHandle handle) {
    RecordOk("XDisplayCloseTimeoutDeferralHandle");
    if (!handle) {
        return;
    }
    bool found = false;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.display_mutex);
        auto it = rt.live_deferrals.find(handle);
        if (it != rt.live_deferrals.end()) {
            found = true;
            if (handle->ref_count.fetch_sub(1u, std::memory_order_acq_rel) <= 1u) {
                rt.live_deferrals.erase(it);
            } else {
                found = false;
            }
        }
    }
    if (found) {
        delete handle;
    }
}

// ===========================================================================
// XLauncher & XGameUI
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XLaunchUri(
    XUserHandle /*requestingUser*/,
    const char* uri) {
    if (!uri || *uri == '\0') {
        return RecordHr("XLaunchUri", XGR_E_INVALIDARG);
    }
    const std::string uri_str(uri);
    const size_t colon = uri_str.find(':');
    if (colon == std::string::npos || colon == 0u) {
        return RecordHr("XLaunchUri", XGR_E_INVALIDARG);
    }
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.ui_mutex);
        rt.last_launched_uri = uri_str;
    }
    return RecordHr("XLaunchUri", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowMessageDialogAsync(
    XAsyncBlock* async,
    const char* /*titleText*/,
    const char* /*contentText*/,
    const char* firstButtonText,
    const char* secondButtonText,
    const char* thirdButtonText,
    XGameUIMessageDialogButton defaultButton,
    XGameUIMessageDialogButton cancelButton) {
    if (!async ||
        !ValidateMessageDialogButtons(
            static_cast<uint32_t>(defaultButton),
            static_cast<uint32_t>(cancelButton),
            firstButtonText,
            secondButtonText,
            thirdButtonText)) {
        return RecordHr("XGameUIShowMessageDialogAsync", XGR_E_INVALIDARG);
    }
    XGameUIMessageDialogButton chosen = defaultButton;
    const std::string env_btn = GetEnvString("OPENSCARLETT_UI_DIALOG_BUTTON");
    if (!env_btn.empty()) {
        XGameUIMessageDialogButton cand = chosen;
        bool matched = false;
        if (env_btn == "0" || env_btn == "first") {
            cand = XGameUIMessageDialogButton_First;
            matched = true;
        } else if (env_btn == "1" || env_btn == "second") {
            cand = XGameUIMessageDialogButton_Second;
            matched = true;
        } else if (env_btn == "2" || env_btn == "third") {
            cand = XGameUIMessageDialogButton_Third;
            matched = true;
        }
        if (matched &&
            IsDialogButtonIndexValid(
                static_cast<uint32_t>(cand),
                firstButtonText,
                secondButtonText,
                thirdButtonText)) {
            chosen = cand;
        }
    }
    auto* ctx = new (std::nothrow) GameUiMessageDialogCtx{chosen};
    if (!ctx) {
        return RecordHr("XGameUIShowMessageDialogAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameUiMessageDialog, kIdGameUiMessageDialog, &GameUiMessageDialogProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUIShowMessageDialogAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUIShowMessageDialogAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowMessageDialogResult(
    XAsyncBlock* async,
    XGameUIMessageDialogButton* resultButton) {
    if (!async || !resultButton) {
        return RecordHr("XGameUIShowMessageDialogResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUIShowMessageDialogResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdGameUiMessageDialog, sizeof(XGameUIMessageDialogButton), resultButton, nullptr);
    return RecordHr("XGameUIShowMessageDialogResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryAsync(
    XAsyncBlock* async,
    const char* /*titleText*/,
    const char* /*descriptionText*/,
    const char* defaultText,
    XGameUITextEntryInputScope /*inputScope*/,
    uint32_t maxTextLength) {
    if (!async) {
        return RecordHr("XGameUIShowTextEntryAsync", XGR_E_INVALIDARG);
    }
    std::string text = GetEnvString("OPENSCARLETT_UI_TEXT_ENTRY");
    if (text.empty() && defaultText) {
        text = defaultText;
    }
    if (maxTextLength > 0u && text.size() > maxTextLength) {
        text.resize(maxTextLength);
    }
    auto* ctx = new (std::nothrow) GameUiTextEntryCtx{std::move(text)};
    if (!ctx) {
        return RecordHr("XGameUIShowTextEntryAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameUiTextEntry, kIdGameUiTextEntry, &GameUiTextEntryProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUIShowTextEntryAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUIShowTextEntryAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryResultSize(
    XAsyncBlock* async,
    uint32_t* resultTextBufferSize) {
    if (!async || !resultTextBufferSize) {
        return RecordHr("XGameUIShowTextEntryResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUIShowTextEntryResultSize", hr);
    }
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr >= 0) {
        *resultTextBufferSize = static_cast<uint32_t>(sz);
    }
    return RecordHr("XGameUIShowTextEntryResultSize", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowTextEntryResult(
    XAsyncBlock* async,
    uint32_t resultTextBufferSize,
    char* resultTextBuffer,
    uint32_t* resultTextBufferUsed) {
    if (!async || !resultTextBuffer) {
        return RecordHr("XGameUIShowTextEntryResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUIShowTextEntryResult", hr);
    }
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdGameUiTextEntry, resultTextBufferSize, resultTextBuffer, &used);
    if (resultTextBufferUsed) {
        *resultTextBufferUsed = static_cast<uint32_t>(used);
    }
    return RecordHr("XGameUIShowTextEntryResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    const char* /*promptText*/,
    uint32_t selectFromPlayersCount,
    const uint64_t* selectFromPlayers,
    uint32_t preSelectedPlayersCount,
    const uint64_t* preSelectedPlayers,
    uint32_t minSelectionCount,
    uint32_t maxSelectionCount) {
    if (!async || minSelectionCount > maxSelectionCount) {
        return RecordHr("XGameUIShowPlayerPickerAsync", XGR_E_INVALIDARG);
    }
    if ((selectFromPlayersCount > 0u && !selectFromPlayers) ||
        (preSelectedPlayersCount > 0u && !preSelectedPlayers)) {
        return RecordHr("XGameUIShowPlayerPickerAsync", XGR_E_INVALIDARG);
    }

    auto* ctx = new (std::nothrow) GameUiPlayerPickerCtx();
    if (!ctx) {
        return RecordHr("XGameUIShowPlayerPickerAsync", XGR_E_OUTOFMEMORY);
    }

    auto contains = [&](uint64_t id) {
        return std::find(ctx->selected_players.begin(), ctx->selected_players.end(), id) !=
               ctx->selected_players.end();
    };
    for (uint32_t i = 0; i < preSelectedPlayersCount && ctx->selected_players.size() < maxSelectionCount; ++i) {
        if (!contains(preSelectedPlayers[i])) {
            ctx->selected_players.push_back(preSelectedPlayers[i]);
        }
    }
    const uint32_t target_min =
        (minSelectionCount > 0u)
            ? minSelectionCount
            : ((ctx->selected_players.empty() && selectFromPlayersCount > 0u && maxSelectionCount > 0u)
                   ? 1u
                   : 0u);
    for (uint32_t i = 0; i < selectFromPlayersCount && ctx->selected_players.size() < target_min &&
                         ctx->selected_players.size() < maxSelectionCount;
         ++i) {
        if (!contains(selectFromPlayers[i])) {
            ctx->selected_players.push_back(selectFromPlayers[i]);
        }
    }

    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameUiPlayerPicker, kIdGameUiPlayerPicker, &GameUiPlayerPickerProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUIShowPlayerPickerAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUIShowPlayerPickerAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerResultCount(
    XAsyncBlock* async,
    uint32_t* resultPlayersCount) {
    if (!async || !resultPlayersCount) {
        return RecordHr("XGameUIShowPlayerPickerResultCount", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUIShowPlayerPickerResultCount", hr);
    }
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr >= 0) {
        *resultPlayersCount = static_cast<uint32_t>(sz / sizeof(uint64_t));
    }
    return RecordHr("XGameUIShowPlayerPickerResultCount", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowPlayerPickerResult(
    XAsyncBlock* async,
    uint32_t resultPlayersCount,
    uint64_t* resultPlayers,
    uint32_t* resultPlayersUsed) {
    if (!async || (resultPlayersCount > 0u && !resultPlayers)) {
        return RecordHr("XGameUIShowPlayerPickerResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUIShowPlayerPickerResult", hr);
    }
    size_t used_bytes = 0;
    hr = XAsyncGetResult(
        async,
        kIdGameUiPlayerPicker,
        static_cast<size_t>(resultPlayersCount) * sizeof(uint64_t),
        resultPlayers,
        &used_bytes);
    if (resultPlayersUsed) {
        *resultPlayersUsed = static_cast<uint32_t>(used_bytes / sizeof(uint64_t));
    }
    return RecordHr("XGameUIShowPlayerPickerResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowErrorDialogAsync(
    XAsyncBlock* async,
    XGameResult errorCode,
    const char* /*context*/) {
    if (!async) {
        return RecordHr("XGameUIShowErrorDialogAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) GameUiErrorDialogCtx{errorCode};
    if (!ctx) {
        return RecordHr("XGameUIShowErrorDialogAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdGameUiErrorDialog, kIdGameUiErrorDialog, &GameUiErrorDialogProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUIShowErrorDialogAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUIShowErrorDialogAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUIShowErrorDialogResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameUIShowErrorDialogResult", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    return RecordHr("XGameUIShowErrorDialogResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUISetNotificationPositionHint(
    XGameUINotificationPositionHint position) {
    if (static_cast<uint32_t>(position) >
        static_cast<uint32_t>(XGameUINotificationPositionHint_TopRight)) {
        return RecordHr("XGameUISetNotificationPositionHint", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.ui_mutex);
    rt.notification_pos = position;
    return RecordHr("XGameUISetNotificationPositionHint", XGR_S_OK);
}

// ===========================================================================
// XGameInvite
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameInviteRegisterForEvent(
    XTaskQueueHandle queue,
    void* context,
    XGameInviteEventCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr("XGameInviteRegisterForEvent", XGR_E_INVALIDARG);
    }
    XTaskQueueHandle dup_q = nullptr;
    if (queue) {
        const XGameResult qhr = XTaskQueueDuplicateHandle(queue, &dup_q);
        if (qhr < 0) {
            return RecordHr("XGameInviteRegisterForEvent", qhr);
        }
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.invite_mutex);
    const uint64_t tok = rt.next_invite_token++;
    rt.invite_listeners.push_back(InviteListenerEntry{tok, dup_q, context, callback});
    token->token = tok;
    return RecordHr("XGameInviteRegisterForEvent", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XGameInviteUnregisterForEvent(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XGameInviteUnregisterForEvent");
    XTaskQueueHandle to_close = nullptr;
    bool removed = false;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.invite_mutex);
        for (auto it = rt.invite_listeners.begin(); it != rt.invite_listeners.end(); ++it) {
            if (it->token == token.token) {
                to_close = it->queue;
                rt.invite_listeners.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (to_close) {
        XTaskQueueCloseHandle(to_close);
    }
    return removed;
}

XGameResult OPENSCARLETT_GDK_CALL XGameInviteDispatchTestEventForTest(const char* inviteUri) {
    if (!inviteUri || *inviteUri == '\0') {
        return RecordHr("XGameInviteDispatchTestEventForTest", XGR_E_INVALIDARG);
    }
    std::vector<InviteListenerEntry> snapshot;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.invite_mutex);
        snapshot = rt.invite_listeners;
        for (auto& e : snapshot) {
            if (e.queue) {
                XTaskQueueHandle dup = nullptr;
                if (XTaskQueueDuplicateHandle(e.queue, &dup) == XGR_S_OK) {
                    e.queue = dup;
                } else {
                    e.queue = nullptr;
                }
            }
        }
    }

    for (const auto& entry : snapshot) {
        if (!entry.callback) {
            if (entry.queue) XTaskQueueCloseHandle(entry.queue);
            continue;
        }
        if (entry.queue) {
            auto* dctx = new (std::nothrow) InviteDispatchCtx{entry.callback, entry.context, inviteUri};
            if (dctx) {
                const XGameResult shr = XTaskQueueSubmitCallback(
                    entry.queue, XTaskQueuePort_Completion, dctx, &InviteDispatchTrampoline);
                if (shr < 0) {
                    delete dctx;
                }
            }
            XTaskQueueCloseHandle(entry.queue);
        } else {
            entry.callback(entry.context, inviteUri);
        }
    }
    return RecordHr("XGameInviteDispatchTestEventForTest", XGR_S_OK);
}

// ===========================================================================
// XAccessibility (Closed Captions & High Contrast)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XClosedCaptionGetProperties(
    XClosedCaptionProperties* properties) {
    if (!properties) {
        return RecordHr("XClosedCaptionGetProperties", XGR_E_INVALIDARG);
    }
    bool enabled = false;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.accessibility_mutex);
        enabled = rt.closed_captions_enabled;
    }
    std::memset(properties, 0, sizeof(*properties));
    properties->BackgroundColor.A = 0xC0u;
    properties->BackgroundColor.R = 0x00u;
    properties->BackgroundColor.G = 0x00u;
    properties->BackgroundColor.B = 0x00u;
    properties->FontColor.A = 0xFFu;
    properties->FontColor.R = 0xFFu;
    properties->FontColor.G = 0xFFu;
    properties->FontColor.B = 0xFFu;
    properties->WindowColor.A = 0x00u;
    properties->FontEdgeAttribute = XClosedCaptionFontEdgeAttribute_DropShadowedEdges;
    properties->FontStyle = XClosedCaptionFontStyle_ProportionalWithoutSerifs;
    properties->FontScale = 1.0f;
    properties->Enabled = enabled;
    return RecordHr("XClosedCaptionGetProperties", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XClosedCaptionSetEnabled(bool enabled) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.accessibility_mutex);
    rt.closed_captions_enabled = enabled;
    return RecordHr("XClosedCaptionSetEnabled", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XHighContrastGetMode(XHighContrastMode* mode) {
    if (!mode) {
        return RecordHr("XHighContrastGetMode", XGR_E_INVALIDARG);
    }
    const std::string env_hc = GetEnvString("OPENSCARLETT_HIGH_CONTRAST");
    if (!env_hc.empty()) {
        std::string lo = env_hc;
        for (char& c : lo) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lo == "dark" || lo == "1") {
            *mode = XHighContrastMode_Dark;
            return RecordHr("XHighContrastGetMode", XGR_S_OK);
        }
        if (lo == "light" || lo == "2") {
            *mode = XHighContrastMode_Light;
            return RecordHr("XHighContrastGetMode", XGR_S_OK);
        }
        if (lo == "other" || lo == "3") {
            *mode = XHighContrastMode_Other;
            return RecordHr("XHighContrastGetMode", XGR_S_OK);
        }
        *mode = XHighContrastMode_Off;
        return RecordHr("XHighContrastGetMode", XGR_S_OK);
    }

    *mode = XHighContrastMode_Off;
#if defined(_WIN32)
    // Probe live OS High Contrast state dynamically without hard-linking user32.dll.
    HMODULE h_user32 = ::GetModuleHandleW(L"user32.dll");
    if (h_user32) {
        using PfnSpiW = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT);
        auto fn_spi = reinterpret_cast<PfnSpiW>(::GetProcAddress(h_user32, "SystemParametersInfoW"));
        if (fn_spi) {
            HIGHCONTRASTW hc{};
            hc.cbSize = sizeof(hc);
            if (fn_spi(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                (hc.dwFlags & HCF_HIGHCONTRASTON) != 0u) {
                *mode = XHighContrastMode_Dark;
            }
        }
    }
#endif
    return RecordHr("XHighContrastGetMode", XGR_S_OK);
}

// ===========================================================================
// XSpeechSynthesizer
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreate(
    XSpeechSynthesizerHandle* speechSynthesizer) {
    if (!speechSynthesizer) {
        return RecordHr("XSpeechSynthesizerCreate", XGR_E_INVALIDARG);
    }
    auto sp = std::make_shared<XSpeechSynthesizer>();
    XSpeechSynthesizerHandle raw = sp.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        rt.live_synths[raw] = std::move(sp);
    }
    *speechSynthesizer = raw;
    return RecordHr("XSpeechSynthesizerCreate", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCloseHandle(
    XSpeechSynthesizerHandle speechSynthesizer) {
    if (!speechSynthesizer) {
        return RecordHr("XSpeechSynthesizerCloseHandle", XGR_S_OK);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.speech_mutex);
    const size_t erased = rt.live_synths.erase(speechSynthesizer);
    return RecordHr("XSpeechSynthesizerCloseHandle", erased > 0 ? XGR_S_OK : XGR_E_HANDLE);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerEnumerateInstalledVoices(
    void* context,
    XSpeechSynthesizerInstalledVoicesCallback callback) {
    if (!callback) {
        return RecordHr("XSpeechSynthesizerEnumerateInstalledVoices", XGR_E_INVALIDARG);
    }
    for (const auto& v : kBuiltinVoices) {
        XSpeechSynthesizerVoiceInformation info{};
        info.Description = v.description;
        info.DisplayName = v.display_name;
        info.Gender = v.gender;
        info.VoiceId = v.voice_id;
        info.Language = v.language;
        if (!callback(&info, context)) {
            break;
        }
    }
    return RecordHr("XSpeechSynthesizerEnumerateInstalledVoices", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerSetDefaultVoice(
    XSpeechSynthesizerHandle speechSynthesizer) {
    if (!speechSynthesizer) {
        return RecordHr("XSpeechSynthesizerSetDefaultVoice", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.speech_mutex);
    auto it = rt.live_synths.find(speechSynthesizer);
    if (it == rt.live_synths.end()) {
        return RecordHr("XSpeechSynthesizerSetDefaultVoice", XGR_E_HANDLE);
    }
    it->second->voice_id = kBuiltinVoices[0].voice_id;
    it->second->is_male = (kBuiltinVoices[0].gender == XSpeechSynthesizerVoiceGender_Male);
    return RecordHr("XSpeechSynthesizerSetDefaultVoice", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerSetCustomVoice(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* voiceId) {
    if (!speechSynthesizer || !voiceId || !*voiceId) {
        return RecordHr("XSpeechSynthesizerSetCustomVoice", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.speech_mutex);
    auto it = rt.live_synths.find(speechSynthesizer);
    if (it == rt.live_synths.end()) {
        return RecordHr("XSpeechSynthesizerSetCustomVoice", XGR_E_HANDLE);
    }
    for (const auto& v : kBuiltinVoices) {
        if (std::strcmp(v.voice_id, voiceId) == 0) {
            it->second->voice_id = v.voice_id;
            it->second->is_male = (v.gender == XSpeechSynthesizerVoiceGender_Male);
            return RecordHr("XSpeechSynthesizerSetCustomVoice", XGR_S_OK);
        }
    }
    return RecordHr("XSpeechSynthesizerSetCustomVoice", XGR_E_NOT_FOUND);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreateStreamFromText(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* text,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream) {
    if (!speechSynthesizer || !text || !speechSynthesisStream) {
        return RecordHr("XSpeechSynthesizerCreateStreamFromText", XGR_E_INVALIDARG);
    }
    *speechSynthesisStream = nullptr;
    bool is_male = false;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        auto it = rt.live_synths.find(speechSynthesizer);
        if (it == rt.live_synths.end()) {
            return RecordHr("XSpeechSynthesizerCreateStreamFromText", XGR_E_HANDLE);
        }
        is_male = it->second->is_male;
    }

    auto stream = std::make_shared<XSpeechSynthesizerStream>();
    stream->wav_bytes = SynthesizeDeterministicWav(text, is_male);
    XSpeechSynthesizerStreamHandle raw = stream.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        rt.live_streams[raw] = std::move(stream);
    }
    *speechSynthesisStream = raw;
    return RecordHr("XSpeechSynthesizerCreateStreamFromText", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerGetStreamDataSize(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t* bufferSize) {
    if (!speechSynthesisStream || !bufferSize) {
        return RecordHr("XSpeechSynthesizerGetStreamDataSize", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.speech_mutex);
    auto it = rt.live_streams.find(speechSynthesisStream);
    if (it == rt.live_streams.end()) {
        return RecordHr("XSpeechSynthesizerGetStreamDataSize", XGR_E_HANDLE);
    }
    *bufferSize = it->second->wav_bytes.size();
    return RecordHr("XSpeechSynthesizerGetStreamDataSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerGetStreamData(
    XSpeechSynthesizerStreamHandle speechSynthesisStream,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed) {
    if (!speechSynthesisStream || !buffer) {
        return RecordHr("XSpeechSynthesizerGetStreamData", XGR_E_INVALIDARG);
    }
    std::shared_ptr<XSpeechSynthesizerStream> stream;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        auto it = rt.live_streams.find(speechSynthesisStream);
        if (it == rt.live_streams.end()) {
            return RecordHr("XSpeechSynthesizerGetStreamData", XGR_E_HANDLE);
        }
        stream = it->second;
    }
    const size_t needed = stream->wav_bytes.size();
    if (bufferUsed) {
        *bufferUsed = needed;
    }
    if (bufferSize < needed) {
        return RecordHr("XSpeechSynthesizerGetStreamData", XGR_E_NOT_SUFFICIENT_BUFFER);
    }
    if (needed > 0u) {
        std::memcpy(buffer, stream->wav_bytes.data(), needed);
    }
    return RecordHr("XSpeechSynthesizerGetStreamData", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCreateStreamFromSsml(
    XSpeechSynthesizerHandle speechSynthesizer,
    const char* ssml,
    XSpeechSynthesizerStreamHandle* speechSynthesisStream) {
    if (!speechSynthesizer || !ssml || !speechSynthesisStream) {
        return RecordHr("XSpeechSynthesizerCreateStreamFromSsml", XGR_E_INVALIDARG);
    }
    *speechSynthesisStream = nullptr;
    bool is_male = false;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        auto it = rt.live_synths.find(speechSynthesizer);
        if (it == rt.live_synths.end()) {
            return RecordHr("XSpeechSynthesizerCreateStreamFromSsml", XGR_E_HANDLE);
        }
        is_male = it->second->is_male;
    }

    // Extract plain text from SSML markup cleanly.
    std::string plain;
    bool in_tag = false;
    for (const char* p = ssml; *p; ++p) {
        if (*p == '<') {
            in_tag = true;
        } else if (*p == '>') {
            in_tag = false;
        } else if (!in_tag) {
            plain.push_back(*p);
        }
    }
    plain = TrimAscii(plain);
    if (plain.empty()) {
        plain = " ";
    }

    auto stream = std::make_shared<XSpeechSynthesizerStream>();
    stream->wav_bytes = SynthesizeDeterministicWav(plain.c_str(), is_male);
    XSpeechSynthesizerStreamHandle raw = stream.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.speech_mutex);
        rt.live_streams[raw] = std::move(stream);
    }
    *speechSynthesisStream = raw;
    return RecordHr("XSpeechSynthesizerCreateStreamFromSsml", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechSynthesizerCloseStreamHandle(
    XSpeechSynthesizerStreamHandle speechSynthesisStream) {
    if (!speechSynthesisStream) {
        return RecordHr("XSpeechSynthesizerCloseStreamHandle", XGR_S_OK);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.speech_mutex);
    const size_t erased = rt.live_streams.erase(speechSynthesisStream);
    return RecordHr("XSpeechSynthesizerCloseStreamHandle", erased > 0 ? XGR_S_OK : XGR_E_HANDLE);
}

// ===========================================================================
// XSpeechToText (XAccessibility local defaults)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextSetPositionHint(
    XSpeechToTextPositionHint position) {
    if (static_cast<uint32_t>(position) >
        static_cast<uint32_t>(XSpeechToTextPositionHint_TopRight)) {
        return RecordHr("XSpeechToTextSetPositionHint", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.stt_mutex);
    rt.stt_position_hint = position;
    return RecordHr("XSpeechToTextSetPositionHint", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextSendString(
    const char* speakerName,
    const char* content,
    XSpeechToTextType /*type*/) {
    if (!speakerName || !content) {
        return RecordHr("XSpeechToTextSendString", XGR_E_INVALIDARG);
    }
    return RecordHr("XSpeechToTextSendString", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextBeginHypothesisString(
    const char* speakerName,
    const char* content,
    XSpeechToTextType /*type*/,
    uint32_t* hypothesisId) {
    if (!speakerName || !content || !hypothesisId) {
        return RecordHr("XSpeechToTextBeginHypothesisString", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.stt_mutex);
    const uint32_t id = rt.next_stt_hypothesis_id++;
    rt.active_stt_hypotheses[id] = std::string(speakerName) + ": " + content;
    *hypothesisId = id;
    return RecordHr("XSpeechToTextBeginHypothesisString", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextUpdateHypothesisString(
    uint32_t hypothesisId,
    const char* content) {
    if (hypothesisId == 0 || !content) {
        return RecordHr("XSpeechToTextUpdateHypothesisString", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.stt_mutex);
    auto it = rt.active_stt_hypotheses.find(hypothesisId);
    if (it == rt.active_stt_hypotheses.end()) {
        return RecordHr("XSpeechToTextUpdateHypothesisString", XGR_E_NOT_FOUND);
    }
    it->second = content;
    return RecordHr("XSpeechToTextUpdateHypothesisString", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextFinalizeHypothesisString(
    uint32_t hypothesisId,
    const char* content) {
    if (hypothesisId == 0 || !content) {
        return RecordHr("XSpeechToTextFinalizeHypothesisString", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.stt_mutex);
    auto it = rt.active_stt_hypotheses.find(hypothesisId);
    if (it == rt.active_stt_hypotheses.end()) {
        return RecordHr("XSpeechToTextFinalizeHypothesisString", XGR_E_NOT_FOUND);
    }
    rt.active_stt_hypotheses.erase(it);
    return RecordHr("XSpeechToTextFinalizeHypothesisString", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XSpeechToTextCancelHypothesisString(uint32_t hypothesisId) {
    if (hypothesisId == 0) {
        return RecordHr("XSpeechToTextCancelHypothesisString", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.stt_mutex);
    const size_t erased = rt.active_stt_hypotheses.erase(hypothesisId);
    return RecordHr(
        "XSpeechToTextCancelHypothesisString", erased > 0 ? XGR_S_OK : XGR_E_NOT_FOUND);
}

// ===========================================================================
// XAppBroadcast & XAppCapture (offline/local defaults)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastGetStatus(
    XUserHandle requestingUser,
    XAppBroadcastStatus* appBroadcastStatus) {
    if (!appBroadcastStatus) {
        return RecordHr("XAppBroadcastGetStatus", XGR_E_INVALIDARG);
    }
    if (requestingUser && !LookupUser(requestingUser)) {
        return RecordHr("XAppBroadcastGetStatus", XGR_E_HANDLE);
    }
    std::memset(appBroadcastStatus, 0, sizeof(*appBroadcastStatus));
    appBroadcastStatus->canStartBroadcast = false;
    appBroadcastStatus->isAnyAppBroadcasting = false;
    appBroadcastStatus->isDisabledBySystem = true;
    return RecordHr("XAppBroadcastGetStatus", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XAppBroadcastIsAppBroadcasting(void) {
    RecordOk("XAppBroadcastIsAppBroadcasting");
    return false;
}

XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastShowUI(XUserHandle requestingUser) {
    if (requestingUser && !LookupUser(requestingUser)) {
        return RecordHr("XAppBroadcastShowUI", XGR_E_HANDLE);
    }
    return RecordHr("XAppBroadcastShowUI", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppBroadcastRegisterIsAppBroadcastingChanged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XAppBroadcastMonitorCallback appBroadcastMonitorCallback,
    XTaskQueueRegistrationToken* token) {
    if (!appBroadcastMonitorCallback || !token) {
        return RecordHr("XAppBroadcastRegisterIsAppBroadcastingChanged", XGR_E_INVALIDARG);
    }
    *token = AllocateRegToken(kRegAppBroadcast);
    return RecordHr("XAppBroadcastRegisterIsAppBroadcastingChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XAppBroadcastUnregisterIsAppBroadcastingChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XAppBroadcastUnregisterIsAppBroadcastingChanged");
    return ReleaseRegToken(kRegAppBroadcast, token);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddStringEvent(
    const char* name,
    const char* value,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name || !value) {
        return RecordHr("XAppCaptureMetadataAddStringEvent", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_bytes_used += std::strlen(name) + std::strlen(value) + 16u;
    return RecordHr("XAppCaptureMetadataAddStringEvent", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddInt32Event(
    const char* name,
    int32_t /*value*/,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name) {
        return RecordHr("XAppCaptureMetadataAddInt32Event", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_bytes_used += std::strlen(name) + 20u;
    return RecordHr("XAppCaptureMetadataAddInt32Event", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataAddDoubleEvent(
    const char* name,
    double /*value*/,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name) {
        return RecordHr("XAppCaptureMetadataAddDoubleEvent", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_bytes_used += std::strlen(name) + 24u;
    return RecordHr("XAppCaptureMetadataAddDoubleEvent", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartStringState(
    const char* name,
    const char* value,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name || !value) {
        return RecordHr("XAppCaptureMetadataStartStringState", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_states[name] = value;
    rt.capture_metadata_bytes_used += std::strlen(name) + std::strlen(value) + 16u;
    return RecordHr("XAppCaptureMetadataStartStringState", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartInt32State(
    const char* name,
    int32_t value,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name) {
        return RecordHr("XAppCaptureMetadataStartInt32State", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_states[name] = std::to_string(value);
    rt.capture_metadata_bytes_used += std::strlen(name) + 20u;
    return RecordHr("XAppCaptureMetadataStartInt32State", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStartDoubleState(
    const char* name,
    double value,
    XAppCaptureMetadataPriority /*priority*/) {
    if (!name || !*name) {
        return RecordHr("XAppCaptureMetadataStartDoubleState", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_states[name] = std::to_string(value);
    rt.capture_metadata_bytes_used += std::strlen(name) + 24u;
    return RecordHr("XAppCaptureMetadataStartDoubleState", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStopState(const char* name) {
    if (!name || !*name) {
        return RecordHr("XAppCaptureMetadataStopState", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_states.erase(name);
    return RecordHr("XAppCaptureMetadataStopState", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataStopAllStates(void) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    rt.capture_metadata_states.clear();
    return RecordHr("XAppCaptureMetadataStopAllStates", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureMetadataRemainingStorageBytesAvailable(
    uint64_t* value) {
    if (!value) {
        return RecordHr("XAppCaptureMetadataRemainingStorageBytesAvailable", XGR_E_INVALIDARG);
    }
    constexpr uint64_t kMaxMetadataBytes = 64ULL * 1024ULL * 1024ULL;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    *value = (rt.capture_metadata_bytes_used < kMaxMetadataBytes)
                 ? (kMaxMetadataBytes - rt.capture_metadata_bytes_used)
                 : 0ULL;
    return RecordHr("XAppCaptureMetadataRemainingStorageBytesAvailable", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureRegisterMetadataPurged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XAppCaptureMetadataPurgedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr("XAppCaptureRegisterMetadataPurged", XGR_E_INVALIDARG);
    }
    *token = AllocateRegToken(kRegAppCaptureMetadataPurged);
    return RecordHr("XAppCaptureRegisterMetadataPurged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XAppCaptureUnRegisterMetadataPurged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XAppCaptureUnRegisterMetadataPurged");
    return ReleaseRegToken(kRegAppCaptureMetadataPurged, token);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureTakeDiagnosticScreenshot(
    bool /*gameDvrShouldBeShutDown*/,
    XAppCaptureScreenshotFormatFlag /*screenshotFormat*/,
    const char* filenamePrefix,
    XAppCaptureDiagnosticScreenshotResult* result) {
    if (!result) {
        return RecordHr("XAppCaptureTakeDiagnosticScreenshot", XGR_E_INVALIDARG);
    }
    std::memset(result, 0, sizeof(*result));
    const fs::path cap_dir = ResolveSaveRoot() / "captures";
    std::error_code ec;
    fs::create_directories(cap_dir, ec);

    uint64_t seq = 0;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.capture_mutex);
        seq = rt.next_screenshot_id++;
    }
    const std::string prefix = (filenamePrefix && *filenamePrefix) ? filenamePrefix : "diag";
    const fs::path out_path = cap_dir / (prefix + "_" + std::to_string(seq) + ".png");
    const std::vector<uint8_t> png = BuildDefaultGamerPicPng(16u);
    (void)WriteBinaryFileAtomic(out_path, png.data(), png.size());

    result->fileCount = 1u;
    const std::string path_str = out_path.string();
    std::snprintf(result->files[0].path, sizeof(result->files[0].path), "%s", path_str.c_str());
    result->files[0].fileSize = png.size();
    result->files[0].width = 16u;
    result->files[0].height = 16u;
    return RecordHr("XAppCaptureTakeDiagnosticScreenshot", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureRecordDiagnosticClip(
    int64_t startTime,
    uint32_t durationInMs,
    const char* filenamePrefix,
    XAppCaptureRecordClipResult* result) {
    if (!result) {
        return RecordHr("XAppCaptureRecordDiagnosticClip", XGR_E_INVALIDARG);
    }
    std::memset(result, 0, sizeof(*result));
    const fs::path cap_dir = ResolveSaveRoot() / "captures";
    std::error_code ec;
    fs::create_directories(cap_dir, ec);

    uint64_t seq = 0;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.capture_mutex);
        seq = rt.next_screenshot_id++;
    }
    const std::string prefix = (filenamePrefix && *filenamePrefix) ? filenamePrefix : "clip";
    const fs::path out_path = cap_dir / (prefix + "_" + std::to_string(seq) + ".mp4");
    static constexpr uint8_t kStubClipHeader[16] = {
        0x00, 0x00, 0x00, 0x10, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0x00, 0x00, 0x02, 0x00};
    (void)WriteBinaryFileAtomic(out_path, kStubClipHeader, sizeof(kStubClipHeader));

    const std::string path_str = out_path.string();
    std::snprintf(result->path, sizeof(result->path), "%s", path_str.c_str());
    result->fileSize = sizeof(kStubClipHeader);
    result->startTime = startTime;
    result->durationInMs = durationInMs;
    result->width = 1920u;
    result->height = 1080u;
    result->encoding = 0u;
    result->startTimePreciseOffsetHns = 0u;
    return RecordHr("XAppCaptureRecordDiagnosticClip", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureTakeScreenshot(
    XUserHandle requestingUser,
    XAppCaptureTakeScreenshotResult* result) {
    if (!result) {
        return RecordHr("XAppCaptureTakeScreenshot", XGR_E_INVALIDARG);
    }
    if (requestingUser && !LookupUser(requestingUser)) {
        return RecordHr("XAppCaptureTakeScreenshot", XGR_E_HANDLE);
    }
    std::memset(result, 0, sizeof(*result));
    std::string local_id;
    std::vector<uint8_t> png = BuildDefaultGamerPicPng(16u);
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.capture_mutex);
        const uint64_t seq = rt.next_screenshot_id++;
        local_id = "openscarlett-screenshot-" + std::to_string(seq);
        rt.capture_screenshots[local_id] = std::move(png);
    }
    std::snprintf(result->localId, sizeof(result->localId), "%s", local_id.c_str());
    result->availableScreenshotFormats = XAppCaptureScreenshotFormatFlag_SDR;
    return RecordHr("XAppCaptureTakeScreenshot", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureOpenScreenshotStream(
    const char* localId,
    XAppCaptureScreenshotFormatFlag /*screenshotFormat*/,
    XAppCaptureScreenshotStreamHandle* handle,
    uint64_t* totalBytes) {
    if (!localId || !*localId || !handle || !totalBytes) {
        return RecordHr("XAppCaptureOpenScreenshotStream", XGR_E_INVALIDARG);
    }
    *handle = nullptr;
    *totalBytes = 0u;

    auto stream = std::make_shared<XAppCaptureScreenshotStream>();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.capture_mutex);
        auto it = rt.capture_screenshots.find(localId);
        if (it != rt.capture_screenshots.end()) {
            stream->bytes = it->second;
        } else {
            stream->bytes = BuildDefaultGamerPicPng(16u);
        }
        XAppCaptureScreenshotStreamHandle raw = stream.get();
        *totalBytes = static_cast<uint64_t>(stream->bytes.size());
        rt.live_capture_streams[raw] = std::move(stream);
        *handle = raw;
    }
    return RecordHr("XAppCaptureOpenScreenshotStream", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureReadScreenshotStream(
    XAppCaptureScreenshotStreamHandle handle,
    uint64_t startPosition,
    uint32_t bytesToRead,
    uint8_t* buffer,
    uint32_t* bytesWritten) {
    if (!handle || !bytesWritten || (bytesToRead > 0u && !buffer)) {
        return RecordHr("XAppCaptureReadScreenshotStream", XGR_E_INVALIDARG);
    }
    *bytesWritten = 0u;
    std::shared_ptr<XAppCaptureScreenshotStream> stream;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.capture_mutex);
        auto it = rt.live_capture_streams.find(handle);
        if (it == rt.live_capture_streams.end()) {
            return RecordHr("XAppCaptureReadScreenshotStream", XGR_E_HANDLE);
        }
        stream = it->second;
    }
    if (startPosition >= stream->bytes.size()) {
        return RecordHr("XAppCaptureReadScreenshotStream", XGR_S_OK);
    }
    const uint64_t avail = static_cast<uint64_t>(stream->bytes.size()) - startPosition;
    const uint32_t to_copy = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(bytesToRead), avail));
    if (to_copy > 0u) {
        std::memcpy(buffer, stream->bytes.data() + startPosition, to_copy);
    }
    *bytesWritten = to_copy;
    return RecordHr("XAppCaptureReadScreenshotStream", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureCloseScreenshotStream(
    XAppCaptureScreenshotStreamHandle handle) {
    if (!handle) {
        return RecordHr("XAppCaptureCloseScreenshotStream", XGR_S_OK);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.capture_mutex);
    const size_t erased = rt.live_capture_streams.erase(handle);
    return RecordHr("XAppCaptureCloseScreenshotStream", erased > 0 ? XGR_S_OK : XGR_E_HANDLE);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureEnableRecord(void) {
    GetRuntime().capture_record_enabled.store(true, std::memory_order_release);
    return RecordHr("XAppCaptureEnableRecord", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XAppCaptureDisableRecord(void) {
    GetRuntime().capture_record_enabled.store(false, std::memory_order_release);
    return RecordHr("XAppCaptureDisableRecord", XGR_S_OK);
}

// ===========================================================================
// XStore (offline/owned-title defaults)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XStoreCreateContext(
    XUserHandle user,
    XStoreContextHandle* storeContextHandle) {
    if (!storeContextHandle) {
        return RecordHr("XStoreCreateContext", XGR_E_INVALIDARG);
    }
    *storeContextHandle = nullptr;
    uint64_t xuid = DefaultUserXuid();
    if (user) {
        auto u = LookupUser(user);
        if (!u) {
            return RecordHr("XStoreCreateContext", XGR_E_HANDLE);
        }
        xuid = u->xuid;
    }
    auto ctx = std::make_shared<XStoreContext>();
    ctx->xuid = xuid;
    XStoreContextHandle raw = ctx.get();
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.store_mutex);
        rt.live_store_contexts[raw] = std::move(ctx);
    }
    *storeContextHandle = raw;
    return RecordHr("XStoreCreateContext", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XStoreCloseContextHandle(XStoreContextHandle storeContextHandle) {
    RecordOk("XStoreCloseContextHandle");
    if (!storeContextHandle) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    rt.live_store_contexts.erase(storeContextHandle);
}

void OPENSCARLETT_GDK_CALL XStoreCloseLicenseHandle(XStoreLicenseHandle storeLicenseHandle) {
    RecordOk("XStoreCloseLicenseHandle");
    if (!storeLicenseHandle) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    rt.live_store_licenses.erase(storeLicenseHandle);
}

void OPENSCARLETT_GDK_CALL XStoreCloseProductsQueryHandle(
    XStoreProductQueryHandle productQueryHandle) {
    RecordOk("XStoreCloseProductsQueryHandle");
    if (!productQueryHandle) return;
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.store_mutex);
    rt.live_store_queries.erase(productQueryHandle);
}

bool OPENSCARLETT_GDK_CALL XStoreIsLicenseValid(XStoreLicenseHandle storeLicenseHandle) {
    RecordOk("XStoreIsLicenseValid");
    auto lic = LookupStoreLicense(storeLicenseHandle);
    return lic != nullptr && lic->valid;
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameLicenseAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryGameLicenseAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryGameLicenseAsync", XGR_E_HANDLE);
    }
    XStoreGameLicense lic{};
    const std::string sku_store_id = CurrentSkuStoreId();
    std::snprintf(lic.skuStoreId, sizeof(lic.skuStoreId), "%s", sku_store_id.c_str());
    lic.isActive = true;
    lic.isTrialOwnedByThisUser = false;
    lic.isDiscLicense = false;
    lic.isTrial = false;
    lic.trialTimeRemainingInSeconds = 0;
    lic.trialUniqueId[0] = '\0';
    lic.expirationDate = 0;
    return RecordHr(
        "XStoreQueryGameLicenseAsync",
        StartPodAsync<XStoreGameLicense>(async, kIdStoreGameLicense, kIdStoreGameLicense, lic));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameLicenseResult(
    XAsyncBlock* async,
    XStoreGameLicense* license) {
    if (!async || !license) {
        return RecordHr("XStoreQueryGameLicenseResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryGameLicenseResult", hr);
    }
    hr = XAsyncGetResult(async, kIdStoreGameLicense, sizeof(XStoreGameLicense), license, nullptr);
    return RecordHr("XStoreQueryGameLicenseResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreRegisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XStoreGameLicenseChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!storeContextHandle || !callback || !token) {
        return RecordHr("XStoreRegisterGameLicenseChanged", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreRegisterGameLicenseChanged", XGR_E_HANDLE);
    }
    *token = AllocateRegToken(kRegStoreGameLicense);
    return RecordHr("XStoreRegisterGameLicenseChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XStoreUnregisterGameLicenseChanged(
    XStoreContextHandle storeContextHandle,
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XStoreUnregisterGameLicenseChanged");
    if (!LookupStoreContext(storeContextHandle)) {
        return false;
    }
    return ReleaseRegToken(kRegStoreGameLicense, token);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreRegisterPackageLicenseLost(
    XStoreLicenseHandle licenseHandle,
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XStorePackageLicenseLostCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!licenseHandle || !callback || !token) {
        return RecordHr("XStoreRegisterPackageLicenseLost", XGR_E_INVALIDARG);
    }
    if (!LookupStoreLicense(licenseHandle)) {
        return RecordHr("XStoreRegisterPackageLicenseLost", XGR_E_HANDLE);
    }
    *token = AllocateRegToken(kRegStorePackageLicense);
    return RecordHr("XStoreRegisterPackageLicenseLost", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XStoreUnregisterPackageLicenseLost(
    XStoreLicenseHandle licenseHandle,
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XStoreUnregisterPackageLicenseLost");
    if (!LookupStoreLicense(licenseHandle)) {
        return false;
    }
    return ReleaseRegToken(kRegStorePackageLicense, token);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryAddOnLicensesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryAddOnLicensesAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreQueryAddOnLicensesAsync",
        StartVoidAsync(async, kIdStoreAddOnLicenses, kIdStoreAddOnLicenses, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesResultCount(
    XAsyncBlock* async,
    uint32_t* count) {
    if (!async || !count) {
        return RecordHr("XStoreQueryAddOnLicensesResultCount", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryAddOnLicensesResultCount", hr);
    }
    *count = static_cast<uint32_t>(ConfiguredAddOnStoreIds().size());
    return RecordHr("XStoreQueryAddOnLicensesResultCount", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAddOnLicensesResult(
    XAsyncBlock* async,
    uint32_t count,
    XStoreAddonLicense* addOnLicenses) {
    if (!async) {
        return RecordHr("XStoreQueryAddOnLicensesResult", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryAddOnLicensesResult", hr);
    }
    const auto addons = ConfiguredAddOnStoreIds();
    if (!addons.empty() && count > 0u && addOnLicenses) {
        const uint32_t n = std::min<uint32_t>(count, static_cast<uint32_t>(addons.size()));
        for (uint32_t i = 0; i < n; ++i) {
            std::memset(&addOnLicenses[i], 0, sizeof(XStoreAddonLicense));
            const std::string sku = addons[i] + "/0001";
            std::snprintf(
                addOnLicenses[i].skuStoreId, sizeof(addOnLicenses[i].skuStoreId), "%s", sku.c_str());
            std::snprintf(
                addOnLicenses[i].inAppOfferToken,
                sizeof(addOnLicenses[i].inAppOfferToken),
                "%s",
                addons[i].c_str());
            addOnLicenses[i].isActive = true;
            addOnLicenses[i].expirationDate = 0;
        }
    }
    return RecordHr("XStoreQueryAddOnLicensesResult", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForCurrentGameAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryProductForCurrentGameAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryProductForCurrentGameAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreQueryProductForCurrentGameAsync",
        StartStoreProductQueryAsync(async, kIdStoreProdCurrentGame, kIdStoreProdCurrentGame, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForCurrentGameResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreQueryProductForCurrentGameResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryProductForCurrentGameResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreProdCurrentGame, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreQueryProductForCurrentGameResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForPackageAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    const char* packageIdentifier,
    XAsyncBlock* async) {
    if (!storeContextHandle || !packageIdentifier || !*packageIdentifier || !async) {
        return RecordHr("XStoreQueryProductForPackageAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryProductForPackageAsync", XGR_E_HANDLE);
    }
    const bool include_game =
        ((static_cast<uint32_t>(productKinds) & static_cast<uint32_t>(XStoreProductKind_Game)) != 0) &&
        (CurrentPackageIdentifier() == packageIdentifier);
    return RecordHr(
        "XStoreQueryProductForPackageAsync",
        StartStoreProductQueryAsync(async, kIdStoreProdPackage, kIdStoreProdPackage, include_game));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductForPackageResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreQueryProductForPackageResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryProductForPackageResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreProdPackage, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreQueryProductForPackageResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAssociatedProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind /*productKinds*/,
    uint32_t /*maxItemsToRetrievePerPage*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryAssociatedProductsAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryAssociatedProductsAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreQueryAssociatedProductsAsync",
        StartStoreProductQueryAsync(async, kIdStoreAssocProds, kIdStoreAssocProds, false));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryAssociatedProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreQueryAssociatedProductsResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryAssociatedProductsResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreAssocProds, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreQueryAssociatedProductsResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryEntitledProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind productKinds,
    uint32_t /*maxItemsToRetrievePerPage*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryEntitledProductsAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryEntitledProductsAsync", XGR_E_HANDLE);
    }
    const bool include_game =
        (static_cast<uint32_t>(productKinds) & static_cast<uint32_t>(XStoreProductKind_Game)) != 0;
    return RecordHr(
        "XStoreQueryEntitledProductsAsync",
        StartStoreProductQueryAsync(
            async, kIdStoreEntitledProds, kIdStoreEntitledProds, include_game));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryEntitledProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreQueryEntitledProductsResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryEntitledProductsResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreEntitledProds, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreQueryEntitledProductsResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductsAsync(
    XStoreContextHandle storeContextHandle,
    XStoreProductKind /*productKinds*/,
    const char** storeIds,
    size_t storeIdsCount,
    const char** /*actionFilters*/,
    size_t /*actionFiltersCount*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryProductsAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryProductsAsync", XGR_E_HANDLE);
    }
    const std::string cur_store_id = CurrentStoreId();
    bool include_game = false;
    if (storeIds && storeIdsCount > 0) {
        for (size_t i = 0; i < storeIdsCount; ++i) {
            if (storeIds[i] &&
                (std::strcmp(storeIds[i], kDefaultStoreId) == 0 ||
                 cur_store_id == storeIds[i])) {
                include_game = true;
                break;
            }
        }
    }
    return RecordHr(
        "XStoreQueryProductsAsync",
        StartStoreProductQueryAsync(async, kIdStoreQueryProds, kIdStoreQueryProds, include_game));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryProductsResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreQueryProductsResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryProductsResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreQueryProds, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreQueryProductsResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreEnumerateProductsQuery(
    XStoreProductQueryHandle productQueryHandle,
    void* context,
    XStoreProductQueryCallback callback) {
    if (!productQueryHandle || !callback) {
        return RecordHr("XStoreEnumerateProductsQuery", XGR_E_INVALIDARG);
    }
    auto q = LookupStoreQuery(productQueryHandle);
    if (!q) {
        return RecordHr("XStoreEnumerateProductsQuery", XGR_E_HANDLE);
    }
    for (const auto& backing : q->products) {
        if (!callback(&backing.product, context)) {
            break;
        }
    }
    return RecordHr("XStoreEnumerateProductsQuery", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XStoreProductsQueryHasMorePages(
    XStoreProductQueryHandle productQueryHandle) {
    RecordOk("XStoreProductsQueryHasMorePages");
    (void)productQueryHandle;
    return false;
}

XGameResult OPENSCARLETT_GDK_CALL XStoreProductsQueryNextPageAsync(
    XStoreProductQueryHandle productQueryHandle,
    XAsyncBlock* async) {
    if (!productQueryHandle || !async) {
        return RecordHr("XStoreProductsQueryNextPageAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreQuery(productQueryHandle)) {
        return RecordHr("XStoreProductsQueryNextPageAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreProductsQueryNextPageAsync",
        StartStoreProductQueryAsync(async, kIdStoreNextPage, kIdStoreNextPage, false));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreProductsQueryNextPageResult(
    XAsyncBlock* async,
    XStoreProductQueryHandle* productQueryHandle) {
    if (!async || !productQueryHandle) {
        return RecordHr("XStoreProductsQueryNextPageResult", XGR_E_INVALIDARG);
    }
    *productQueryHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreProductsQueryNextPageResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreNextPage, sizeof(XStoreProductQueryHandle),
        productQueryHandle, nullptr);
    return RecordHr("XStoreProductsQueryNextPageResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForPackageAsync(
    XStoreContextHandle storeContextHandle,
    const char* packageIdentifier,
    XAsyncBlock* async) {
    if (!storeContextHandle || !IsValidOfflineIdentifier(packageIdentifier, 256u) ||
        !async) {
        return RecordHr("XStoreCanAcquireLicenseForPackageAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreCanAcquireLicenseForPackageAsync", XGR_E_HANDLE);
    }
    const std::string req_lo = ToLowerAsciiStr(TrimAscii(packageIdentifier));
    const std::string cur_pkg_lo = ToLowerAsciiStr(CurrentPackageIdentifier());
    const auto lsa_pkg = openscarlett::ConsoleLsaPolicy::Global().GetPackageIdentity();
    const std::string lsa_full_lo = ToLowerAsciiStr(lsa_pkg.package_full_name);
    const std::string lsa_pfn_lo = ToLowerAsciiStr(lsa_pkg.package_family_name);

    bool owned = (req_lo == cur_pkg_lo) ||
                 (!lsa_full_lo.empty() && req_lo == lsa_full_lo) ||
                 (!lsa_pfn_lo.empty() && req_lo == lsa_pfn_lo);
    if (!owned) {
        for (const auto& p : ConfiguredOwnedPackages()) {
            if (ToLowerAsciiStr(p) == req_lo) {
                owned = true;
                break;
            }
        }
    }

    XStoreCanAcquireLicenseResult res{};
    if (owned) {
        std::snprintf(res.licensableSku, sizeof(res.licensableSku), "%s", "0001");
        res.status = XStoreCanLicenseStatus_Licensable;
    } else {
        res.licensableSku[0] = '\0';
        res.status = XStoreCanLicenseStatus_NotLicensableToUser;
    }
    return RecordHr(
        "XStoreCanAcquireLicenseForPackageAsync",
        StartPodAsync<XStoreCanAcquireLicenseResult>(
            async, kIdStoreCanAcquirePkg, kIdStoreCanAcquirePkg, res));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForPackageResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult) {
    if (!async || !storeCanAcquireLicenseResult) {
        return RecordHr("XStoreCanAcquireLicenseForPackageResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreCanAcquireLicenseForPackageResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreCanAcquirePkg, sizeof(XStoreCanAcquireLicenseResult),
        storeCanAcquireLicenseResult, nullptr);
    return RecordHr("XStoreCanAcquireLicenseForPackageResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForStoreIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) {
    if (!storeContextHandle || !IsValidOfflineIdentifier(storeProductId, 64u) || !async) {
        return RecordHr("XStoreCanAcquireLicenseForStoreIdAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreCanAcquireLicenseForStoreIdAsync", XGR_E_HANDLE);
    }
    const std::string req_lo = ToLowerAsciiStr(TrimAscii(storeProductId));
    const std::string cur_sid_lo = ToLowerAsciiStr(CurrentStoreId());
    const std::string default_sid_lo = ToLowerAsciiStr(kDefaultStoreId);
    const auto lsa_pkg = openscarlett::ConsoleLsaPolicy::Global().GetPackageIdentity();
    const bool lsa_is_explicit =
        !lsa_pkg.package_name.empty() &&
        lsa_pkg.package_name != "OpenScarlett.DefaultTitle";
    const std::string lsa_sid_lo =
        lsa_is_explicit ? ToLowerAsciiStr(lsa_pkg.store_id) : std::string();
    const std::vector<std::string> addons = ConfiguredAddOnStoreIds();

    bool owned = (req_lo == cur_sid_lo) ||
                 (!lsa_sid_lo.empty() && req_lo == lsa_sid_lo);
    if (!owned) {
        for (const auto& addon : addons) {
            if (ToLowerAsciiStr(addon) == req_lo) {
                owned = true;
                break;
            }
        }
    }
    if (!owned) {
        // When no title-specific StoreId or add-on catalog is configured (standalone offline
        // mode without MicrosoftGame.Config StoreId), allow valid non-empty store product IDs
        // so unpacked titles and offline tests are not blocked.
        const bool has_explicit_catalog =
            (cur_sid_lo != default_sid_lo) ||
            (!lsa_sid_lo.empty() && lsa_sid_lo != default_sid_lo) ||
            !addons.empty();
        if (!has_explicit_catalog) {
            owned = true;
        }
    }

    XStoreCanAcquireLicenseResult res{};
    if (owned) {
        std::snprintf(res.licensableSku, sizeof(res.licensableSku), "%s", "0001");
        res.status = XStoreCanLicenseStatus_Licensable;
    } else {
        res.licensableSku[0] = '\0';
        res.status = XStoreCanLicenseStatus_NotLicensableToUser;
    }
    return RecordHr(
        "XStoreCanAcquireLicenseForStoreIdAsync",
        StartPodAsync<XStoreCanAcquireLicenseResult>(
            async, kIdStoreCanAcquireStoreId, kIdStoreCanAcquireStoreId, res));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreCanAcquireLicenseForStoreIdResult(
    XAsyncBlock* async,
    XStoreCanAcquireLicenseResult* storeCanAcquireLicenseResult) {
    if (!async || !storeCanAcquireLicenseResult) {
        return RecordHr("XStoreCanAcquireLicenseForStoreIdResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreCanAcquireLicenseForStoreIdResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreCanAcquireStoreId, sizeof(XStoreCanAcquireLicenseResult),
        storeCanAcquireLicenseResult, nullptr);
    return RecordHr("XStoreCanAcquireLicenseForStoreIdResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForPackageAsync(
    XStoreContextHandle storeContextHandle,
    const char* packageIdentifier,
    XAsyncBlock* async) {
    if (!storeContextHandle || !packageIdentifier || !*packageIdentifier || !async) {
        return RecordHr("XStoreAcquireLicenseForPackageAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreAcquireLicenseForPackageAsync", XGR_E_HANDLE);
    }
    auto* ctx = new (std::nothrow) StoreAcquireLicenseAsyncCtx{packageIdentifier, nullptr};
    if (!ctx) {
        return RecordHr("XStoreAcquireLicenseForPackageAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdStoreAcquirePkg, kIdStoreAcquirePkg, &StoreAcquireLicenseProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XStoreAcquireLicenseForPackageAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XStoreAcquireLicenseForPackageAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForPackageResult(
    XAsyncBlock* async,
    XStoreLicenseHandle* storeLicenseHandle) {
    if (!async || !storeLicenseHandle) {
        return RecordHr("XStoreAcquireLicenseForPackageResult", XGR_E_INVALIDARG);
    }
    *storeLicenseHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreAcquireLicenseForPackageResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreAcquirePkg, sizeof(XStoreLicenseHandle), storeLicenseHandle, nullptr);
    return RecordHr("XStoreAcquireLicenseForPackageResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForDurablesAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeId,
    XAsyncBlock* async) {
    if (!storeContextHandle || !storeId || !*storeId || !async) {
        return RecordHr("XStoreAcquireLicenseForDurablesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreAcquireLicenseForDurablesAsync", XGR_E_HANDLE);
    }
    auto* ctx = new (std::nothrow) StoreAcquireLicenseAsyncCtx{storeId, nullptr};
    if (!ctx) {
        return RecordHr("XStoreAcquireLicenseForDurablesAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdStoreAcquireDurable, kIdStoreAcquireDurable, &StoreAcquireLicenseProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XStoreAcquireLicenseForDurablesAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XStoreAcquireLicenseForDurablesAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreAcquireLicenseForDurablesResult(
    XAsyncBlock* async,
    XStoreLicenseHandle* storeLicenseHandle) {
    if (!async || !storeLicenseHandle) {
        return RecordHr("XStoreAcquireLicenseForDurablesResult", XGR_E_INVALIDARG);
    }
    *storeLicenseHandle = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreAcquireLicenseForDurablesResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreAcquireDurable, sizeof(XStoreLicenseHandle), storeLicenseHandle, nullptr);
    return RecordHr("XStoreAcquireLicenseForDurablesResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryConsumableBalanceRemainingAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    XAsyncBlock* async) {
    if (!storeContextHandle || !storeProductId || !*storeProductId || !async) {
        return RecordHr("XStoreQueryConsumableBalanceRemainingAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryConsumableBalanceRemainingAsync", XGR_E_HANDLE);
    }
    XStoreConsumableResult res{0u};
    return RecordHr(
        "XStoreQueryConsumableBalanceRemainingAsync",
        StartPodAsync<XStoreConsumableResult>(
            async, kIdStoreConsumableBal, kIdStoreConsumableBal, res));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryConsumableBalanceRemainingResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult) {
    if (!async || !consumableResult) {
        return RecordHr("XStoreQueryConsumableBalanceRemainingResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryConsumableBalanceRemainingResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreConsumableBal, sizeof(XStoreConsumableResult), consumableResult, nullptr);
    return RecordHr("XStoreQueryConsumableBalanceRemainingResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreReportConsumableFulfillmentAsync(
    XStoreContextHandle storeContextHandle,
    const char* storeProductId,
    uint32_t /*quantity*/,
    XgrGuid /*trackingId*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !storeProductId || !*storeProductId || !async) {
        return RecordHr("XStoreReportConsumableFulfillmentAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreReportConsumableFulfillmentAsync", XGR_E_HANDLE);
    }
    XStoreConsumableResult res{0u};
    return RecordHr(
        "XStoreReportConsumableFulfillmentAsync",
        StartPodAsync<XStoreConsumableResult>(
            async, kIdStoreConsumableFulfill, kIdStoreConsumableFulfill, res));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreReportConsumableFulfillmentResult(
    XAsyncBlock* async,
    XStoreConsumableResult* consumableResult) {
    if (!async || !consumableResult) {
        return RecordHr("XStoreReportConsumableFulfillmentResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreReportConsumableFulfillmentResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdStoreConsumableFulfill, sizeof(XStoreConsumableResult),
        consumableResult, nullptr);
    return RecordHr("XStoreReportConsumableFulfillmentResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryGameAndDlcPackageUpdatesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryGameAndDlcPackageUpdatesAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreQueryGameAndDlcPackageUpdatesAsync",
        StartVoidAsync(async, kIdStorePkgUpdates, kIdStorePkgUpdates, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesResultCount(
    XAsyncBlock* async,
    uint32_t* count) {
    if (!async || !count) {
        return RecordHr("XStoreQueryGameAndDlcPackageUpdatesResultCount", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryGameAndDlcPackageUpdatesResultCount", hr);
    }
    *count = 0u;
    return RecordHr("XStoreQueryGameAndDlcPackageUpdatesResultCount", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryGameAndDlcPackageUpdatesResult(
    XAsyncBlock* async,
    uint32_t /*count*/,
    XStorePackageUpdate* /*packageUpdates*/) {
    if (!async) {
        return RecordHr("XStoreQueryGameAndDlcPackageUpdatesResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreQueryGameAndDlcPackageUpdatesResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    const char** /*packageIdentifiers*/,
    size_t /*packageIdentifiersCount*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreDownloadPackageUpdatesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreDownloadPackageUpdatesAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreDownloadPackageUpdatesAsync",
        StartVoidAsync(async, kIdStoreDownloadUpdates, kIdStoreDownloadUpdates, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadPackageUpdatesResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreDownloadPackageUpdatesResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreDownloadPackageUpdatesResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackageUpdatesAsync(
    XStoreContextHandle storeContextHandle,
    const char** /*packageIdentifiers*/,
    size_t /*packageIdentifiersCount*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreDownloadAndInstallPackageUpdatesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreDownloadAndInstallPackageUpdatesAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreDownloadAndInstallPackageUpdatesAsync",
        StartVoidAsync(
            async, kIdStoreDownloadInstallUpdates, kIdStoreDownloadInstallUpdates, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackageUpdatesResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreDownloadAndInstallPackageUpdatesResult", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XStoreDownloadAndInstallPackageUpdatesResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesAsync(
    XStoreContextHandle storeContextHandle,
    const char** /*storeIds*/,
    size_t /*storeIdsCount*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreDownloadAndInstallPackagesAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreDownloadAndInstallPackagesAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreDownloadAndInstallPackagesAsync",
        StartVoidAsync(async, kIdStoreDownloadInstallPkgs, kIdStoreDownloadInstallPkgs, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesResultCount(
    XAsyncBlock* async,
    uint32_t* count) {
    if (!async || !count) {
        return RecordHr("XStoreDownloadAndInstallPackagesResultCount", XGR_E_INVALIDARG);
    }
    const XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreDownloadAndInstallPackagesResultCount", hr);
    }
    *count = 0u;
    return RecordHr("XStoreDownloadAndInstallPackagesResultCount", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XStoreDownloadAndInstallPackagesResult(
    XAsyncBlock* async,
    uint32_t /*count*/,
    char (*/*packageIdentifiers*/)[XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH]) {
    if (!async) {
        return RecordHr("XStoreDownloadAndInstallPackagesResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreDownloadAndInstallPackagesResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*serviceTicket*/,
    const char* /*publisherUserId*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreGetUserCollectionsIdAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreGetUserCollectionsIdAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreGetUserCollectionsIdAsync",
        StartStringAsync(
            async, kIdStoreCollectionsId, kIdStoreCollectionsId,
            "openscarlett-offline-collections-id"));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdResultSize(
    XAsyncBlock* async,
    size_t* size) {
    if (!async || !size) {
        return RecordHr("XStoreGetUserCollectionsIdResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreGetUserCollectionsIdResultSize", hr);
    }
    return RecordHr("XStoreGetUserCollectionsIdResultSize", XAsyncGetResultSize(async, size));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserCollectionsIdResult(
    XAsyncBlock* async,
    size_t size,
    char* result) {
    if (!async || !result) {
        return RecordHr("XStoreGetUserCollectionsIdResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreGetUserCollectionsIdResult", hr);
    }
    return RecordHr(
        "XStoreGetUserCollectionsIdResult",
        XAsyncGetResult(async, kIdStoreCollectionsId, size, result, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*serviceTicket*/,
    const char* /*publisherUserId*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreGetUserPurchaseIdAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreGetUserPurchaseIdAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreGetUserPurchaseIdAsync",
        StartStringAsync(
            async, kIdStorePurchaseId, kIdStorePurchaseId,
            "openscarlett-offline-purchase-id"));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdResultSize(
    XAsyncBlock* async,
    size_t* size) {
    if (!async || !size) {
        return RecordHr("XStoreGetUserPurchaseIdResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreGetUserPurchaseIdResultSize", hr);
    }
    return RecordHr("XStoreGetUserPurchaseIdResultSize", XAsyncGetResultSize(async, size));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreGetUserPurchaseIdResult(
    XAsyncBlock* async,
    size_t size,
    char* result) {
    if (!async || !result) {
        return RecordHr("XStoreGetUserPurchaseIdResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreGetUserPurchaseIdResult", hr);
    }
    return RecordHr(
        "XStoreGetUserPurchaseIdResult",
        XAsyncGetResult(async, kIdStorePurchaseId, size, result, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenAsync(
    XStoreContextHandle storeContextHandle,
    const char** /*productIds*/,
    size_t /*productIdsCount*/,
    const char* /*customDeveloperString*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreQueryLicenseTokenAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreQueryLicenseTokenAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreQueryLicenseTokenAsync",
        StartStringAsync(
            async, kIdStoreLicenseToken, kIdStoreLicenseToken,
            "openscarlett-offline-license-token"));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenResultSize(
    XAsyncBlock* async,
    size_t* size) {
    if (!async || !size) {
        return RecordHr("XStoreQueryLicenseTokenResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryLicenseTokenResultSize", hr);
    }
    return RecordHr("XStoreQueryLicenseTokenResultSize", XAsyncGetResultSize(async, size));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryLicenseTokenResult(
    XAsyncBlock* async,
    size_t size,
    char* result) {
    if (!async || !result) {
        return RecordHr("XStoreQueryLicenseTokenResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XStoreQueryLicenseTokenResult", hr);
    }
    return RecordHr(
        "XStoreQueryLicenseTokenResult",
        XAsyncGetResult(async, kIdStoreLicenseToken, size, result, nullptr));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreQueryPackageIdentifier(
    const char* storeId,
    size_t size,
    char* packageIdentifier) {
    if (!storeId || !*storeId || !packageIdentifier) {
        return RecordHr("XStoreQueryPackageIdentifier", XGR_E_INVALIDARG);
    }
    std::string pkg = CurrentPackageIdentifier();
    if (pkg.size() >= XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH) {
        pkg.resize(XSTORE_PACKAGE_IDENTIFIER_MAX_LENGTH - 1);
    }
    return RecordHr(
        "XStoreQueryPackageIdentifier",
        CopyStringResult(pkg, size, packageIdentifier, nullptr));
}

bool OPENSCARLETT_GDK_CALL XStoreIsAvailabilityPurchasable(
    const XStoreAvailability availability) {
    RecordOk("XStoreIsAvailabilityPurchasable");
    return availability.availabilityId != nullptr && availability.availabilityId[0] != '\0';
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowAssociatedProductsUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*storeId*/,
    XStoreProductKind /*productKinds*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreShowAssociatedProductsUIAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreShowAssociatedProductsUIAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreShowAssociatedProductsUIAsync",
        StartVoidAsync(async, kIdStoreShowAssocUi, kIdStoreShowAssocUi, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowAssociatedProductsUIResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreShowAssociatedProductsUIResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreShowAssociatedProductsUIResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowProductPageUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*storeId*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreShowProductPageUIAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreShowProductPageUIAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreShowProductPageUIAsync",
        StartVoidAsync(async, kIdStoreShowProdUi, kIdStoreShowProdUi, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowProductPageUIResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreShowProductPageUIResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreShowProductPageUIResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowPurchaseUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*storeId*/,
    const char* /*name*/,
    const char* /*extendedJsonData*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreShowPurchaseUIAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreShowPurchaseUIAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreShowPurchaseUIAsync",
        StartVoidAsync(async, kIdStoreShowPurchaseUi, kIdStoreShowPurchaseUi, XGR_E_ABORT));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowPurchaseUIResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreShowPurchaseUIResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreShowPurchaseUIResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowRateAndReviewUIAsync(
    XStoreContextHandle storeContextHandle,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreShowRateAndReviewUIAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreShowRateAndReviewUIAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreShowRateAndReviewUIAsync",
        StartVoidAsync(async, kIdStoreShowRateUi, kIdStoreShowRateUi, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowRateAndReviewUIResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreShowRateAndReviewUIResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreShowRateAndReviewUIResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowRedeemTokenUIAsync(
    XStoreContextHandle storeContextHandle,
    const char* /*token*/,
    const char** /*allowedStoreIds*/,
    size_t /*allowedStoreIdsCount*/,
    bool /*disallowCsvRedemption*/,
    XAsyncBlock* async) {
    if (!storeContextHandle || !async) {
        return RecordHr("XStoreShowRedeemTokenUIAsync", XGR_E_INVALIDARG);
    }
    if (!LookupStoreContext(storeContextHandle)) {
        return RecordHr("XStoreShowRedeemTokenUIAsync", XGR_E_HANDLE);
    }
    return RecordHr(
        "XStoreShowRedeemTokenUIAsync",
        StartVoidAsync(async, kIdStoreShowRedeemUi, kIdStoreShowRedeemUi, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XStoreShowRedeemTokenUIResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XStoreShowRedeemTokenUIResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XStoreShowRedeemTokenUIResult", XAsyncGetStatus(async, true));
}

// ===========================================================================
// XGameUi (non-blocking offline/local defaults)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XGameUiSetNotificationPositionHint(
    XGameUiNotificationPositionHint position) {
    GetRuntime().ui_notification_hint.store(
        static_cast<uint32_t>(position), std::memory_order_release);
    return RecordHr("XGameUiSetNotificationPositionHint", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowMessageDialogAsync(
    XAsyncBlock* async,
    const char* /*titleText*/,
    const char* /*contentText*/,
    const char* firstButtonText,
    const char* secondButtonText,
    const char* thirdButtonText,
    XGameUiMessageDialogButton defaultButton,
    XGameUiMessageDialogButton cancelButton) {
    if (!async ||
        !ValidateMessageDialogButtons(
            static_cast<uint32_t>(defaultButton),
            static_cast<uint32_t>(cancelButton),
            firstButtonText,
            secondButtonText,
            thirdButtonText)) {
        return RecordHr("XGameUiShowMessageDialogAsync", XGR_E_INVALIDARG);
    }
    XGameUiMessageDialogButton chosen = defaultButton;
    const std::string env_btn = GetEnvString("OPENSCARLETT_UI_DIALOG_BUTTON");
    if (!env_btn.empty()) {
        XGameUiMessageDialogButton cand = chosen;
        bool matched = false;
        if (env_btn == "0" || env_btn == "first") {
            cand = XGameUiMessageDialogButton_First;
            matched = true;
        } else if (env_btn == "1" || env_btn == "second") {
            cand = XGameUiMessageDialogButton_Second;
            matched = true;
        } else if (env_btn == "2" || env_btn == "third") {
            cand = XGameUiMessageDialogButton_Third;
            matched = true;
        }
        if (matched &&
            IsDialogButtonIndexValid(
                static_cast<uint32_t>(cand),
                firstButtonText,
                secondButtonText,
                thirdButtonText)) {
            chosen = cand;
        }
    }
    return RecordHr(
        "XGameUiShowMessageDialogAsync",
        StartPodAsync<XGameUiMessageDialogButton>(
            async, kIdUiMessageDialog, kIdUiMessageDialog, chosen));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowMessageDialogResult(
    XAsyncBlock* async,
    XGameUiMessageDialogButton* resultButton) {
    if (!async || !resultButton) {
        return RecordHr("XGameUiShowMessageDialogResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowMessageDialogResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdUiMessageDialog, sizeof(XGameUiMessageDialogButton), resultButton, nullptr);
    return RecordHr("XGameUiShowMessageDialogResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowErrorDialogAsync(
    XAsyncBlock* async,
    XGameResult /*errorCode*/,
    const char* /*context*/) {
    if (!async) {
        return RecordHr("XGameUiShowErrorDialogAsync", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XGameUiShowErrorDialogAsync",
        StartVoidAsync(async, kIdUiErrorDialog, kIdUiErrorDialog, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowErrorDialogResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameUiShowErrorDialogResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XGameUiShowErrorDialogResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryAsync(
    XAsyncBlock* async,
    const char* /*titleText*/,
    const char* /*descriptionText*/,
    const char* defaultText,
    XGameUiTextEntryInputScope /*inputScope*/,
    uint32_t maxTextLength) {
    if (!async) {
        return RecordHr("XGameUiShowTextEntryAsync", XGR_E_INVALIDARG);
    }
    std::string text = defaultText ? defaultText : "";
    if (maxTextLength > 0 && text.size() > maxTextLength) {
        text.resize(maxTextLength);
    }
    return RecordHr(
        "XGameUiShowTextEntryAsync",
        StartStringAsync(async, kIdUiTextEntry, kIdUiTextEntry, std::move(text)));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryResultSize(
    XAsyncBlock* async,
    uint32_t* resultTextBufferSize) {
    if (!async || !resultTextBufferSize) {
        return RecordHr("XGameUiShowTextEntryResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowTextEntryResultSize", hr);
    }
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr >= 0) {
        *resultTextBufferSize = static_cast<uint32_t>(sz);
    }
    return RecordHr("XGameUiShowTextEntryResultSize", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowTextEntryResult(
    XAsyncBlock* async,
    uint32_t resultTextBufferSize,
    char* resultTextBuffer,
    uint32_t* resultTextBufferUsed) {
    if (!async || !resultTextBuffer) {
        return RecordHr("XGameUiShowTextEntryResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowTextEntryResult", hr);
    }
    size_t used = 0;
    hr = XAsyncGetResult(async, kIdUiTextEntry, resultTextBufferSize, resultTextBuffer, &used);
    if (hr >= 0 && resultTextBufferUsed) {
        *resultTextBufferUsed = static_cast<uint32_t>(used);
    }
    return RecordHr("XGameUiShowTextEntryResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowAchievementsAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    uint32_t /*titleId*/) {
    if (!async) {
        return RecordHr("XGameUiShowAchievementsAsync", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XGameUiShowAchievementsAsync",
        StartVoidAsync(async, kIdUiAchievements, kIdUiAchievements, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowAchievementsResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameUiShowAchievementsResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XGameUiShowAchievementsResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    const char* /*promptText*/,
    uint32_t /*selectFromPlayersCount*/,
    const uint64_t* /*selectFromPlayers*/,
    uint32_t preSelectedPlayersCount,
    const uint64_t* preSelectedPlayers,
    uint32_t /*minSelectionCount*/,
    uint32_t maxSelectionCount) {
    if (!async) {
        return RecordHr("XGameUiShowPlayerPickerAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) UiPlayerPickerAsyncCtx();
    if (!ctx) {
        return RecordHr("XGameUiShowPlayerPickerAsync", XGR_E_OUTOFMEMORY);
    }
    if (preSelectedPlayers && preSelectedPlayersCount > 0) {
        const uint32_t limit =
            (maxSelectionCount > 0) ? std::min(preSelectedPlayersCount, maxSelectionCount)
                                    : preSelectedPlayersCount;
        ctx->players.assign(preSelectedPlayers, preSelectedPlayers + limit);
    }
    XGameResult hr = XAsyncBegin(
        async, ctx, kIdUiPlayerPicker, kIdUiPlayerPicker, &UiPlayerPickerProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUiShowPlayerPickerAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUiShowPlayerPickerAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerResultCount(
    XAsyncBlock* async,
    uint32_t* resultPlayersCount) {
    if (!async || !resultPlayersCount) {
        return RecordHr("XGameUiShowPlayerPickerResultCount", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowPlayerPickerResultCount", hr);
    }
    size_t sz = 0;
    hr = XAsyncGetResultSize(async, &sz);
    if (hr >= 0) {
        *resultPlayersCount = static_cast<uint32_t>(sz / sizeof(uint64_t));
    }
    return RecordHr("XGameUiShowPlayerPickerResultCount", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerPickerResult(
    XAsyncBlock* async,
    uint32_t resultPlayersCount,
    uint64_t* resultPlayers,
    uint32_t* resultPlayersUsed) {
    if (!async) {
        return RecordHr("XGameUiShowPlayerPickerResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowPlayerPickerResult", hr);
    }
    size_t req_bytes = 0;
    hr = XAsyncGetResultSize(async, &req_bytes);
    if (hr < 0) {
        return RecordHr("XGameUiShowPlayerPickerResult", hr);
    }
    if (req_bytes == 0) {
        if (resultPlayersUsed) *resultPlayersUsed = 0u;
        return RecordHr("XGameUiShowPlayerPickerResult", XGR_S_OK);
    }
    if (!resultPlayers) {
        return RecordHr("XGameUiShowPlayerPickerResult", XGR_E_INVALIDARG);
    }
    size_t used_bytes = 0;
    hr = XAsyncGetResult(
        async, kIdUiPlayerPicker, static_cast<size_t>(resultPlayersCount) * sizeof(uint64_t),
        resultPlayers, &used_bytes);
    if (hr >= 0 && resultPlayersUsed) {
        *resultPlayersUsed = static_cast<uint32_t>(used_bytes / sizeof(uint64_t));
    }
    return RecordHr("XGameUiShowPlayerPickerResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerProfileCardAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    uint64_t /*targetPlayer*/) {
    if (!async) {
        return RecordHr("XGameUiShowPlayerProfileCardAsync", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XGameUiShowPlayerProfileCardAsync",
        StartVoidAsync(async, kIdUiProfileCard, kIdUiProfileCard, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowPlayerProfileCardResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameUiShowPlayerProfileCardResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XGameUiShowPlayerProfileCardResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowSendGameInviteAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    const char* /*sessionConfigurationId*/,
    const char* /*sessionTemplateName*/,
    const char* /*sessionId*/,
    const char* /*invitationText*/,
    const char* /*customActivationContext*/) {
    if (!async) {
        return RecordHr("XGameUiShowSendGameInviteAsync", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XGameUiShowSendGameInviteAsync",
        StartVoidAsync(async, kIdUiSendInvite, kIdUiSendInvite, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowSendGameInviteResult(XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XGameUiShowSendGameInviteResult", XGR_E_INVALIDARG);
    }
    return RecordHr("XGameUiShowSendGameInviteResult", XAsyncGetStatus(async, true));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationWithOptionsAsync(
    XAsyncBlock* async,
    XUserHandle /*requestingUser*/,
    const char* requestUri,
    const char* completionUri,
    XGameUiWebAuthenticationOptions /*options*/) {
    if (!async || !requestUri || !*requestUri) {
        return RecordHr("XGameUiShowWebAuthenticationWithOptionsAsync", XGR_E_INVALIDARG);
    }
    auto* ctx = new (std::nothrow) UiWebAuthAsyncCtx{completionUri ? completionUri : ""};
    if (!ctx) {
        return RecordHr("XGameUiShowWebAuthenticationWithOptionsAsync", XGR_E_OUTOFMEMORY);
    }
    XGameResult hr = XAsyncBegin(async, ctx, kIdUiWebAuth, kIdUiWebAuth, &UiWebAuthProvider);
    if (hr < 0) {
        delete ctx;
        return RecordHr("XGameUiShowWebAuthenticationWithOptionsAsync", hr);
    }
    hr = XAsyncSchedule(async, 0);
    if (hr < 0) {
        XAsyncComplete(async, hr, 0);
    }
    return RecordHr("XGameUiShowWebAuthenticationWithOptionsAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationAsync(
    XAsyncBlock* async,
    XUserHandle requestingUser,
    const char* requestUri,
    const char* completionUri) {
    const XGameResult hr = XGameUiShowWebAuthenticationWithOptionsAsync(
        async, requestingUser, requestUri, completionUri, XGameUiWebAuthenticationOptions_None);
    return RecordHr("XGameUiShowWebAuthenticationAsync", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationResultSize(
    XAsyncBlock* async,
    size_t* bufferSize) {
    if (!async || !bufferSize) {
        return RecordHr("XGameUiShowWebAuthenticationResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowWebAuthenticationResultSize", hr);
    }
    return RecordHr("XGameUiShowWebAuthenticationResultSize", XAsyncGetResultSize(async, bufferSize));
}

XGameResult OPENSCARLETT_GDK_CALL XGameUiShowWebAuthenticationResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XGameUiWebAuthenticationResultData** ptrToBuffer,
    size_t* bufferUsed) {
    if (!async || !buffer || !ptrToBuffer) {
        return RecordHr("XGameUiShowWebAuthenticationResult", XGR_E_INVALIDARG);
    }
    *ptrToBuffer = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XGameUiShowWebAuthenticationResult", hr);
    }
    hr = XAsyncGetResult(async, kIdUiWebAuth, bufferSize, buffer, bufferUsed);
    if (hr >= 0) {
        *ptrToBuffer = static_cast<XGameUiWebAuthenticationResultData*>(buffer);
    }
    return RecordHr("XGameUiShowWebAuthenticationResult", hr);
}

// ===========================================================================
// XNetworking (PC/local connectivity & TLS info)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XNetworkingGetConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint) {
    if (!connectivityHint) {
        return RecordHr("XNetworkingGetConnectivityHint", XGR_E_INVALIDARG);
    }
    connectivityHint->connectivityLevel = XNetworkingConnectivityLevelHint_InternetAccess;
    connectivityHint->connectivityCost = XNetworkingConnectivityCostHint_Unrestricted;
    connectivityHint->ianaInterfaceType = 6u;  // IF_TYPE_ETHERNET_CSMACD
    connectivityHint->networkInitialized = true;
    connectivityHint->approachingDataLimit = false;
    connectivityHint->overDataLimit = false;
    connectivityHint->roaming = false;
    return RecordHr("XNetworkingGetConnectivityHint", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryConnectivityHint(
    XNetworkingConnectivityHint* connectivityHint) {
    const XGameResult hr = XNetworkingGetConnectivityHint(connectivityHint);
    return RecordHr("XNetworkingQueryConnectivityHint", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPort(
    uint16_t* preferredLocalUdpMultiplayerPort) {
    if (!preferredLocalUdpMultiplayerPort) {
        return RecordHr("XNetworkingQueryPreferredLocalUdpMultiplayerPort", XGR_E_INVALIDARG);
    }
    *preferredLocalUdpMultiplayerPort = 3074u;
    return RecordHr("XNetworkingQueryPreferredLocalUdpMultiplayerPort", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(
    XAsyncBlock* async) {
    if (!async) {
        return RecordHr("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", XGR_E_INVALIDARG);
    }
    const uint16_t port = 3074u;
    return RecordHr(
        "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync",
        StartPodAsync<uint16_t>(async, kIdNetUdpPort, kIdNetUdpPort, port));
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(
    XAsyncBlock* async,
    uint16_t* preferredLocalUdpMultiplayerPort) {
    if (!async || !preferredLocalUdpMultiplayerPort) {
        return RecordHr(
            "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdNetUdpPort, sizeof(uint16_t), preferredLocalUdpMultiplayerPort, nullptr);
    return RecordHr("XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingRegisterConnectivityHintChanged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XNetworkingConnectivityHintChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr("XNetworkingRegisterConnectivityHintChanged", XGR_E_INVALIDARG);
    }
    *token = AllocateRegToken(kRegNetConnectivity);
    return RecordHr("XNetworkingRegisterConnectivityHintChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XNetworkingUnregisterConnectivityHintChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XNetworkingUnregisterConnectivityHintChanged");
    return ReleaseRegToken(kRegNetConnectivity, token);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueHandle /*queue*/,
    void* /*context*/,
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback callback,
    XTaskQueueRegistrationToken* token) {
    if (!callback || !token) {
        return RecordHr(
            "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", XGR_E_INVALIDARG);
    }
    *token = AllocateRegToken(kRegNetUdpPort);
    return RecordHr("XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", XGR_S_OK);
}

bool OPENSCARLETT_GDK_CALL XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueRegistrationToken token,
    bool /*wait*/) {
    RecordOk("XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged");
    return ReleaseRegToken(kRegNetUdpPort, token);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsync(
    const char* url,
    XAsyncBlock* async) {
    if (!url || !*url || !async) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlAsync", XGR_E_INVALIDARG);
    }
    XNetworkingSecurityInformation info{};
    info.enabledHttpSecurityProtocolFlags = 0x00000A00u;  // TLS 1.1 | TLS 1.2
    info.thumbprintCount = 0;
    info.thumbprints = nullptr;
    return RecordHr(
        "XNetworkingQuerySecurityInformationForUrlAsync",
        StartPodAsync<XNetworkingSecurityInformation>(async, kIdNetSecInfo, kIdNetSecInfo, info));
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsyncResultSize(
    XAsyncBlock* async,
    size_t* securityInformationBufferByteCount) {
    if (!async || !securityInformationBufferByteCount) {
        return RecordHr(
            "XNetworkingQuerySecurityInformationForUrlAsyncResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlAsyncResultSize", hr);
    }
    return RecordHr(
        "XNetworkingQuerySecurityInformationForUrlAsyncResultSize",
        XAsyncGetResultSize(async, securityInformationBufferByteCount));
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlAsyncResult(
    XAsyncBlock* async,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** pointerToSecurityInformation) {
    if (!async || !securityInformationBuffer || !pointerToSecurityInformation) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlAsyncResult", XGR_E_INVALIDARG);
    }
    *pointerToSecurityInformation = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlAsyncResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdNetSecInfo, securityInformationBufferByteCount,
        securityInformationBuffer, securityInformationBufferByteCountUsed);
    if (hr >= 0) {
        *pointerToSecurityInformation =
            reinterpret_cast<XNetworkingSecurityInformation*>(securityInformationBuffer);
    }
    return RecordHr("XNetworkingQuerySecurityInformationForUrlAsyncResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16Async(
    const wchar_t* url,
    XAsyncBlock* async) {
    if (!url || !*url || !async) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlUtf16Async", XGR_E_INVALIDARG);
    }
    XNetworkingSecurityInformation info{};
    info.enabledHttpSecurityProtocolFlags = 0x00000A00u;  // TLS 1.1 | TLS 1.2
    info.thumbprintCount = 0;
    info.thumbprints = nullptr;
    return RecordHr(
        "XNetworkingQuerySecurityInformationForUrlUtf16Async",
        StartPodAsync<XNetworkingSecurityInformation>(
            async, kIdNetSecInfoUtf16, kIdNetSecInfoUtf16, info));
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(
    XAsyncBlock* async,
    size_t* securityInformationBufferByteCount) {
    if (!async || !securityInformationBufferByteCount) {
        return RecordHr(
            "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize", XGR_E_INVALIDARG);
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize", hr);
    }
    return RecordHr(
        "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize",
        XAsyncGetResultSize(async, securityInformationBufferByteCount));
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(
    XAsyncBlock* async,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** pointerToSecurityInformation) {
    if (!async || !securityInformationBuffer || !pointerToSecurityInformation) {
        return RecordHr(
            "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", XGR_E_INVALIDARG);
    }
    *pointerToSecurityInformation = nullptr;
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr < 0) {
        return RecordHr("XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", hr);
    }
    hr = XAsyncGetResult(
        async, kIdNetSecInfoUtf16, securityInformationBufferByteCount,
        securityInformationBuffer, securityInformationBufferByteCountUsed);
    if (hr >= 0) {
        *pointerToSecurityInformation =
            reinterpret_cast<XNetworkingSecurityInformation*>(securityInformationBuffer);
    }
    return RecordHr("XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", hr);
}

XGameResult OPENSCARLETT_GDK_CALL XNetworkingVerifyServerCertificate(
    void* requestHandle,
    const XNetworkingSecurityInformation* securityInformation) {
    if (!requestHandle || !securityInformation) {
        return RecordHr("XNetworkingVerifyServerCertificate", XGR_E_INVALIDARG);
    }
    return RecordHr("XNetworkingVerifyServerCertificate", XGR_S_OK);
}

// ===========================================================================
// Core Runtime Extensions (PLS, XThread, XError, XGameEvent, XGameInvite, XLaunch, XAccessibility)
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetPathSize(size_t* pathSize) {
    if (!pathSize) {
        return RecordHr("XPersistentLocalStorageGetPathSize", XGR_E_INVALIDARG);
    }
    const fs::path pls_dir = ResolveSaveRoot() / "pls";
    std::error_code ec;
    fs::create_directories(pls_dir, ec);
    const std::string pls_str = pls_dir.string();
    *pathSize = pls_str.size() + 1;
    return RecordHr("XPersistentLocalStorageGetPathSize", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetPath(
    size_t pathSize,
    char* path,
    size_t* pathUsed) {
    const fs::path pls_dir = ResolveSaveRoot() / "pls";
    std::error_code ec;
    fs::create_directories(pls_dir, ec);
    const std::string pls_str = pls_dir.string();
    return RecordHr(
        "XPersistentLocalStorageGetPath",
        CopyStringResult(pls_str, pathSize, path, pathUsed));
}

XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStorageGetSpaceInfo(
    XPersistentLocalStorageSpaceInfo* spaceInfo) {
    if (!spaceInfo) {
        return RecordHr("XPersistentLocalStorageGetSpaceInfo", XGR_E_INVALIDARG);
    }
    const fs::path pls_dir = ResolveSaveRoot() / "pls";
    std::error_code ec;
    fs::create_directories(pls_dir, ec);
    const auto sp = fs::space(pls_dir, ec);
    if (!ec && sp.capacity > 0u) {
        spaceInfo->availableFreeBytes = static_cast<uint64_t>(sp.available);
        spaceInfo->totalFreeBytes = static_cast<uint64_t>(sp.free);
        spaceInfo->totalBytes = static_cast<uint64_t>(sp.capacity);
        spaceInfo->usedBytes = (sp.capacity >= sp.free)
                                   ? static_cast<uint64_t>(sp.capacity - sp.free)
                                   : 0ULL;
    } else {
        constexpr uint64_t kFallbackFree = 4ULL * 1024ULL * 1024ULL * 1024ULL;
        spaceInfo->availableFreeBytes = kFallbackFree;
        spaceInfo->totalFreeBytes = kFallbackFree;
        spaceInfo->usedBytes = 0ULL;
        spaceInfo->totalBytes = kFallbackFree;
    }
    return RecordHr("XPersistentLocalStorageGetSpaceInfo", XGR_S_OK);
}

static const char kIdPlsPromptSpace[] = "XPersistentLocalStoragePromptUserForSpaceAsync";

XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStoragePromptUserForSpaceAsync(
    uint64_t /*requestedBytes*/,
    XAsyncBlock* asyncBlock) {
    if (!asyncBlock) {
        return RecordHr("XPersistentLocalStoragePromptUserForSpaceAsync", XGR_E_INVALIDARG);
    }
    // Headless PC mode: succeed cleanly without blocking on a modal UI prompt.
    return RecordHr(
        "XPersistentLocalStoragePromptUserForSpaceAsync",
        StartVoidAsync(asyncBlock, kIdPlsPromptSpace, kIdPlsPromptSpace, XGR_S_OK));
}

XGameResult OPENSCARLETT_GDK_CALL XPersistentLocalStoragePromptUserForSpaceResult(
    XAsyncBlock* asyncBlock) {
    if (!asyncBlock) {
        return RecordHr("XPersistentLocalStoragePromptUserForSpaceResult", XGR_E_INVALIDARG);
    }
    return RecordHr(
        "XPersistentLocalStoragePromptUserForSpaceResult", XAsyncGetStatus(asyncBlock, true));
}

bool OPENSCARLETT_GDK_CALL XThreadIsTimeSensitive(void) {
    RecordOk("XThreadIsTimeSensitive");
    return g_thread_time_sensitive;
}

XGameResult OPENSCARLETT_GDK_CALL XThreadSetTimeSensitive(bool isTimeSensitiveThread) {
    g_thread_time_sensitive = isTimeSensitiveThread;
    return RecordHr("XThreadSetTimeSensitive", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL XThreadAssertNotTimeSensitive(void) {
    RecordOk("XThreadAssertNotTimeSensitive");
}

bool OPENSCARLETT_GDK_CALL XThreadVerifyNotTimeSensitive(void) {
    RecordOk("XThreadVerifyNotTimeSensitive");
    return !g_thread_time_sensitive;
}

void OPENSCARLETT_GDK_CALL XErrorReport(XGameResult hr, const char* msg) {
    RecordOk("XErrorReport");
    XErrorCallback cb = nullptr;
    void* ctx = nullptr;
    XErrorOptions opts_dbg = XErrorOptions_None;
    XErrorOptions opts_nodbg = XErrorOptions_None;
    {
        auto& rt = GetRuntime();
        std::lock_guard<std::mutex> lk(rt.error_mutex);
        cb = rt.error_callback;
        ctx = rt.error_callback_ctx;
        opts_dbg = rt.error_opts_debugger;
        opts_nodbg = rt.error_opts_no_debugger;
    }
    bool run_default_actions = true;
    if (cb) {
        run_default_actions = cb(hr, msg ? msg : "", ctx);
    }
    if (run_default_actions) {
#if defined(_WIN32)
        const bool dbg_present = (::IsDebuggerPresent() != FALSE);
        const uint32_t active_opts = static_cast<uint32_t>(dbg_present ? opts_dbg : opts_nodbg);
        if ((active_opts & static_cast<uint32_t>(XErrorOptions_OutputDebugStringOnError)) != 0u) {
            char dbg_buf[512]{};
            std::snprintf(
                dbg_buf,
                sizeof(dbg_buf),
                "[OpenScarlett XErrorReport] hr=0x%08X msg=%s\n",
                static_cast<unsigned>(hr),
                msg ? msg : "");
            ::OutputDebugStringA(dbg_buf);
        }
        // DebugBreakOnError (0x02) and FailFastOnError (0x04) are validated and stored, but
        // guarded against terminating headless/compatibility runs unless explicitly opted in.
        if (dbg_present &&
            (active_opts & static_cast<uint32_t>(XErrorOptions_DebugBreakOnError)) != 0u &&
            GetEnvString("OPENSCARLETT_XERROR_BREAK") == "1") {
            ::DebugBreak();
        }
#else
        (void)opts_dbg;
        (void)opts_nodbg;
#endif
    }
}

void OPENSCARLETT_GDK_CALL XErrorSetCallback(XErrorCallback callback, void* context) {
    RecordOk("XErrorSetCallback");
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.error_mutex);
    rt.error_callback = callback;
    rt.error_callback_ctx = context;
}

// Validates and stores GDK XErrorOptions bitmasks:
//   - Bit 0 (0x01): XErrorOptions_OutputDebugStringOnError
//   - Bit 1 (0x02): XErrorOptions_DebugBreakOnError
//   - Bit 2 (0x04): XErrorOptions_FailFastOnError
// Unknown/reserved bits (~0x07u) are stripped before storage and logged as XGR_E_INVALIDARG
// in CallTelemetry so buggy callers cannot corrupt runtime error-handling state.
void OPENSCARLETT_GDK_CALL XErrorSetOptions(
    XErrorOptions optionsDebuggerPresent,
    XErrorOptions optionsDebuggerNotPresent) {
    const uint32_t raw_dbg = static_cast<uint32_t>(optionsDebuggerPresent);
    const uint32_t raw_nodbg = static_cast<uint32_t>(optionsDebuggerNotPresent);
    const bool has_unknown_bits =
        ((raw_dbg & ~kValidXErrorOptionsMask) != 0u) ||
        ((raw_nodbg & ~kValidXErrorOptionsMask) != 0u);
    if (has_unknown_bits) {
        (void)RecordHr("XErrorSetOptions", XGR_E_INVALIDARG);
    } else {
        RecordOk("XErrorSetOptions");
    }
    const auto clean_dbg = static_cast<XErrorOptions>(raw_dbg & kValidXErrorOptionsMask);
    const auto clean_nodbg = static_cast<XErrorOptions>(raw_nodbg & kValidXErrorOptionsMask);
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.error_mutex);
    rt.error_opts_debugger = clean_dbg;
    rt.error_opts_no_debugger = clean_nodbg;
}

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XErrorGetOptions(
    XErrorOptions* optionsDebuggerPresent,
    XErrorOptions* optionsDebuggerNotPresent) {
    if (!optionsDebuggerPresent || !optionsDebuggerNotPresent) {
        return RecordHr("XErrorGetOptions", XGR_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.error_mutex);
    *optionsDebuggerPresent = rt.error_opts_debugger;
    *optionsDebuggerNotPresent = rt.error_opts_no_debugger;
    return RecordHr("XErrorGetOptions", XGR_S_OK);
}

uint32_t OPENSCARLETT_GDK_CALL XGameEventWrite(
    uint64_t /*regHandle*/,
    const XGameEventDescriptor* /*eventDescriptor*/,
    uint32_t /*userDataCount*/,
    XGameEventDataDescriptor* /*userData*/) {
    RecordOk("XGameEventWrite");
    return 0u;  // ERROR_SUCCESS
}

void OPENSCARLETT_GDK_CALL XLaunchNewGame(
    const char* /*exePath*/,
    const char* /*args*/,
    XUserHandle /*defaultUser*/) {
    RecordOk("XLaunchNewGame");
}

}  // extern "C"

namespace {

bool XgrGuidEquals(const XgrGuid& a, const XgrGuid& b) noexcept {
    return a.data1 == b.data1 &&
           a.data2 == b.data2 &&
           a.data3 == b.data3 &&
           std::memcmp(a.data4, b.data4, sizeof(a.data4)) == 0;
}

std::string FormatXgrGuid(const XgrGuid& g) {
    char buf[64]{};
    std::snprintf(
        buf, sizeof(buf),
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.data1,
        static_cast<unsigned>(g.data2),
        static_cast<unsigned>(g.data3),
        static_cast<unsigned>(g.data4[0]),
        static_cast<unsigned>(g.data4[1]),
        static_cast<unsigned>(g.data4[2]),
        static_cast<unsigned>(g.data4[3]),
        static_cast<unsigned>(g.data4[4]),
        static_cast<unsigned>(g.data4[5]),
        static_cast<unsigned>(g.data4[6]),
        static_cast<unsigned>(g.data4[7]));
    return std::string(buf);
}

enum XgrApiIndex : size_t {
    kApiTaskQueue = 0,
    kApiAsync = 1,
    kApiUser = 2,
    kApiGameSave = 3,
    kApiSystem = 4,
    kApiMem = 5,
    kApiPackage = 6,
    kApiDisplay = 7,
    kApiGameUi = 8,
    kApiGameInvite = 9,
    kApiAccessibility = 10,
    kApiSpeechSynthesizer = 11,
    kApiStore = 12,
    kApiNetworking = 13,
    kApiCoreRuntime = 14,
    kApiGameStreaming = 15,
    kApiCount = 16,
};

extern const IXgrTaskQueueApiVtable g_task_queue_vtable;
extern const IXgrAsyncApiVtable g_async_vtable;
extern const IXgrUserApiVtable g_user_vtable;
extern const IXgrGameSaveApiVtable g_game_save_vtable;
extern const IXgrSystemApiVtable g_system_vtable;
extern const IXgrMemApiVtable g_mem_vtable;
extern const IXgrPackageApiVtable g_package_vtable;
extern const IXgrDisplayApiVtable g_display_vtable;
extern const IXgrGameUiApiVtable g_game_ui_vtable;
extern const IXgrGameInviteApiVtable g_game_invite_vtable;
extern const IXgrAccessibilityApiVtable g_accessibility_vtable;
extern const IXgrSpeechSynthesizerApiVtable g_speech_synth_vtable;
extern const IXgrStoreApiVtable g_store_vtable;
extern const IXgrNetworkingApiVtable g_networking_vtable;
extern const IXgrCoreRuntimeApiVtable g_core_runtime_vtable;
extern const IXgrGameStreamingApiVtable g_game_streaming_vtable;

int FindApiIndexByVtablePtr(const void* self) noexcept {
    if (self == &g_task_queue_vtable) return static_cast<int>(kApiTaskQueue);
    if (self == &g_async_vtable) return static_cast<int>(kApiAsync);
    if (self == &g_user_vtable) return static_cast<int>(kApiUser);
    if (self == &g_game_save_vtable) return static_cast<int>(kApiGameSave);
    if (self == &g_system_vtable) return static_cast<int>(kApiSystem);
    if (self == &g_mem_vtable) return static_cast<int>(kApiMem);
    if (self == &g_package_vtable) return static_cast<int>(kApiPackage);
    if (self == &g_display_vtable) return static_cast<int>(kApiDisplay);
    if (self == &g_game_ui_vtable) return static_cast<int>(kApiGameUi);
    if (self == &g_game_invite_vtable) return static_cast<int>(kApiGameInvite);
    if (self == &g_accessibility_vtable) return static_cast<int>(kApiAccessibility);
    if (self == &g_speech_synth_vtable) return static_cast<int>(kApiSpeechSynthesizer);
    if (self == &g_store_vtable) return static_cast<int>(kApiStore);
    if (self == &g_networking_vtable) return static_cast<int>(kApiNetworking);
    if (self == &g_core_runtime_vtable) return static_cast<int>(kApiCoreRuntime);
    if (self == &g_game_streaming_vtable) return static_cast<int>(kApiGameStreaming);
    return -1;
}

uint32_t OPENSCARLETT_GDK_CALL XgrVtableAddRef(const void* self) {
    const int idx = FindApiIndexByVtablePtr(self);
    RecordOk("IXgrApiVtable::AddRef");
    if (idx < 0 || idx >= static_cast<int>(kApiCount)) {
        return 1u;
    }
    return GetRuntime().vtable_refs[idx].fetch_add(1u, std::memory_order_acq_rel) + 1u;
}

uint32_t OPENSCARLETT_GDK_CALL XgrVtableRelease(const void* self) {
    const int idx = FindApiIndexByVtablePtr(self);
    RecordOk("IXgrApiVtable::Release");
    if (idx < 0 || idx >= static_cast<int>(kApiCount)) {
        return 1u;
    }
    auto& ref = GetRuntime().vtable_refs[idx];
    uint32_t cur = ref.load(std::memory_order_acquire);
    while (cur > 1u) {
        if (ref.compare_exchange_weak(cur, cur - 1u, std::memory_order_acq_rel)) {
            return cur - 1u;
        }
    }
    return 1u;
}

XGameResult OPENSCARLETT_GDK_CALL XgrVtableQueryInterface(
    const void* self,
    const XgrGuid* riid,
    void** ppvObject) {
    if (!ppvObject) {
        return RecordHr("IXgrApiVtable::QueryInterface", XGR_E_POINTER);
    }
    *ppvObject = nullptr;
    if (!riid) {
        return RecordHr("IXgrApiVtable::QueryInterface", XGR_E_INVALIDARG);
    }
    if (XgrGuidEquals(*riid, XGR_IID_IUNKNOWN) && FindApiIndexByVtablePtr(self) >= 0) {
        XgrVtableAddRef(self);
        *ppvObject = const_cast<void*>(self);
        return RecordHr("IXgrApiVtable::QueryInterface", XGR_S_OK);
    }
    return QueryApiImpl(riid, ppvObject);
}

const IXgrTaskQueueApiVtable g_task_queue_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XTaskQueueCreate,
    &XTaskQueueDuplicateHandle,
    &XTaskQueueCloseHandle,
    &XTaskQueueGetCurrentProcessTaskQueue,
    &XTaskQueueSetCurrentProcessTaskQueue,
    &XTaskQueueSubmitCallback,
    &XTaskQueueSubmitDelayedCallback,
    &XTaskQueueDispatch,
    &XTaskQueueTerminate,
    &XTaskQueueRegisterMonitor,
    &XTaskQueueUnregisterMonitor,
    &XTaskQueueGetPort,
    &XTaskQueueCreateComposite,
    &XTaskQueueRegisterWaiter,
    &XTaskQueueUnregisterWaiter,
};

const IXgrAsyncApiVtable g_async_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XAsyncBegin,
    &XAsyncSchedule,
    &XAsyncComplete,
    &XAsyncGetStatus,
    &XAsyncGetResultSize,
    &XAsyncGetResult,
    &XAsyncCancel,
    &XAsyncRun,
};

const IXgrUserApiVtable g_user_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XUserAddAsync,
    &XUserAddResult,
    &XUserDuplicateHandle,
    &XUserCloseHandle,
    &XUserGetId,
    &XUserGetLocalId,
    &XUserGetGamertag,
    &XUserGetState,
    &XUserGetAgeGroup,
    &XUserCheckPrivilege,
    &XUserGetMaxUsers,
    &XUserCompare,
    &XUserGetGamerPictureAsync,
    &XUserGetGamerPictureResultSize,
    &XUserGetGamerPictureResult,
    &XUserGetIsGuest,
    &XUserFindUserById,
    &XUserFindUserByLocalId,
    &XUserFindForDevice,
    &XUserGetTokenAndSignatureAsync,
    &XUserGetTokenAndSignatureResultSize,
    &XUserGetTokenAndSignatureResult,
    &XUserGetTokenAndSignatureUtf16Async,
    &XUserGetTokenAndSignatureUtf16ResultSize,
    &XUserGetTokenAndSignatureUtf16Result,
    &XUserGetDefaultAudioEndpointUtf16,
    &XUserGetSignOutDeferral,
    &XUserCloseSignOutDeferralHandle,
    &XUserRegisterForChangeEvent,
    &XUserUnregisterForChangeEvent,
    &XUserRegisterForDefaultAudioEndpointUtf16Changed,
    &XUserUnregisterForDefaultAudioEndpointUtf16Changed,
    &XUserRegisterForDeviceAssociationChanged,
    &XUserUnregisterForDeviceAssociationChanged,
    &XUserResolveIssueWithUiAsync,
    &XUserResolveIssueWithUiResult,
    &XUserResolveIssueWithUiUtf16Async,
    &XUserResolveIssueWithUiUtf16Result,
    &XUserResolvePrivilegeWithUiAsync,
    &XUserResolvePrivilegeWithUiResult,
};

const IXgrGameSaveApiVtable g_game_save_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XGameSaveInitializeProvider,
    &XGameSaveInitializeProviderAsync,
    &XGameSaveInitializeProviderResult,
    &XGameSaveCloseProvider,
    &XGameSaveCreateContainer,
    &XGameSaveCloseContainer,
    &XGameSaveCreateUpdate,
    &XGameSaveSubmitBlobWrite,
    &XGameSaveSubmitBlobDelete,
    &XGameSaveSubmitUpdate,
    &XGameSaveCloseUpdate,
    &XGameSaveReadBlobData,
    &XGameSaveEnumerateContainerInfo,
    &XGameSaveEnumerateBlobInfo,
    &XGameSaveDeleteContainer,
    &XGameSaveGetRemainingQuota,
    &XGameSaveEnumerateContainerInfoByName,
    &XGameSaveEnumerateBlobInfoByName,
    &XGameSaveGetContainerInfo,
    &XGameSaveReadBlobDataAsync,
    &XGameSaveReadBlobDataResult,
    &XGameSaveSubmitUpdateAsync,
    &XGameSaveSubmitUpdateResult,
    &XGameSaveDeleteContainerAsync,
    &XGameSaveDeleteContainerResult,
    &XGameSaveGetRemainingQuotaAsync,
    &XGameSaveGetRemainingQuotaResult,
};

const IXgrSystemApiVtable g_system_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XGameRuntimeInitialize,
    &XGameRuntimeUninitialize,
    &XGameGetXboxTitleId,
    &XSystemGetDeviceType,
    &XSystemGetConsoleId,
    &XSystemGetXboxLiveSandboxId,
    &XSystemGetAnalyticsInfo,
    &XSystemGetRuntimeInfo,
    &XSystemIsHandleValid,
    &XGameRuntimeIsFeatureAvailable,
    &XSystemGetAppSpecificDeviceId,
};

const IXgrMemApiVtable g_mem_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XMemAllocDefault,
    &XMemFreeDefault,
    &XMemAlloc,
    &XMemFree,
    &XMemSetAllocationHooks,
};

const IXgrPackageApiVtable g_package_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XPackageGetCurrentProcessPackageIdentifier,
    &XPackageIsPackagedProcess,
    &XPackageEnumerateChunkAvailability,
    &XPackageMount,
    &XPackageCloseMountHandle,
    &XPackageGetMountPathSize,
    &XPackageGetMountPath,
    &XPackageGetUserLocale,
    &XPackageGetWriteStats,
    &XPackageCreateInstallationMonitor,
    &XPackageCloseInstallationMonitorHandle,
    &XPackageGetInstallationProgress,
    &XPackageUpdateInstallationMonitor,
    &XPackageRegisterInstallationProgressChanged,
    &XPackageUnregisterInstallationProgressChanged,
    &XPackageRegisterPackageInstalled,
    &XPackageUnregisterPackageInstalled,
    &XPackageEnumeratePackages,
    &XPackageEnumerateFeatures,
    &XPackageFindChunkAvailability,
    &XPackageEstimateDownloadSize,
    &XPackageChangeChunkInstallOrder,
    &XPackageInstallChunks,
    &XPackageInstallChunksAsync,
    &XPackageInstallChunksResult,
    &XPackageUninstallChunks,
    &XPackageUninstallUWPInstance,
};

const IXgrDisplayApiVtable g_display_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XDisplayTryEnableHdrMode,
    &XDisplayTryAcquireTimeoutDeferral,
    &XDisplayCloseTimeoutDeferralHandle,
    &XDisplayAcquireTimeoutDeferral,
};

const IXgrStoreApiVtable g_store_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XStoreCreateContext,
    &XStoreCloseContextHandle,
    &XStoreCloseLicenseHandle,
    &XStoreCloseProductsQueryHandle,
    &XStoreIsLicenseValid,
    &XStoreQueryGameLicenseAsync,
    &XStoreQueryGameLicenseResult,
    &XStoreRegisterGameLicenseChanged,
    &XStoreUnregisterGameLicenseChanged,
    &XStoreRegisterPackageLicenseLost,
    &XStoreUnregisterPackageLicenseLost,
    &XStoreQueryAddOnLicensesAsync,
    &XStoreQueryAddOnLicensesResultCount,
    &XStoreQueryAddOnLicensesResult,
    &XStoreQueryProductForCurrentGameAsync,
    &XStoreQueryProductForCurrentGameResult,
    &XStoreQueryProductForPackageAsync,
    &XStoreQueryProductForPackageResult,
    &XStoreQueryAssociatedProductsAsync,
    &XStoreQueryAssociatedProductsResult,
    &XStoreQueryEntitledProductsAsync,
    &XStoreQueryEntitledProductsResult,
    &XStoreQueryProductsAsync,
    &XStoreQueryProductsResult,
    &XStoreEnumerateProductsQuery,
    &XStoreProductsQueryHasMorePages,
    &XStoreProductsQueryNextPageAsync,
    &XStoreProductsQueryNextPageResult,
    &XStoreCanAcquireLicenseForPackageAsync,
    &XStoreCanAcquireLicenseForPackageResult,
    &XStoreCanAcquireLicenseForStoreIdAsync,
    &XStoreCanAcquireLicenseForStoreIdResult,
    &XStoreAcquireLicenseForPackageAsync,
    &XStoreAcquireLicenseForPackageResult,
    &XStoreAcquireLicenseForDurablesAsync,
    &XStoreAcquireLicenseForDurablesResult,
    &XStoreQueryConsumableBalanceRemainingAsync,
    &XStoreQueryConsumableBalanceRemainingResult,
    &XStoreReportConsumableFulfillmentAsync,
    &XStoreReportConsumableFulfillmentResult,
    &XStoreQueryGameAndDlcPackageUpdatesAsync,
    &XStoreQueryGameAndDlcPackageUpdatesResultCount,
    &XStoreQueryGameAndDlcPackageUpdatesResult,
    &XStoreDownloadPackageUpdatesAsync,
    &XStoreDownloadPackageUpdatesResult,
    &XStoreDownloadAndInstallPackageUpdatesAsync,
    &XStoreDownloadAndInstallPackageUpdatesResult,
    &XStoreDownloadAndInstallPackagesAsync,
    &XStoreDownloadAndInstallPackagesResultCount,
    &XStoreDownloadAndInstallPackagesResult,
    &XStoreGetUserCollectionsIdAsync,
    &XStoreGetUserCollectionsIdResultSize,
    &XStoreGetUserCollectionsIdResult,
    &XStoreGetUserPurchaseIdAsync,
    &XStoreGetUserPurchaseIdResultSize,
    &XStoreGetUserPurchaseIdResult,
    &XStoreQueryLicenseTokenAsync,
    &XStoreQueryLicenseTokenResultSize,
    &XStoreQueryLicenseTokenResult,
    &XStoreQueryPackageIdentifier,
    &XStoreIsAvailabilityPurchasable,
    &XStoreShowAssociatedProductsUIAsync,
    &XStoreShowAssociatedProductsUIResult,
    &XStoreShowProductPageUIAsync,
    &XStoreShowProductPageUIResult,
    &XStoreShowPurchaseUIAsync,
    &XStoreShowPurchaseUIResult,
    &XStoreShowRateAndReviewUIAsync,
    &XStoreShowRateAndReviewUIResult,
    &XStoreShowRedeemTokenUIAsync,
    &XStoreShowRedeemTokenUIResult,
};

const IXgrGameUiApiVtable g_game_ui_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XLaunchUri,
    &XGameUIShowMessageDialogAsync,
    &XGameUIShowMessageDialogResult,
    &XGameUIShowTextEntryAsync,
    &XGameUIShowTextEntryResultSize,
    &XGameUIShowTextEntryResult,
    &XGameUIShowPlayerPickerAsync,
    &XGameUIShowPlayerPickerResultCount,
    &XGameUIShowPlayerPickerResult,
    &XGameUIShowErrorDialogAsync,
    &XGameUIShowErrorDialogResult,
    &XGameUISetNotificationPositionHint,
    &XGameUiSetNotificationPositionHint,
    &XGameUiShowMessageDialogAsync,
    &XGameUiShowMessageDialogResult,
    &XGameUiShowErrorDialogAsync,
    &XGameUiShowErrorDialogResult,
    &XGameUiShowTextEntryAsync,
    &XGameUiShowTextEntryResultSize,
    &XGameUiShowTextEntryResult,
    &XGameUiShowAchievementsAsync,
    &XGameUiShowAchievementsResult,
    &XGameUiShowPlayerPickerAsync,
    &XGameUiShowPlayerPickerResultCount,
    &XGameUiShowPlayerPickerResult,
    &XGameUiShowPlayerProfileCardAsync,
    &XGameUiShowPlayerProfileCardResult,
    &XGameUiShowSendGameInviteAsync,
    &XGameUiShowSendGameInviteResult,
    &XGameUiShowWebAuthenticationAsync,
    &XGameUiShowWebAuthenticationWithOptionsAsync,
    &XGameUiShowWebAuthenticationResultSize,
    &XGameUiShowWebAuthenticationResult,
};

const IXgrGameInviteApiVtable g_game_invite_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XGameInviteRegisterForEvent,
    &XGameInviteUnregisterForEvent,
};

const IXgrAccessibilityApiVtable g_accessibility_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XClosedCaptionGetProperties,
    &XClosedCaptionSetEnabled,
    &XHighContrastGetMode,
};

const IXgrSpeechSynthesizerApiVtable g_speech_synth_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XSpeechSynthesizerCreate,
    &XSpeechSynthesizerCloseHandle,
    &XSpeechSynthesizerEnumerateInstalledVoices,
    &XSpeechSynthesizerSetDefaultVoice,
    &XSpeechSynthesizerCreateStreamFromText,
    &XSpeechSynthesizerGetStreamDataSize,
    &XSpeechSynthesizerGetStreamData,
    &XSpeechSynthesizerCloseStreamHandle,
};

const IXgrNetworkingApiVtable g_networking_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XNetworkingGetConnectivityHint,
    &XNetworkingQueryConnectivityHint,
    &XNetworkingQueryPreferredLocalUdpMultiplayerPort,
    &XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync,
    &XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult,
    &XNetworkingRegisterConnectivityHintChanged,
    &XNetworkingUnregisterConnectivityHintChanged,
    &XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged,
    &XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged,
    &XNetworkingQuerySecurityInformationForUrlAsync,
    &XNetworkingQuerySecurityInformationForUrlAsyncResultSize,
    &XNetworkingQuerySecurityInformationForUrlAsyncResult,
    &XNetworkingQuerySecurityInformationForUrlUtf16Async,
    &XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize,
    &XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult,
    &XNetworkingVerifyServerCertificate,
};

const IXgrCoreRuntimeApiVtable g_core_runtime_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XGameRuntimeIsFeatureAvailable,
    &XPersistentLocalStorageGetPathSize,
    &XPersistentLocalStorageGetPath,
    &XThreadIsTimeSensitive,
    &XThreadSetTimeSensitive,
    &XThreadAssertNotTimeSensitive,
    &XThreadVerifyNotTimeSensitive,
    &XErrorReport,
    &XErrorSetCallback,
    &XErrorSetOptions,
    &XGameEventWrite,
    &XGameInviteRegisterForEvent,
    &XGameInviteUnregisterForEvent,
    &XLaunchNewGame,
    &XLaunchUri,
    &XHighContrastGetMode,
    &XClosedCaptionGetProperties,
    &XClosedCaptionSetEnabled,
};

const IXgrGameStreamingApiVtable g_game_streaming_vtable = {
    &XgrVtableQueryInterface,
    &XgrVtableAddRef,
    &XgrVtableRelease,
    &XGameStreamingInitialize,
    &XGameStreamingUninitialize,
    &XGameStreamingIsStreaming,
    &XGameStreamingGetClientCount,
    &XGameStreamingGetClients,
    &XGameStreamingGetConnectionState,
    &XGameStreamingRegisterConnectionStateChanged,
    &XGameStreamingUnregisterConnectionStateChanged,
    &XGameStreamingHideTouchControls,
    &XGameStreamingShowTouchControlLayout,
    &XGameStreamingGetStreamPhysicalDimensions,
    &XGameStreamingGetServerLocationNameSize,
    &XGameStreamingGetServerLocationName,
    &XGameStreamingGetLastFrameDisplayed,
    &XGameStreamingIsTouchInputEnabledOnClient,
};

// ===========================================================================
// Real Microsoft XGamingRuntimeThunks.dll 3-arg QueryApiImpl COM ABI Tables
// All 24 (CLSID, IID) interfaces & 288 COM vtable slots verified against
// shipping XGamingRuntimeThunks.dll.
// ===========================================================================
using ComVtableSlot = void (*)(void);

template <auto Fn>
struct ComThunk;

template <typename Ret, typename... Args, Ret(OPENSCARLETT_GDK_CALL* Fn)(Args...)>
struct ComThunk<Fn> {
    static Ret OPENSCARLETT_GDK_CALL Call(void* /*self*/, Args... args) {
        return Fn(args...);
    }
};

XGameResult OPENSCARLETT_GDK_CALL ComThunk_Unimpl(void* /*self*/) {
    return RecordHr("IXgrComApi::UnimplSlot", XGR_E_NOTIMPL);
}

// Win64 COM virtual method ABI for >8-byte struct return (XSystemAnalyticsInfo = 144 bytes):
// RCX = this, RDX = hidden return buffer, returns RDX in RAX.
XSystemAnalyticsInfo* OPENSCARLETT_GDK_CALL ComThunk_XSystemGetAnalyticsInfo(
    void* /*self*/,
    XSystemAnalyticsInfo* outRet) {
    if (outRet) {
        *outRet = XSystemGetAnalyticsInfo();
    }
    return outRet;
}

#define OS_COM_SLOT(fn) reinterpret_cast<ComVtableSlot>(&ComThunk<&fn>::Call)
#define OS_COM_UNIMPL() reinterpret_cast<ComVtableSlot>(&ComThunk_Unimpl)

struct XgrRealComObject {
    const ComVtableSlot* lpVtbl;
    uint32_t com_index;
};

enum RealComObjIndex : uint32_t {
    kComUser = 0,
    kComUserGamertag = 1,
    kComDisplayHdr = 2,
    kComGameInvite = 3,
    kComAsyncTaskQueue = 4,
    kComStore = 5,
    kComAppBroadcastMeta = 6,
    kComLaunchDisplay = 7,
    kComNetworking = 8,
    kComAccessibilitySpeech = 9,
    kComGameSave = 10,
    kComUserDeviceAudio = 11,
    kComRuntimeFeature = 12,
    kComError = 13,
    kComGameTitle = 14,
    kComAppCapture = 15,
    kComPackage = 16,
    kComSystemAnalytics = 17,
    kComGameEvent = 18,
    kComGameUi = 19,
    kComSystemConsole = 20,
    kComPls = 21,
    kComObjCount = 22,
};

// Forward declare COM lookup so RealComQueryInterface can cross-QI on multi-IID classes.
XgrRealComObject* LookupRealComInterface(const XgrGuid& clsid, const XgrGuid& iid) noexcept;
const XgrGuid& GetRealComClsidByIndex(uint32_t idx) noexcept;

std::atomic<uint32_t> g_real_com_refs[kComObjCount]{
    1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
    1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};

uint32_t OPENSCARLETT_GDK_CALL RealComAddRef(void* self) {
    RecordOk("IXgrComApi::AddRef");
    const auto* obj = static_cast<const XgrRealComObject*>(self);
    if (!obj || obj->com_index >= kComObjCount) {
        return 1u;
    }
    return g_real_com_refs[obj->com_index].fetch_add(1u, std::memory_order_acq_rel) + 1u;
}

uint32_t OPENSCARLETT_GDK_CALL RealComRelease(void* self) {
    RecordOk("IXgrComApi::Release");
    const auto* obj = static_cast<const XgrRealComObject*>(self);
    if (!obj || obj->com_index >= kComObjCount) {
        return 1u;
    }
    auto& ref = g_real_com_refs[obj->com_index];
    uint32_t cur = ref.load(std::memory_order_acquire);
    while (cur > 1u) {
        if (ref.compare_exchange_weak(cur, cur - 1u, std::memory_order_acq_rel)) {
            return cur - 1u;
        }
    }
    return 1u;
}

XGameResult OPENSCARLETT_GDK_CALL RealComQueryInterface(
    void* self,
    const XgrGuid* riid,
    void** ppvObject) {
    if (!ppvObject) {
        return RecordHr("IXgrComApi::QueryInterface", XGR_E_POINTER);
    }
    *ppvObject = nullptr;
    const auto* obj = static_cast<const XgrRealComObject*>(self);
    if (!obj || obj->com_index >= kComObjCount || !riid) {
        return RecordHr("IXgrComApi::QueryInterface", XGR_E_INVALIDARG);
    }
    if (XgrGuidEquals(*riid, XGR_IID_IUNKNOWN)) {
        RealComAddRef(self);
        *ppvObject = self;
        return RecordHr("IXgrComApi::QueryInterface", XGR_S_OK);
    }
    XgrRealComObject* target = LookupRealComInterface(GetRealComClsidByIndex(obj->com_index), *riid);
    if (!target) {
        return RecordHr("IXgrComApi::QueryInterface", XGR_E_NOINTERFACE);
    }
    RealComAddRef(target);
    *ppvObject = target;
    return RecordHr("IXgrComApi::QueryInterface", XGR_S_OK);
}

#define OS_COM_IUNKNOWN_SLOTS \
    reinterpret_cast<ComVtableSlot>(&RealComQueryInterface), \
    reinterpret_cast<ComVtableSlot>(&RealComAddRef), \
    reinterpret_cast<ComVtableSlot>(&RealComRelease)

// 1. CLSID={01ACD177-91F9-4763-A38E-CCBB55CE32E0} IID={1BF2F8C5-D507-4E52-BB05-F726D0E71161}
const ComVtableSlot g_com_vtbl_user[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XUserDuplicateHandle),
    /*  4 */ OS_COM_SLOT(XUserCloseHandle),
    /*  5 */ OS_COM_SLOT(XUserCompare),
    /*  6 */ OS_COM_SLOT(XUserGetMaxUsers),
    /*  7 */ OS_COM_SLOT(XUserAddAsync),
    /*  8 */ OS_COM_SLOT(XUserAddResult),
    /*  9 */ OS_COM_SLOT(XUserGetLocalId),
    /* 10 */ OS_COM_SLOT(XUserFindUserByLocalId),
    /* 11 */ OS_COM_SLOT(XUserGetId),
    /* 12 */ OS_COM_SLOT(XUserFindUserById),
    /* 13 */ OS_COM_SLOT(XUserGetIsGuest),
    /* 14 */ OS_COM_SLOT(XUserGetState),
    /* 15 */ OS_COM_UNIMPL(),
    /* 16 */ OS_COM_SLOT(XUserGetGamerPictureAsync),
    /* 17 */ OS_COM_SLOT(XUserGetGamerPictureResultSize),
    /* 18 */ OS_COM_SLOT(XUserGetGamerPictureResult),
    /* 19 */ OS_COM_SLOT(XUserGetAgeGroup),
    /* 20 */ OS_COM_SLOT(XUserCheckPrivilege),
    /* 21 */ OS_COM_SLOT(XUserResolvePrivilegeWithUiAsync),
    /* 22 */ OS_COM_SLOT(XUserResolvePrivilegeWithUiResult),
    /* 23 */ OS_COM_SLOT(XUserGetTokenAndSignatureAsync),
    /* 24 */ OS_COM_SLOT(XUserGetTokenAndSignatureResultSize),
    /* 25 */ OS_COM_SLOT(XUserGetTokenAndSignatureResult),
    /* 26 */ OS_COM_SLOT(XUserGetTokenAndSignatureUtf16Async),
    /* 27 */ OS_COM_SLOT(XUserGetTokenAndSignatureUtf16ResultSize),
    /* 28 */ OS_COM_SLOT(XUserGetTokenAndSignatureUtf16Result),
    /* 29 */ OS_COM_SLOT(XUserResolveIssueWithUiAsync),
    /* 30 */ OS_COM_SLOT(XUserResolveIssueWithUiResult),
    /* 31 */ OS_COM_SLOT(XUserResolveIssueWithUiUtf16Async),
    /* 32 */ OS_COM_SLOT(XUserResolveIssueWithUiUtf16Result),
    /* 33 */ OS_COM_SLOT(XUserRegisterForChangeEvent),
    /* 34 */ OS_COM_SLOT(XUserUnregisterForChangeEvent),
    /* 35 */ OS_COM_SLOT(XUserGetSignOutDeferral),
    /* 36 */ OS_COM_SLOT(XUserCloseSignOutDeferralHandle),
};

// 2. CLSID={01ACD177-91F9-4763-A38E-CCBB55CE32E0} IID={CEF4FAC0-7676-4A94-A119-4C43F9EB5B74}
const ComVtableSlot g_com_vtbl_user_gamertag[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XUserGetGamertag),
};

// 3. CLSID={03F0FE74-FDD9-4E5C-B630-F9339C47ACC5} IID={35F07670-706E-4BFB-9476-090798C5EBF3}
const ComVtableSlot g_com_vtbl_display_hdr[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_UNIMPL(),
    /*  4 */ OS_COM_SLOT(XDisplayTryEnableHdrMode),
};

// 4. CLSID={0651AAE2-4012-4077-BF84-8B9097090E2C} IID={0651AAE2-4012-4077-BF84-8B9097090E2C}
const ComVtableSlot g_com_vtbl_game_invite[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameInviteRegisterForEvent),
    /*  4 */ OS_COM_SLOT(XGameInviteUnregisterForEvent),
};

// 5. CLSID={073B7DCB-1FCF-4030-94BE-E3C9EB623428} IID={073B7DCB-1FCF-4030-94BE-E3C9EB623428}
const ComVtableSlot g_com_vtbl_async_taskqueue[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XAsyncGetStatus),
    /*  4 */ OS_COM_SLOT(XAsyncGetResultSize),
    /*  5 */ OS_COM_SLOT(XAsyncCancel),
    /*  6 */ OS_COM_SLOT(XAsyncRun),
    /*  7 */ OS_COM_SLOT(XAsyncBegin),
    /*  8 */ OS_COM_UNIMPL(),
    /*  9 */ OS_COM_SLOT(XAsyncSchedule),
    /* 10 */ OS_COM_SLOT(XAsyncComplete),
    /* 11 */ OS_COM_SLOT(XAsyncGetResult),
    /* 12 */ OS_COM_SLOT(XTaskQueueCreate),
    /* 13 */ OS_COM_SLOT(XTaskQueueCreateComposite),
    /* 14 */ OS_COM_SLOT(XTaskQueueGetPort),
    /* 15 */ OS_COM_SLOT(XTaskQueueDuplicateHandle),
    /* 16 */ OS_COM_SLOT(XTaskQueueDispatch),
    /* 17 */ OS_COM_SLOT(XTaskQueueCloseHandle),
    /* 18 */ OS_COM_SLOT(XTaskQueueSubmitCallback),
    /* 19 */ OS_COM_SLOT(XTaskQueueSubmitDelayedCallback),
    /* 20 */ OS_COM_SLOT(XTaskQueueRegisterWaiter),
    /* 21 */ OS_COM_SLOT(XTaskQueueUnregisterWaiter),
    /* 22 */ OS_COM_SLOT(XTaskQueueTerminate),
    /* 23 */ OS_COM_SLOT(XTaskQueueRegisterMonitor),
    /* 24 */ OS_COM_SLOT(XTaskQueueUnregisterMonitor),
    /* 25 */ OS_COM_SLOT(XTaskQueueGetCurrentProcessTaskQueue),
    /* 26 */ OS_COM_SLOT(XTaskQueueSetCurrentProcessTaskQueue),
    /* 27 */ OS_COM_SLOT(XThreadSetTimeSensitive),
    /* 28 */ OS_COM_SLOT(XThreadVerifyNotTimeSensitive),
    /* 29 */ OS_COM_SLOT(XThreadAssertNotTimeSensitive),
    /* 30 */ OS_COM_SLOT(XThreadIsTimeSensitive),
};

// 6. CLSID={0DD112AC-7C24-448C-B92B-3960FB5BD30C}
//    IIDs={DE3DBDD4-0B37-4BDB-A10E-ACF3A354D06A}, {60B09F4E-1B85-45B1-826C-169118E230E1},
//         {2D42FEA5-E71D-4B76-97CD-C50AFBB3AE5D}
const ComVtableSlot g_com_vtbl_store[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XStoreCreateContext),
    /*  4 */ OS_COM_SLOT(XStoreCloseContextHandle),
    /*  5 */ OS_COM_SLOT(XStoreQueryAssociatedProductsAsync),
    /*  6 */ OS_COM_SLOT(XStoreQueryAssociatedProductsResult),
    /*  7 */ OS_COM_SLOT(XStoreQueryProductsAsync),
    /*  8 */ OS_COM_SLOT(XStoreQueryProductsResult),
    /*  9 */ OS_COM_SLOT(XStoreQueryEntitledProductsAsync),
    /* 10 */ OS_COM_SLOT(XStoreQueryEntitledProductsResult),
    /* 11 */ OS_COM_SLOT(XStoreQueryProductForCurrentGameAsync),
    /* 12 */ OS_COM_SLOT(XStoreQueryProductForCurrentGameResult),
    /* 13 */ OS_COM_SLOT(XStoreQueryProductForPackageAsync),
    /* 14 */ OS_COM_SLOT(XStoreQueryProductForPackageResult),
    /* 15 */ OS_COM_SLOT(XStoreEnumerateProductsQuery),
    /* 16 */ OS_COM_SLOT(XStoreProductsQueryHasMorePages),
    /* 17 */ OS_COM_SLOT(XStoreProductsQueryNextPageAsync),
    /* 18 */ OS_COM_SLOT(XStoreProductsQueryNextPageResult),
    /* 19 */ OS_COM_SLOT(XStoreCloseProductsQueryHandle),
    /* 20 */ OS_COM_SLOT(XStoreAcquireLicenseForPackageAsync),
    /* 21 */ OS_COM_SLOT(XStoreAcquireLicenseForPackageResult),
    /* 22 */ OS_COM_SLOT(XStoreIsLicenseValid),
    /* 23 */ OS_COM_SLOT(XStoreCloseLicenseHandle),
    /* 24 */ OS_COM_SLOT(XStoreCanAcquireLicenseForStoreIdAsync),
    /* 25 */ OS_COM_SLOT(XStoreCanAcquireLicenseForStoreIdResult),
    /* 26 */ OS_COM_SLOT(XStoreCanAcquireLicenseForPackageAsync),
    /* 27 */ OS_COM_SLOT(XStoreCanAcquireLicenseForPackageResult),
    /* 28 */ OS_COM_SLOT(XStoreQueryGameLicenseAsync),
    /* 29 */ OS_COM_SLOT(XStoreQueryGameLicenseResult),
    /* 30 */ OS_COM_SLOT(XStoreQueryAddOnLicensesAsync),
    /* 31 */ OS_COM_SLOT(XStoreQueryAddOnLicensesResultCount),
    /* 32 */ OS_COM_SLOT(XStoreQueryAddOnLicensesResult),
    /* 33 */ OS_COM_SLOT(XStoreQueryConsumableBalanceRemainingAsync),
    /* 34 */ OS_COM_SLOT(XStoreQueryConsumableBalanceRemainingResult),
    /* 35 */ OS_COM_SLOT(XStoreReportConsumableFulfillmentAsync),
    /* 36 */ OS_COM_SLOT(XStoreReportConsumableFulfillmentResult),
    /* 37 */ OS_COM_SLOT(XStoreGetUserCollectionsIdAsync),
    /* 38 */ OS_COM_SLOT(XStoreGetUserCollectionsIdResultSize),
    /* 39 */ OS_COM_SLOT(XStoreGetUserCollectionsIdResult),
    /* 40 */ OS_COM_SLOT(XStoreGetUserPurchaseIdAsync),
    /* 41 */ OS_COM_SLOT(XStoreGetUserPurchaseIdResultSize),
    /* 42 */ OS_COM_SLOT(XStoreGetUserPurchaseIdResult),
    /* 43 */ OS_COM_SLOT(XStoreQueryLicenseTokenAsync),
    /* 44 */ OS_COM_SLOT(XStoreQueryLicenseTokenResultSize),
    /* 45 */ OS_COM_SLOT(XStoreQueryLicenseTokenResult),
    /* 46 */ OS_COM_UNIMPL(),
    /* 47 */ OS_COM_UNIMPL(),
    /* 48 */ OS_COM_UNIMPL(),
    /* 49 */ OS_COM_SLOT(XStoreShowPurchaseUIAsync),
    /* 50 */ OS_COM_SLOT(XStoreShowPurchaseUIResult),
    /* 51 */ OS_COM_SLOT(XStoreShowRateAndReviewUIAsync),
    /* 52 */ OS_COM_SLOT(XStoreShowRateAndReviewUIResult),
    /* 53 */ OS_COM_SLOT(XStoreShowRedeemTokenUIAsync),
    /* 54 */ OS_COM_SLOT(XStoreShowRedeemTokenUIResult),
    /* 55 */ OS_COM_SLOT(XStoreQueryGameAndDlcPackageUpdatesAsync),
    /* 56 */ OS_COM_SLOT(XStoreQueryGameAndDlcPackageUpdatesResultCount),
    /* 57 */ OS_COM_SLOT(XStoreQueryGameAndDlcPackageUpdatesResult),
    /* 58 */ OS_COM_SLOT(XStoreDownloadPackageUpdatesAsync),
    /* 59 */ OS_COM_SLOT(XStoreDownloadPackageUpdatesResult),
    /* 60 */ OS_COM_SLOT(XStoreDownloadAndInstallPackageUpdatesAsync),
    /* 61 */ OS_COM_SLOT(XStoreDownloadAndInstallPackageUpdatesResult),
    /* 62 */ OS_COM_SLOT(XStoreDownloadAndInstallPackagesAsync),
    /* 63 */ OS_COM_SLOT(XStoreDownloadAndInstallPackagesResultCount),
    /* 64 */ OS_COM_SLOT(XStoreDownloadAndInstallPackagesResult),
    /* 65 */ OS_COM_SLOT(XStoreQueryPackageIdentifier),
    /* 66 */ OS_COM_SLOT(XStoreRegisterGameLicenseChanged),
    /* 67 */ OS_COM_SLOT(XStoreUnregisterGameLicenseChanged),
    /* 68 */ OS_COM_SLOT(XStoreRegisterPackageLicenseLost),
    /* 69 */ OS_COM_SLOT(XStoreUnregisterPackageLicenseLost),
    /* 70 */ OS_COM_SLOT(XStoreIsAvailabilityPurchasable),
    /* 71 */ OS_COM_SLOT(XStoreAcquireLicenseForDurablesAsync),
    /* 72 */ OS_COM_SLOT(XStoreAcquireLicenseForDurablesResult),
    /* 73 */ OS_COM_SLOT(XStoreShowAssociatedProductsUIAsync),
    /* 74 */ OS_COM_SLOT(XStoreShowAssociatedProductsUIResult),
    /* 75 */ OS_COM_SLOT(XStoreShowProductPageUIAsync),
    /* 76 */ OS_COM_SLOT(XStoreShowProductPageUIResult),
};

// 7. CLSID={186D5592-A72D-45FB-9560-11AED0E6647A} IID={186D5592-A72D-45FB-9560-11AED0E6647A}
const ComVtableSlot g_com_vtbl_app_broadcast_meta[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XAppBroadcastIsAppBroadcasting),
    /*  4 */ OS_COM_SLOT(XAppBroadcastShowUI),
    /*  5 */ OS_COM_SLOT(XAppBroadcastGetStatus),
    /*  6 */ OS_COM_SLOT(XAppBroadcastRegisterIsAppBroadcastingChanged),
    /*  7 */ OS_COM_SLOT(XAppBroadcastUnregisterIsAppBroadcastingChanged),
    /*  8 */ OS_COM_SLOT(XAppCaptureMetadataAddStringEvent),
    /*  9 */ OS_COM_SLOT(XAppCaptureMetadataAddInt32Event),
    /* 10 */ OS_COM_SLOT(XAppCaptureMetadataAddDoubleEvent),
    /* 11 */ OS_COM_SLOT(XAppCaptureMetadataStartStringState),
    /* 12 */ OS_COM_SLOT(XAppCaptureMetadataStartInt32State),
    /* 13 */ OS_COM_SLOT(XAppCaptureMetadataStartDoubleState),
    /* 14 */ OS_COM_SLOT(XAppCaptureMetadataStopState),
    /* 15 */ OS_COM_SLOT(XAppCaptureMetadataStopAllStates),
    /* 16 */ OS_COM_SLOT(XAppCaptureMetadataRemainingStorageBytesAvailable),
    /* 17 */ OS_COM_SLOT(XAppCaptureRegisterMetadataPurged),
    /* 18 */ OS_COM_SLOT(XAppCaptureUnRegisterMetadataPurged),
};

// 8. CLSID={1B339674-328D-4283-A200-3171F18D3639} IID={1B339674-328D-4283-A200-3171F18D3639}
const ComVtableSlot g_com_vtbl_launch_display[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XLaunchUri),
    /*  4 */ OS_COM_SLOT(XDisplayAcquireTimeoutDeferral),
    /*  5 */ OS_COM_SLOT(XDisplayCloseTimeoutDeferralHandle),
};

// 9. CLSID={37E56907-2F10-41E8-B72F-36EDB185331A} IID={37E56907-2F10-41E8-B72F-36EDB185331A}
const ComVtableSlot g_com_vtbl_networking[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XNetworkingQueryPreferredLocalUdpMultiplayerPort),
    /*  4 */ OS_COM_SLOT(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync),
    /*  5 */ OS_COM_SLOT(XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult),
    /*  6 */ OS_COM_SLOT(XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged),
    /*  7 */ OS_COM_SLOT(XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged),
    /*  8 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlAsync),
    /*  9 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlAsyncResultSize),
    /* 10 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlAsyncResult),
    /* 11 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlUtf16Async),
    /* 12 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize),
    /* 13 */ OS_COM_SLOT(XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult),
    /* 14 */ OS_COM_SLOT(XNetworkingVerifyServerCertificate),
    /* 15 */ OS_COM_SLOT(XNetworkingGetConnectivityHint),
    /* 16 */ OS_COM_SLOT(XNetworkingRegisterConnectivityHintChanged),
    /* 17 */ OS_COM_SLOT(XNetworkingUnregisterConnectivityHintChanged),
};

// 10. CLSID={3E241977-6237-41E9-8559-102C2D9983F1} IID={D722B373-8C4D-4692-9E51-D6AD9B37AA7D}
const ComVtableSlot g_com_vtbl_accessibility_speech[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XClosedCaptionGetProperties),
    /*  4 */ OS_COM_SLOT(XClosedCaptionSetEnabled),
    /*  5 */ OS_COM_SLOT(XHighContrastGetMode),
    /*  6 */ OS_COM_SLOT(XSpeechToTextSetPositionHint),
    /*  7 */ OS_COM_SLOT(XSpeechToTextSendString),
    /*  8 */ OS_COM_SLOT(XSpeechSynthesizerEnumerateInstalledVoices),
    /*  9 */ OS_COM_SLOT(XSpeechSynthesizerCreate),
    /* 10 */ OS_COM_SLOT(XSpeechSynthesizerCloseHandle),
    /* 11 */ OS_COM_SLOT(XSpeechSynthesizerSetDefaultVoice),
    /* 12 */ OS_COM_SLOT(XSpeechSynthesizerSetCustomVoice),
    /* 13 */ OS_COM_SLOT(XSpeechSynthesizerCreateStreamFromText),
    /* 14 */ OS_COM_SLOT(XSpeechSynthesizerCloseStreamHandle),
    /* 15 */ OS_COM_SLOT(XSpeechSynthesizerGetStreamDataSize),
    /* 16 */ OS_COM_SLOT(XSpeechSynthesizerGetStreamData),
    /* 17 */ OS_COM_SLOT(XSpeechToTextBeginHypothesisString),
    /* 18 */ OS_COM_SLOT(XSpeechToTextUpdateHypothesisString),
    /* 19 */ OS_COM_SLOT(XSpeechToTextFinalizeHypothesisString),
    /* 20 */ OS_COM_SLOT(XSpeechToTextCancelHypothesisString),
    /* 21 */ OS_COM_SLOT(XSpeechSynthesizerCreateStreamFromSsml),
};

// 11. CLSID={704C3F58-E629-4CC2-B197-30511B996FE2} IID={704C3F58-E629-4CC2-B197-30511B996EE2}
const ComVtableSlot g_com_vtbl_game_save[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameSaveInitializeProvider),
    /*  4 */ OS_COM_SLOT(XGameSaveInitializeProviderAsync),
    /*  5 */ OS_COM_SLOT(XGameSaveInitializeProviderResult),
    /*  6 */ OS_COM_SLOT(XGameSaveCloseProvider),
    /*  7 */ OS_COM_SLOT(XGameSaveGetRemainingQuota),
    /*  8 */ OS_COM_SLOT(XGameSaveGetRemainingQuotaAsync),
    /*  9 */ OS_COM_SLOT(XGameSaveGetRemainingQuotaResult),
    /* 10 */ OS_COM_SLOT(XGameSaveDeleteContainer),
    /* 11 */ OS_COM_SLOT(XGameSaveDeleteContainerAsync),
    /* 12 */ OS_COM_SLOT(XGameSaveDeleteContainerResult),
    /* 13 */ OS_COM_SLOT(XGameSaveGetContainerInfo),
    /* 14 */ OS_COM_SLOT(XGameSaveEnumerateContainerInfo),
    /* 15 */ OS_COM_SLOT(XGameSaveEnumerateContainerInfoByName),
    /* 16 */ OS_COM_SLOT(XGameSaveCreateContainer),
    /* 17 */ OS_COM_SLOT(XGameSaveCloseContainer),
    /* 18 */ OS_COM_SLOT(XGameSaveEnumerateBlobInfo),
    /* 19 */ OS_COM_SLOT(XGameSaveEnumerateBlobInfoByName),
    /* 20 */ OS_COM_SLOT(XGameSaveReadBlobData),
    /* 21 */ OS_COM_SLOT(XGameSaveReadBlobDataAsync),
    /* 22 */ OS_COM_SLOT(XGameSaveReadBlobDataResult),
    /* 23 */ OS_COM_SLOT(XGameSaveCreateUpdate),
    /* 24 */ OS_COM_SLOT(XGameSaveCloseUpdate),
    /* 25 */ OS_COM_SLOT(XGameSaveSubmitBlobWrite),
    /* 26 */ OS_COM_SLOT(XGameSaveSubmitBlobDelete),
    /* 27 */ OS_COM_SLOT(XGameSaveSubmitUpdate),
    /* 28 */ OS_COM_SLOT(XGameSaveSubmitUpdateAsync),
    /* 29 */ OS_COM_SLOT(XGameSaveSubmitUpdateResult),
};

// 12. CLSID={7D824997-10DC-45AB-86B7-2737767C0BF1} IID={0CC6A956-E7E1-4FDF-9341-9D5DA649EBC8}
const ComVtableSlot g_com_vtbl_user_device_audio[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XUserFindForDevice),
    /*  4 */ OS_COM_SLOT(XUserRegisterForDeviceAssociationChanged),
    /*  5 */ OS_COM_SLOT(XUserUnregisterForDeviceAssociationChanged),
    /*  6 */ OS_COM_SLOT(XUserGetDefaultAudioEndpointUtf16),
    /*  7 */ OS_COM_SLOT(XUserRegisterForDefaultAudioEndpointUtf16Changed),
    /*  8 */ OS_COM_SLOT(XUserUnregisterForDefaultAudioEndpointUtf16Changed),
};

// 13. CLSID={8836FE87-EDB9-4FE3-8DAD-05F0D2CD5B40} IID={8836FE87-EDB9-4FE3-8DAD-05F0D2CD5B40}
const ComVtableSlot g_com_vtbl_runtime_feature[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameRuntimeIsFeatureAvailable),
};

// 14. CLSID={8CA467F7-22E8-4096-8456-BB8AA13F79D8} IID={8CA467F7-22E8-4096-8456-BB8AA13F79D8}
const ComVtableSlot g_com_vtbl_error[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XErrorReport),
    /*  4 */ OS_COM_SLOT(XErrorSetCallback),
    /*  5 */ OS_COM_SLOT(XErrorSetOptions),
};

// 15. CLSID={973A344E-24BF-4D0F-8457-56C534892B29} IID={2549F142-6419-4A06-97B5-931AAB7C2F34}
const ComVtableSlot g_com_vtbl_game_title[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameGetXboxTitleId),
    /*  4 */ OS_COM_SLOT(XLaunchNewGame),
};

// 16. CLSID={A4F1AEE2-4BF1-4485-B008-A7C26D52AC27} IID={3A949778-772E-4799-BDEA-0A6639E96BAA}
const ComVtableSlot g_com_vtbl_app_capture[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XAppCaptureTakeDiagnosticScreenshot),
    /*  4 */ OS_COM_SLOT(XAppCaptureRecordDiagnosticClip),
    /*  5 */ OS_COM_SLOT(XAppCaptureTakeScreenshot),
    /*  6 */ OS_COM_SLOT(XAppCaptureOpenScreenshotStream),
    /*  7 */ OS_COM_SLOT(XAppCaptureReadScreenshotStream),
    /*  8 */ OS_COM_SLOT(XAppCaptureCloseScreenshotStream),
    /*  9 */ OS_COM_SLOT(XAppCaptureEnableRecord),
    /* 10 */ OS_COM_SLOT(XAppCaptureDisableRecord),
};

// 17. CLSID={AF406016-E850-4AA8-A88D-2F3DCB9DAC7E} IID={F92D8712-2B27-4D8A-BF01-11A6F8E3EB42}
const ComVtableSlot g_com_vtbl_package[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XPackageGetCurrentProcessPackageIdentifier),
    /*  4 */ OS_COM_SLOT(XPackageIsPackagedProcess),
    /*  5 */ OS_COM_SLOT(XPackageCreateInstallationMonitor),
    /*  6 */ OS_COM_SLOT(XPackageCloseInstallationMonitorHandle),
    /*  7 */ OS_COM_SLOT(XPackageGetInstallationProgress),
    /*  8 */ OS_COM_SLOT(XPackageUpdateInstallationMonitor),
    /*  9 */ OS_COM_SLOT(XPackageRegisterInstallationProgressChanged),
    /* 10 */ OS_COM_SLOT(XPackageUnregisterInstallationProgressChanged),
    /* 11 */ OS_COM_SLOT(XPackageGetUserLocale),
    /* 12 */ OS_COM_SLOT(XPackageFindChunkAvailability),
    /* 13 */ OS_COM_SLOT(XPackageEnumerateChunkAvailability),
    /* 14 */ OS_COM_SLOT(XPackageChangeChunkInstallOrder),
    /* 15 */ OS_COM_SLOT(XPackageInstallChunks),
    /* 16 */ OS_COM_SLOT(XPackageInstallChunksAsync),
    /* 17 */ OS_COM_SLOT(XPackageInstallChunksResult),
    /* 18 */ OS_COM_SLOT(XPackageEstimateDownloadSize),
    /* 19 */ OS_COM_SLOT(XPackageUninstallChunks),
    /* 20 */ OS_COM_UNIMPL(),
    /* 21 */ OS_COM_UNIMPL(),
    /* 22 */ OS_COM_SLOT(XPackageUnregisterPackageInstalled),
    /* 23 */ OS_COM_UNIMPL(),
    /* 24 */ OS_COM_SLOT(XPackageGetMountPathSize),
    /* 25 */ OS_COM_SLOT(XPackageGetMountPath),
    /* 26 */ OS_COM_SLOT(XPackageCloseMountHandle),
    /* 27 */ OS_COM_UNIMPL(),
    /* 28 */ OS_COM_UNIMPL(),
    /* 29 */ OS_COM_UNIMPL(),
    /* 30 */ OS_COM_SLOT(XPackageGetWriteStats),
    /* 31 */ OS_COM_UNIMPL(),
    /* 32 */ OS_COM_SLOT(XPackageUninstallUWPInstance),
    /* 33 */ OS_COM_SLOT(XPackageEnumerateFeatures),
    /* 34 */ OS_COM_UNIMPL(),
    /* 35 */ OS_COM_SLOT(XPackageEnumeratePackages),
    /* 36 */ OS_COM_SLOT(XPackageRegisterPackageInstalled),
};

// 18. CLSID={B884675D-B738-4A9C-815D-9A9A1E0C6C9B} IID={B884675D-B738-4A9C-815D-9A9A1E0C6C9B}
const ComVtableSlot g_com_vtbl_system_analytics[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ reinterpret_cast<ComVtableSlot>(&ComThunk_XSystemGetAnalyticsInfo),
};

// 19. CLSID={BBFBDCC7-BFE7-409B-A5CA-EDF054960B4D} IID={BBFBDCC7-BFE7-409B-A5CA-EDF054960B4D}
const ComVtableSlot g_com_vtbl_game_event[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameEventWrite),
};

// 20. CLSID={DFCD4649-4FF8-4043-BA07-35D607DF98B0} IID={72C4E70F-4A67-481B-BB84-AA72EDA14FE5}
const ComVtableSlot g_com_vtbl_game_ui[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XGameUiShowMessageDialogAsync),
    /*  4 */ OS_COM_SLOT(XGameUiShowMessageDialogResult),
    /*  5 */ OS_COM_SLOT(XGameUiShowSendGameInviteAsync),
    /*  6 */ OS_COM_SLOT(XGameUiShowSendGameInviteResult),
    /*  7 */ OS_COM_SLOT(XGameUiShowPlayerProfileCardAsync),
    /*  8 */ OS_COM_SLOT(XGameUiShowPlayerProfileCardResult),
    /*  9 */ OS_COM_SLOT(XGameUiShowAchievementsAsync),
    /* 10 */ OS_COM_SLOT(XGameUiShowAchievementsResult),
    /* 11 */ OS_COM_SLOT(XGameUiShowPlayerPickerAsync),
    /* 12 */ OS_COM_SLOT(XGameUiShowPlayerPickerResultCount),
    /* 13 */ OS_COM_SLOT(XGameUiShowPlayerPickerResult),
    /* 14 */ OS_COM_SLOT(XGameUiShowErrorDialogAsync),
    /* 15 */ OS_COM_SLOT(XGameUiShowErrorDialogResult),
    /* 16 */ OS_COM_SLOT(XGameUiSetNotificationPositionHint),
    /* 17 */ OS_COM_SLOT(XGameUiShowTextEntryAsync),
    /* 18 */ OS_COM_SLOT(XGameUiShowTextEntryResultSize),
    /* 19 */ OS_COM_SLOT(XGameUiShowTextEntryResult),
    /* 20 */ OS_COM_UNIMPL(),
    /* 21 */ OS_COM_UNIMPL(),
    /* 22 */ OS_COM_UNIMPL(),
    /* 23 */ OS_COM_UNIMPL(),
    /* 24 */ OS_COM_SLOT(XGameUiShowWebAuthenticationAsync),
    /* 25 */ OS_COM_SLOT(XGameUiShowWebAuthenticationResultSize),
    /* 26 */ OS_COM_SLOT(XGameUiShowWebAuthenticationResult),
    /* 27 */ OS_COM_SLOT(XGameUiShowWebAuthenticationWithOptionsAsync),
};

// 21. CLSID={E349BD1A-FC20-4E40-B99C-4178CC6B409F} IID={67CE4BFC-B1D1-4AC7-BC3A-CB9219A97A85}
const ComVtableSlot g_com_vtbl_system_console[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XSystemGetConsoleId),
    /*  4 */ OS_COM_SLOT(XSystemGetXboxLiveSandboxId),
};

// 22. CLSID={F4FAF4D4-2D04-4FCE-B3E0-474A713A3E84} IID={D29411DF-0794-4553-8B27-95FC02D0F75D}
const ComVtableSlot g_com_vtbl_pls[] = {
    OS_COM_IUNKNOWN_SLOTS,
    /*  3 */ OS_COM_SLOT(XPersistentLocalStorageGetPathSize),
    /*  4 */ OS_COM_SLOT(XPersistentLocalStorageGetPath),
    /*  5 */ OS_COM_SLOT(XPersistentLocalStorageGetSpaceInfo),
    /*  6 */ OS_COM_SLOT(XPersistentLocalStoragePromptUserForSpaceAsync),
    /*  7 */ OS_COM_SLOT(XPersistentLocalStoragePromptUserForSpaceResult),
};

#undef OS_COM_IUNKNOWN_SLOTS
#undef OS_COM_UNIMPL
#undef OS_COM_SLOT

XgrRealComObject g_real_com_objects[kComObjCount] = {
    {g_com_vtbl_user, kComUser},
    {g_com_vtbl_user_gamertag, kComUserGamertag},
    {g_com_vtbl_display_hdr, kComDisplayHdr},
    {g_com_vtbl_game_invite, kComGameInvite},
    {g_com_vtbl_async_taskqueue, kComAsyncTaskQueue},
    {g_com_vtbl_store, kComStore},
    {g_com_vtbl_app_broadcast_meta, kComAppBroadcastMeta},
    {g_com_vtbl_launch_display, kComLaunchDisplay},
    {g_com_vtbl_networking, kComNetworking},
    {g_com_vtbl_accessibility_speech, kComAccessibilitySpeech},
    {g_com_vtbl_game_save, kComGameSave},
    {g_com_vtbl_user_device_audio, kComUserDeviceAudio},
    {g_com_vtbl_runtime_feature, kComRuntimeFeature},
    {g_com_vtbl_error, kComError},
    {g_com_vtbl_game_title, kComGameTitle},
    {g_com_vtbl_app_capture, kComAppCapture},
    {g_com_vtbl_package, kComPackage},
    {g_com_vtbl_system_analytics, kComSystemAnalytics},
    {g_com_vtbl_game_event, kComGameEvent},
    {g_com_vtbl_game_ui, kComGameUi},
    {g_com_vtbl_system_console, kComSystemConsole},
    {g_com_vtbl_pls, kComPls},
};

struct RealComInterfaceMapping {
    XgrGuid clsid;
    XgrGuid iid;
    uint32_t obj_index;
};

constexpr RealComInterfaceMapping kRealComMappings[] = {
    // 1. XUser main
    {{0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}},
     {0x1BF2F8C5u, 0xD507u, 0x4E52u, {0xBB, 0x05, 0xF7, 0x26, 0xD0, 0xE7, 0x11, 0x61}},
     kComUser},
    // 2. XUser gamertag
    {{0x01ACD177u, 0x91F9u, 0x4763u, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}},
     {0xCEF4FAC0u, 0x7676u, 0x4A94u, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}},
     kComUserGamertag},
    // 3. XDisplay HDR
    {{0x03F0FE74u, 0xFDD9u, 0x4E5Cu, {0xB6, 0x30, 0xF9, 0x33, 0x9C, 0x47, 0xAC, 0xC5}},
     {0x35F07670u, 0x706Eu, 0x4BFBu, {0x94, 0x76, 0x09, 0x07, 0x98, 0xC5, 0xEB, 0xF3}},
     kComDisplayHdr},
    // 4. XGameInvite
    {{0x0651AAE2u, 0x4012u, 0x4077u, {0xBF, 0x84, 0x8B, 0x90, 0x97, 0x09, 0x0E, 0x2C}},
     {0x0651AAE2u, 0x4012u, 0x4077u, {0xBF, 0x84, 0x8B, 0x90, 0x97, 0x09, 0x0E, 0x2C}},
     kComGameInvite},
    // 5. XAsync / XTaskQueue / XThread
    {{0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}},
     {0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}},
     kComAsyncTaskQueue},
    // 6a. XStore main
    {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
     {0xDE3DBDD4u, 0x0B37u, 0x4BDBu, {0xA1, 0x0E, 0xAC, 0xF3, 0xA3, 0x54, 0xD0, 0x6A}},
     kComStore},
    // 6b. XStore IsAvailabilityPurchasable
    {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
     {0x60B09F4Eu, 0x1B85u, 0x45B1u, {0x82, 0x6C, 0x16, 0x91, 0x18, 0xE2, 0x30, 0xE1}},
     kComStore},
    // 6c. XStore AcquireLicenseForDurables
    {{0x0DD112ACu, 0x7C24u, 0x448Cu, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}},
     {0x2D42FEA5u, 0xE71Du, 0x4B76u, {0x97, 0xCD, 0xC5, 0x0A, 0xFB, 0xB3, 0xAE, 0x5D}},
     kComStore},
    // 7. XAppBroadcast + XAppCaptureMetadata
    {{0x186D5592u, 0xA72Du, 0x45FBu, {0x95, 0x60, 0x11, 0xAE, 0xD0, 0xE6, 0x64, 0x7A}},
     {0x186D5592u, 0xA72Du, 0x45FBu, {0x95, 0x60, 0x11, 0xAE, 0xD0, 0xE6, 0x64, 0x7A}},
     kComAppBroadcastMeta},
    // 8. XLaunchUri + XDisplay timeout deferral
    {{0x1B339674u, 0x328Du, 0x4283u, {0xA2, 0x00, 0x31, 0x71, 0xF1, 0x8D, 0x36, 0x39}},
     {0x1B339674u, 0x328Du, 0x4283u, {0xA2, 0x00, 0x31, 0x71, 0xF1, 0x8D, 0x36, 0x39}},
     kComLaunchDisplay},
    // 9. XNetworking
    {{0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}},
     {0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}},
     kComNetworking},
    // 10. XAccessibility + XSpeechSynthesizer + XSpeechToText
    {{0x3E241977u, 0x6237u, 0x41E9u, {0x85, 0x59, 0x10, 0x2C, 0x2D, 0x99, 0x83, 0xF1}},
     {0xD722B373u, 0x8C4Du, 0x4692u, {0x9E, 0x51, 0xD6, 0xAD, 0x9B, 0x37, 0xAA, 0x7D}},
     kComAccessibilitySpeech},
    // 11. XGameSave
    {{0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}},
     {0x704C3F58u, 0xE629u, 0x4CC2u, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6E, 0xE2}},
     kComGameSave},
    // 12. XUser device & audio endpoints
    {{0x7D824997u, 0x10DCu, 0x45ABu, {0x86, 0xB7, 0x27, 0x37, 0x76, 0x7C, 0x0B, 0xF1}},
     {0x0CC6A956u, 0xE7E1u, 0x4FDFu, {0x93, 0x41, 0x9D, 0x5D, 0xA6, 0x49, 0xEB, 0xC8}},
     kComUserDeviceAudio},
    // 13. XGameRuntimeIsFeatureAvailable
    {{0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}},
     {0x8836FE87u, 0xEDB9u, 0x4FE3u, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}},
     kComRuntimeFeature},
    // 14. XError
    {{0x8CA467F7u, 0x22E8u, 0x4096u, {0x84, 0x56, 0xBB, 0x8A, 0xA1, 0x3F, 0x79, 0xD8}},
     {0x8CA467F7u, 0x22E8u, 0x4096u, {0x84, 0x56, 0xBB, 0x8A, 0xA1, 0x3F, 0x79, 0xD8}},
     kComError},
    // 15. XGameGetXboxTitleId + XLaunchNewGame
    {{0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}},
     {0x2549F142u, 0x6419u, 0x4A06u, {0x97, 0xB5, 0x93, 0x1A, 0xAB, 0x7C, 0x2F, 0x34}},
     kComGameTitle},
    // 16. XAppCapture screenshot & record
    {{0xA4F1AEE2u, 0x4BF1u, 0x4485u, {0xB0, 0x08, 0xA7, 0xC2, 0x6D, 0x52, 0xAC, 0x27}},
     {0x3A949778u, 0x772Eu, 0x4799u, {0xBD, 0xEA, 0x0A, 0x66, 0x39, 0xE9, 0x6B, 0xAA}},
     kComAppCapture},
    // 17. XPackage
    {{0xAF406016u, 0xE850u, 0x4AA8u, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}},
     {0xF92D8712u, 0x2B27u, 0x4D8Au, {0xBF, 0x01, 0x11, 0xA6, 0xF8, 0xE3, 0xEB, 0x42}},
     kComPackage},
    // 18. XSystemGetAnalyticsInfo
    {{0xB884675Du, 0xB738u, 0x4A9Cu, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}},
     {0xB884675Du, 0xB738u, 0x4A9Cu, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}},
     kComSystemAnalytics},
    // 19. XGameEventWrite
    {{0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}},
     {0xBBFBDCC7u, 0xBFE7u, 0x409Bu, {0xA5, 0xCA, 0xED, 0xF0, 0x54, 0x96, 0x0B, 0x4D}},
     kComGameEvent},
    // 20. XGameUi
    {{0xDFCD4649u, 0x4FF8u, 0x4043u, {0xBA, 0x07, 0x35, 0xD6, 0x07, 0xDF, 0x98, 0xB0}},
     {0x72C4E70Fu, 0x4A67u, 0x481Bu, {0xBB, 0x84, 0xAA, 0x72, 0xED, 0xA1, 0x4F, 0xE5}},
     kComGameUi},
    // 21. XSystem ConsoleId + SandboxId
    {{0xE349BD1Au, 0xFC20u, 0x4E40u, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}},
     {0x67CE4BFCu, 0xB1D1u, 0x4AC7u, {0xBC, 0x3A, 0xCB, 0x92, 0x19, 0xA9, 0x7A, 0x85}},
     kComSystemConsole},
    // 22. XPersistentLocalStorage
    {{0xF4FAF4D4u, 0x2D04u, 0x4FCEu, {0xB3, 0xE0, 0x47, 0x4A, 0x71, 0x3A, 0x3E, 0x84}},
     {0xD29411DFu, 0x0794u, 0x4553u, {0x8B, 0x27, 0x95, 0xFC, 0x02, 0xD0, 0xF7, 0x5D}},
     kComPls},
};

bool IsKnownRealGdkClsid(const XgrGuid& clsid) noexcept {
    for (const auto& entry : kRealComMappings) {
        if (XgrGuidEquals(entry.clsid, clsid)) {
            return true;
        }
    }
    return false;
}

XgrRealComObject* LookupRealComInterface(const XgrGuid& clsid, const XgrGuid& iid) noexcept {
    for (const auto& entry : kRealComMappings) {
        if (XgrGuidEquals(entry.clsid, clsid) &&
            (XgrGuidEquals(entry.iid, iid) || XgrGuidEquals(iid, XGR_IID_IUNKNOWN))) {
            return &g_real_com_objects[entry.obj_index];
        }
    }
    return nullptr;
}

const XgrGuid& GetRealComClsidByIndex(uint32_t idx) noexcept {
    for (const auto& entry : kRealComMappings) {
        if (entry.obj_index == idx) {
            return entry.clsid;
        }
    }
    return kRealComMappings[0].clsid;
}

const void* LookupInternal2ArgVtable(const XgrGuid& apiId) noexcept {
    if (XgrGuidEquals(apiId, XGR_API_GUID_TASK_QUEUE)) return &g_task_queue_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_ASYNC)) return &g_async_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_USER)) return &g_user_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_GAME_SAVE)) return &g_game_save_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_SYSTEM)) return &g_system_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_MEM)) return &g_mem_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_PACKAGE)) return &g_package_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_DISPLAY)) return &g_display_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_GAME_UI)) return &g_game_ui_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_GAME_INVITE)) return &g_game_invite_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_ACCESSIBILITY)) return &g_accessibility_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_SPEECH_SYNTHESIZER)) return &g_speech_synth_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_STORE)) return &g_store_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_NETWORKING)) return &g_networking_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_CORE_RUNTIME)) return &g_core_runtime_vtable;
    if (XgrGuidEquals(apiId, XGR_API_GUID_GAME_STREAMING)) return &g_game_streaming_vtable;
    return nullptr;
}

}  // namespace

extern "C" {

// ===========================================================================
// GDK Static Import GUID COM Vtable Dispatch
// ===========================================================================
XGameResult OPENSCARLETT_GDK_CALL InitializeApiImpl(void) {
    auto& rt = GetRuntime();
    rt.api_impl_init_count.fetch_add(1, std::memory_order_acq_rel);
    SyncConsoleLsaPolicyFromConfig();
    return RecordHr("InitializeApiImpl", XGR_S_OK);
}

XGameResult OPENSCARLETT_GDK_CALL InitializeApiImplEx(
    uint64_t /*versionHigh*/,
    uint64_t /*versionLow*/,
    uint64_t /*options*/) {
    auto& rt = GetRuntime();
    rt.api_impl_init_count.fetch_add(1, std::memory_order_acq_rel);
    SyncConsoleLsaPolicyFromConfig();
    return RecordHr("InitializeApiImplEx", XGR_S_OK);
}

void OPENSCARLETT_GDK_CALL UninitializeApiImpl(void) {
    auto& rt = GetRuntime();
    const int32_t prev = rt.api_impl_init_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        rt.api_impl_init_count.store(0, std::memory_order_release);
    }
    RecordOk("UninitializeApiImpl");
}

static bool IsWritableMemoryPtr(const void* ptr) noexcept {
#if defined(_WIN32)
    if (!ptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_READWRITE ||
           prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
#else
    return ptr != nullptr;
#endif
}

XGameResult OPENSCARLETT_GDK_CALL QueryApiImpl(
    const XgrGuid* apiId,
    void** ppApiVtable,
    void** ppApiVtable3) {
    // Support BOTH:
    //   - 2-arg OpenScarlett internal ABI: QueryApiImpl(const XgrGuid* apiId, void** ppApiVtable)
    //   - 3-arg real Microsoft XGamingRuntimeThunks.dll COM ABI:
    //       QueryApiImpl(const GUID* clsid, const GUID* iid, void** ppv)
    //     where arg2 points to a .rdata IID (non-writable) OR arg1 is a known real GDK CLSID
    //     with a valid writable arg3 output pointer.
    if (apiId) {
        const void* internal_vtable = LookupInternal2ArgVtable(*apiId);
        if (internal_vtable) {
            if (!ppApiVtable) {
                return RecordHr("QueryApiImpl", XGR_E_POINTER);
            }
            XgrVtableAddRef(internal_vtable);
            *ppApiVtable = const_cast<void*>(internal_vtable);
            return RecordHr("QueryApiImpl", XGR_S_OK);
        }
    }

    const bool is_three_arg =
        apiId && ppApiVtable &&
        (!IsWritableMemoryPtr(ppApiVtable) ||
         (IsKnownRealGdkClsid(*apiId) && ppApiVtable3 && IsWritableMemoryPtr(ppApiVtable3)));

    if (is_three_arg) {
        const auto* clsid = apiId;
        const auto* iid = reinterpret_cast<const XgrGuid*>(ppApiVtable);
        if (!ppApiVtable3 || !IsWritableMemoryPtr(ppApiVtable3)) {
            return RecordHr("QueryApiImpl", XGR_E_POINTER);
        }
        *ppApiVtable3 = nullptr;
        XgrRealComObject* com_obj = LookupRealComInterface(*clsid, *iid);
        if (!com_obj) {
            const std::string sym =
                "QueryApiImpl(clsid=" + FormatXgrGuid(*clsid) + ",iid=" + FormatXgrGuid(*iid) + ")";
            openscarlett::CallTelemetry::Global().Record(
                kModName, sym, openscarlett::CallOutcome::NotImplemented, XGR_E_NOINTERFACE, -1);
            return XGR_E_NOINTERFACE;
        }
        RealComAddRef(com_obj);
        *ppApiVtable3 = com_obj;
        return RecordHr("QueryApiImpl", XGR_S_OK);
    }

    if (!ppApiVtable) {
        return RecordHr("QueryApiImpl", XGR_E_POINTER);
    }
    *ppApiVtable = nullptr;
    if (!apiId) {
        return RecordHr("QueryApiImpl", XGR_E_INVALIDARG);
    }

    const std::string sym = "QueryApiImpl(" + FormatXgrGuid(*apiId) + ")";
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::NotImplemented, XGR_E_NOINTERFACE, -1);
    return XGR_E_NOINTERFACE;
}

int32_t OPENSCARLETT_GDK_CALL XgrGetTelemetryCallCountForTest(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outNotImplCalls) {
    if (!symbolName) {
        return 0;
    }
    const auto entries = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& e : entries) {
        if (e.module_name == kModName && e.symbol_name == symbolName) {
            if (outTotalCalls) *outTotalCalls = e.total_calls;
            if (outNotImplCalls) *outNotImplCalls = e.not_impl_calls;
            return 1;
        }
    }
    if (outTotalCalls) *outTotalCalls = 0;
    if (outNotImplCalls) *outNotImplCalls = 0;
    return 0;
}

// ===========================================================================
// Xbox Game Core PLM Notifications (appnotify.h) exported from xgameruntime.dll
// ===========================================================================
}  // extern "C"

namespace {

openscarlett::PlmNotificationRegistry& GetXgrPlmRegistry() {
    static openscarlett::PlmNotificationRegistry reg;
    return reg;
}

}  // namespace

extern "C" {

OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL RegisterAppStateChangeNotification(
    openscarlett::PlmStateChangeCallback Routine,
    void* Context,
    void** Registration) {
    const uint32_t err = GetXgrPlmRegistry().RegisterStateChange(Routine, Context, Registration);
    if (err == 0u) {
        RecordOk("RegisterAppStateChangeNotification");
    } else {
        (void)RecordHr("RegisterAppStateChangeNotification", XGR_E_INVALIDARG);
    }
    return err;
}

OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL UnregisterAppStateChangeNotification(
    void* Registration) {
    (void)GetXgrPlmRegistry().UnregisterStateChange(Registration);
    RecordOk("UnregisterAppStateChangeNotification");
}

OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL RegisterAppConstrainedChangeNotification(
    openscarlett::PlmConstrainedChangeCallback Routine,
    void* Context,
    void** Registration) {
    const uint32_t err = GetXgrPlmRegistry().RegisterConstrainedChange(Routine, Context, Registration);
    if (err == 0u) {
        RecordOk("RegisterAppConstrainedChangeNotification");
    } else {
        (void)RecordHr("RegisterAppConstrainedChangeNotification", XGR_E_INVALIDARG);
    }
    return err;
}

OPENSCARLETT_GDK_API void OPENSCARLETT_GDK_CALL UnregisterAppConstrainedChangeNotification(
    void* Registration) {
    (void)GetXgrPlmRegistry().UnregisterConstrainedChange(Registration);
    RecordOk("UnregisterAppConstrainedChangeNotification");
}

OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL XgrDispatchAppStateChange(uint8_t Quiesced) {
    return GetXgrPlmRegistry().DispatchStateChange(Quiesced);
}

OPENSCARLETT_GDK_API uint32_t OPENSCARLETT_GDK_CALL XgrDispatchAppConstrainedChange(uint8_t Constrained) {
    return GetXgrPlmRegistry().DispatchConstrainedChange(Constrained);
}

OPENSCARLETT_GDK_API XGameResult OPENSCARLETT_GDK_CALL XgrGetConsoleLsaIdentityForTest(
    char* pfnBuf,
    size_t pfnSize,
    uint32_t* outTitleId) {
    const auto pkg = openscarlett::ConsoleLsaPolicy::Global().GetPackageIdentity();
    if (outTitleId) {
        *outTitleId = pkg.title_id;
    }
    if (pfnBuf && pfnSize > 0u) {
        return CopyStringResult(pkg.package_family_name, pfnSize, pfnBuf, nullptr);
    }
    return XGR_S_OK;
}

}  // extern "C"
