#define OPENSCARLETT_BUILDING_DSTORAGE 1
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/kernel/vram_residency_manager.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/gdeflate.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <d3d12.h>
#  include <windows.h>

#  if defined(_MSC_VER)
#    define OS_UUIDOF(T) __uuidof(T)
#  elif defined(__CRT_UUID_DECL)
#    define OS_UUIDOF(T) __mingw_uuidof<T>()
#  else
#    define OS_UUIDOF(T) IID_##T
#  endif
#endif

namespace {

constexpr const char* kModName = "dstorage_xs.dll";

#if defined(_WIN32)
using PfnQueryTitlePhysicalPages = int32_t(__stdcall*)(
    const void*, uint64_t*, uint64_t*, uint32_t*);
using PfnXMemSize = size_t(__stdcall*)(void*, uint64_t);

bool IsWin32ProtectWritable(DWORD prot) {
    if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD base = prot & 0xFFu;
    return base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}
#endif

struct DestinationClassification {
    bool is_physical_page = false;
    bool is_xmem = false;
    uint64_t physical_id = 0;
    uint64_t physical_mapping_size = 0;
};

int32_t ValidateAndClassifyDestination(
    void* dst_ptr,
    size_t dst_cap,
    DestinationClassification& out_class) {
    out_class = DestinationClassification{};
    if (!dst_ptr || dst_cap == 0u) {
        return OS_DSTORAGE_E_INVALIDARG;
    }

#if defined(_WIN32)
    uintptr_t cursor = reinterpret_cast<uintptr_t>(dst_ptr);
    if (cursor + dst_cap < cursor) {
        return OS_DSTORAGE_E_INVALIDARG;
    }
    size_t remaining = dst_cap;
    while (remaining > 0u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0u) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        if (mbi.State != MEM_COMMIT || !IsWin32ProtectWritable(mbi.Protect)) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const uintptr_t region_end =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cursor) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const size_t avail = static_cast<size_t>(region_end - cursor);
        const size_t step = std::min(remaining, avail);
        cursor += step;
        remaining -= step;
    }

    HMODULE h_kx = GetModuleHandleW(L"kernelx.dll");
    if (h_kx) {
        auto fn_query = reinterpret_cast<PfnQueryTitlePhysicalPages>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "QueryTitlePhysicalPages")));
        if (fn_query) {
            uint64_t phys_id = 0u;
            uint64_t map_size = 0u;
            uint32_t flags = 0u;
            if (fn_query(dst_ptr, &phys_id, &map_size, &flags) != 0) {
                out_class.is_physical_page = true;
                out_class.physical_id = phys_id;
                out_class.physical_mapping_size = map_size;
                if (map_size > 0u && dst_cap > map_size) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
            }
        }
        auto fn_xmem_size = reinterpret_cast<PfnXMemSize>(
            reinterpret_cast<void*>(GetProcAddress(h_kx, "XMemSize")));
        if (fn_xmem_size) {
            const size_t xsz = fn_xmem_size(dst_ptr, 0u);
            if (xsz > 0u) {
                out_class.is_xmem = true;
            }
        }
    }
#else
    (void)dst_ptr;
    (void)dst_cap;
#endif
    return OS_DSTORAGE_S_OK;
}

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const DStorageGuid* a, const DStorageGuid& b) {
    return a && std::memcmp(a, &b, sizeof(DStorageGuid)) == 0;
}

// ===========================================================================
// Global Custom Decompression Callback Registry & Priority Telemetry
// ===========================================================================
struct CustomDecompressorEntry {
    PfnOpenScarlettDStorageCustomDecompress callback = nullptr;
    void* user_context = nullptr;
};

struct GlobalDStorageRuntime {
    std::mutex custom_mutex;
    std::unordered_map<uint8_t, CustomDecompressorEntry> custom_decompressors;

    std::atomic<uint64_t> realtime_dispatched{0u};
    std::atomic<uint64_t> high_dispatched{0u};
    std::atomic<uint64_t> normal_dispatched{0u};
    std::atomic<uint64_t> low_dispatched{0u};
    std::atomic<uint64_t> custom_decompressed{0u};
    std::atomic<uint64_t> gpu_texture_uploads{0u};
    std::atomic<uint64_t> gpu_untiled_uploads{0u};

    bool LookupCustom(uint8_t format, CustomDecompressorEntry& out) {
        std::lock_guard<std::mutex> lk(custom_mutex);
        auto it = custom_decompressors.find(format);
        if (it == custom_decompressors.end() || !it->second.callback) {
            return false;
        }
        out = it->second;
        return true;
    }
};

GlobalDStorageRuntime& GetRuntime() {
    static GlobalDStorageRuntime rt;
    return rt;
}

size_t PriorityToBucket(DSTORAGE_PRIORITY p) noexcept {
    if (p >= DSTORAGE_PRIORITY_REALTIME) return 0u;
    if (p == DSTORAGE_PRIORITY_HIGH) return 1u;
    if (p == DSTORAGE_PRIORITY_NORMAL) return 2u;
    return 3u;  // DSTORAGE_PRIORITY_LOW
}

// ===========================================================================
// IDStorageFileX Implementation (real Win32 file I/O)
// ===========================================================================
class DStorageFileImpl final : public IDStorageFileX {
public:
    explicit DStorageFileImpl(
#if defined(_WIN32)
        HANDLE hfile
#else
        void* hfile
#endif
        )
        : file_handle_(hfile) {}

    ~DStorageFileImpl() {
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) || GuidEquals(riid, IID_IDStorageFileX)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFileX*>(this);
            return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFileX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFileX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFileX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageFileX::Close", 3);
        CloseInternal();
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetFileInformation(DSTORAGE_FILE_INFO* info) override {
        if (!info) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file_handle_, &sz)) {
            return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
        }
        info->FileSize = static_cast<uint64_t>(sz.QuadPart);
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_S_OK, 4);
#else
        info->FileSize = 0;
        return RecordHr("IDStorageFileX::GetFileInformation", OS_DSTORAGE_E_IO_ERROR, 4);
#endif
    }

    int32_t ReadAt(
        uint64_t offset,
        uint32_t size,
        std::vector<uint8_t>& out,
        const std::atomic<bool>* cancel_flag = nullptr) {
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ == INVALID_HANDLE_VALUE || !file_handle_) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }
        if (offset + static_cast<uint64_t>(size) < offset) {
            return OS_DSTORAGE_E_INVALIDARG;
        }
        LARGE_INTEGER file_sz{};
        if (!GetFileSizeEx(file_handle_, &file_sz) ||
            file_sz.QuadPart < 0 ||
            offset + static_cast<uint64_t>(size) > static_cast<uint64_t>(file_sz.QuadPart)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_handle_, pos, nullptr, FILE_BEGIN)) {
            return OS_DSTORAGE_E_IO_ERROR;
        }
        out.resize(size);
        constexpr uint32_t kReadChunkBytes = 65536u;
        uint32_t total_read = 0;
        while (total_read < size) {
            if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
                out.clear();
                return OS_DSTORAGE_E_CANCELLED;
            }
            const DWORD to_read = static_cast<DWORD>(std::min<uint32_t>(size - total_read, kReadChunkBytes));
            DWORD chunk = 0;
            if (!ReadFile(file_handle_, out.data() + total_read, to_read, &chunk, nullptr)) {
                return OS_DSTORAGE_E_IO_ERROR;
            }
            if (chunk == 0) {
                return OS_DSTORAGE_E_IO_ERROR;
            }
            total_read += chunk;
        }
        return OS_DSTORAGE_S_OK;
#else
        (void)offset;
        (void)size;
        (void)out;
        (void)cancel_flag;
        return OS_DSTORAGE_E_IO_ERROR;
#endif
    }

private:
    void CloseInternal() {
        std::lock_guard<std::mutex> lk(io_mutex_);
#if defined(_WIN32)
        if (file_handle_ != INVALID_HANDLE_VALUE && file_handle_ != nullptr) {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
        }
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    std::mutex io_mutex_;
#if defined(_WIN32)
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
#else
    void* file_handle_ = nullptr;
#endif
};

// ===========================================================================
// IDStorageStatusArrayX Implementation
// ===========================================================================
class DStorageStatusArrayImpl final : public IDStorageStatusArrayX {
public:
    explicit DStorageStatusArrayImpl(uint32_t capacity)
        : complete_(capacity, true), results_(capacity, OS_DSTORAGE_S_OK) {}

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageStatusArrayX)) {
            AddRef();
            *ppvObject = static_cast<IDStorageStatusArrayX*>(this);
            return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageStatusArrayX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageStatusArrayX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageStatusArrayX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    bool OPENSCARLETT_DSTORAGE_CALL IsComplete(uint32_t index) override {
        RecordOk("IDStorageStatusArrayX::IsComplete", 3);
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= complete_.size()) return false;
        return complete_[index];
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL GetHResult(uint32_t index) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= results_.size()) {
            return RecordHr("IDStorageStatusArrayX::GetHResult", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        const int32_t hr = complete_[index] ? results_[index] : OS_DSTORAGE_E_PENDING;
        return RecordHr("IDStorageStatusArrayX::GetHResult", hr, 4);
    }

    void MarkPending(uint32_t index) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            complete_[index] = false;
            results_[index] = OS_DSTORAGE_E_PENDING;
        }
    }

    void MarkComplete(uint32_t index, int32_t hr) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < complete_.size()) {
            results_[index] = hr;
            complete_[index] = true;
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    std::vector<bool> complete_;
    std::vector<int32_t> results_;
};

#if defined(_WIN32)
// ===========================================================================
// Lightweight Per-Queue D3D12 Upload + XgGpuUntiler Context
// ===========================================================================
struct DStorageGpuUploadContext {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* cmd_queue = nullptr;
    ID3D12CommandAllocator* cmd_alloc = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0ull;
    openscarlett::XgGpuUntiler untiler;
    bool untiler_attempted = false;

    ~DStorageGpuUploadContext() {
        Reset();
    }

    void Reset() {
        untiler.Shutdown();
        untiler_attempted = false;
        if (cmd_list) { cmd_list->Release(); cmd_list = nullptr; }
        if (cmd_alloc) { cmd_alloc->Release(); cmd_alloc = nullptr; }
        if (cmd_queue) { cmd_queue->Release(); cmd_queue = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        fence_value = 0ull;
    }

    bool EnsureInitialized(ID3D12Device* dev) {
        if (!dev) return false;
        if (device == dev && cmd_queue && cmd_alloc && cmd_list && fence && fence_event) {
            return true;
        }
        Reset();
        dev->AddRef();
        device = dev;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue),
                                           reinterpret_cast<void**>(&cmd_queue))) ||
            !cmd_queue) {
            Reset();
            return false;
        }
        if (FAILED(dev->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                OS_UUIDOF(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&cmd_alloc))) ||
            !cmd_alloc) {
            Reset();
            return false;
        }
        if (FAILED(dev->CreateCommandList(
                0u,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                cmd_alloc,
                nullptr,
                OS_UUIDOF(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&cmd_list))) ||
            !cmd_list) {
            Reset();
            return false;
        }
        cmd_list->Close();

        if (FAILED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence),
                                    reinterpret_cast<void**>(&fence))) ||
            !fence) {
            Reset();
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            Reset();
            return false;
        }
        return true;
    }

    bool EnsureUntilerReady() {
        if (!device) return false;
        if (untiler.IsD3D12Ready()) return true;
        if (untiler_attempted) return false;
        untiler_attempted = true;
        return untiler.InitializeD3D12WithDevice(device).ok();
    }

    bool ExecuteAndWait() {
        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list};
        cmd_queue->ExecuteCommandLists(1u, lists);
        const uint64_t wait_val = ++fence_value;
        if (FAILED(cmd_queue->Signal(fence, wait_val))) return false;
        if (fence->GetCompletedValue() < wait_val) {
            if (FAILED(fence->SetEventOnCompletion(wait_val, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 30000u) != WAIT_OBJECT_0) return false;
        }
        return true;
    }
};

HRESULT CreateUploadBufferLocal(ID3D12Device* dev, uint64_t size_bytes, ID3D12Resource** out_buf) {
    *out_buf = nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(size_bytes, 256ull);
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return dev->CreateCommittedResource(
        &hp,
        D3D12_HEAP_FLAG_NONE,
        &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        OS_UUIDOF(ID3D12Resource),
        reinterpret_cast<void**>(out_buf));
}
#endif

// Forward declaration for priority scheduler.
class DStorageQueueImpl;

// ===========================================================================
// Cross-Queue Priority Scheduler (REALTIME > HIGH > NORMAL > LOW)
// Ref-counted worker lifecycle: starts on first queue, joins on last queue close
// (guarantees zero DLL_PROCESS_DETACH loader-lock hangs on Windows).
// ===========================================================================
class DStoragePriorityScheduler {
public:
    static DStoragePriorityScheduler& Global() {
        static DStoragePriorityScheduler sched;
        return sched;
    }

    ~DStoragePriorityScheduler() {
        // If any caller leaked a queue until DLL_PROCESS_DETACH, detach instead of hanging.
        if (worker_.joinable()) {
            worker_.detach();
        }
    }

    void RegisterQueue(DStorageQueueImpl* q, DSTORAGE_PRIORITY priority);
    void UnregisterQueue(DStorageQueueImpl* q);
    void NotifyWorkAvailable();

private:
    struct Slot {
        DStorageQueueImpl* queue = nullptr;
        size_t bucket = 2u;
        bool in_progress = false;
        bool removing = false;
    };

    void RunLoop();

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::vector<Slot> slots_;
    std::array<size_t, 4> rr_cursor_{0u, 0u, 0u, 0u};
    bool stop_ = false;
    bool worker_running_ = false;
    std::thread worker_;
};

// ===========================================================================
// IDStorageQueueX Implementation (Priority-Scheduled Async Worker +
// GDeflate / BCPack / Custom Decompressors + GPU XgGpuUntiler Texture Upload)
// ===========================================================================
class DStorageQueueImpl final : public IDStorageQueueX {
public:
    explicit DStorageQueueImpl(
        const DSTORAGE_QUEUE_DESC& desc,
        std::shared_ptr<std::atomic<uint32_t>> staging_size_ref = nullptr)
        : desc_(desc),
          staging_size_ref_(staging_size_ref
                                ? std::move(staging_size_ref)
                                : std::make_shared<std::atomic<uint32_t>>(
                                      DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)) {
        if (desc_.Capacity < DSTORAGE_MIN_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        } else if (desc_.Capacity > DSTORAGE_MAX_QUEUE_CAPACITY) {
            desc_.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        }
        auto_submit_threshold_ =
            std::max<uint16_t>(1u, static_cast<uint16_t>(desc_.Capacity / 2u));
        if (desc_.Name) {
            name_storage_ = desc_.Name;
            desc_.Name = name_storage_.c_str();
        }
#if defined(_WIN32)
        error_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#endif
        DStoragePriorityScheduler::Global().RegisterQueue(this, desc_.Priority);
    }

    ~DStorageQueueImpl() {
        CloseInternal();
#if defined(_WIN32)
        if (error_event_) {
            CloseHandle(error_event_);
            error_event_ = nullptr;
        }
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageQueueX) ||
            GuidEquals(riid, IID_IDStorageQueueX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageQueueX*>(this);
            return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageQueueX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageQueueX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageQueueX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueRequest(const DSTORAGE_REQUEST* request) override {
        RecordOk("IDStorageQueueX::EnqueueRequest", 3);
        if (!request) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Request;
        item.request = *request;
        if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
            item.request.Source.File.Source) {
            item.request.Source.File.Source->AddRef();
        }
        bool release_file = false;
        bool flushed = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
                ++staged_request_count_;
                if (staged_request_count_ >= auto_submit_threshold_) {
                    flushed = FlushStagedLocked();
                }
            } else if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                       item.request.Source.File.Source) {
                release_file = true;
            }
        }
        if (flushed) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
        if (release_file) {
            item.request.Source.File.Source->Release();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) override {
        RecordOk("IDStorageQueueX::EnqueueStatus", 4);
        if (!statusArray) return;
        auto* impl = static_cast<DStorageStatusArrayImpl*>(statusArray);
        impl->MarkPending(index);
        impl->AddRef();

        QueueItem item{};
        item.kind = QueueItem::Kind::Status;
        item.status_array = impl;
        item.status_index = index;
        bool fail_closed = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            WaitForSlotLocked(lk);
            if (!closed_) {
                staged_.push_back(item);
            } else {
                fail_closed = true;
            }
        }
        if (fail_closed) {
            impl->MarkComplete(index, OS_DSTORAGE_E_FAIL);
            impl->Release();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL EnqueueSetEvent(void* handle) override {
        RecordOk("IDStorageQueueX::EnqueueSetEvent", 5);
        if (!handle) return;
        QueueItem item{};
        item.kind = QueueItem::Kind::Event;
        item.event_handle = handle;
        std::unique_lock<std::mutex> lk(mutex_);
        WaitForSlotLocked(lk);
        if (!closed_) {
            staged_.push_back(item);
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL Submit() override {
        RecordOk("IDStorageQueueX::Submit", 6);
        bool flushed = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_ || staged_.empty()) return;
            flushed = FlushStagedLocked();
        }
        if (flushed) {
            DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        }
    }

    void OPENSCARLETT_DSTORAGE_CALL CancelRequestsWithTag(
        uint64_t mask, uint64_t value) override {
        RecordOk("IDStorageQueueX::CancelRequestsWithTag", 7);
        std::lock_guard<std::mutex> lk(mutex_);
        auto cancel_in = [&](std::deque<QueueItem>& q, uint64_t& counter) {
            for (auto& item : q) {
                if (item.kind == QueueItem::Kind::Request &&
                    !item.canceled &&
                    (item.request.CancellationTag & mask) == value) {
                    item.canceled = true;
                    ++counter;
                }
            }
        };
        cancel_in(staged_, staged_canceled_count_);
        cancel_in(active_, submitted_canceled_count_);
        if (executing_request_ && !executing_canceled_ &&
            (executing_tag_ & mask) == value) {
            executing_canceled_ = true;
            current_cancel_requested_.store(true, std::memory_order_release);
            ++inflight_canceled_count_;
        }
    }

    void GetQueueStats(DSTORAGE_QUEUE_STATS& out) {
        std::lock_guard<std::mutex> lk(mutex_);
        out.StagedCanceled = staged_canceled_count_;
        out.SubmittedCanceled = submitted_canceled_count_;
        out.InFlightCanceled = inflight_canceled_count_;
        out.CompletedOk = completed_ok_count_.load(std::memory_order_acquire);
        out.CompletedFailed = completed_failed_count_.load(std::memory_order_acquire);
        out.CompletedCanceled = completed_canceled_count_.load(std::memory_order_acquire);
        out.PhysicalPageWrites = physical_page_writes_.load(std::memory_order_acquire);
        out.XMemDestinationWrites = xmem_destination_writes_.load(std::memory_order_acquire);
        out.StagingBufferSize = staging_size_ref_->load(std::memory_order_acquire);
        out.LastIntermediateSize = last_intermediate_size_.load(std::memory_order_acquire);
    }

    void OPENSCARLETT_DSTORAGE_CALL Close() override {
        RecordOk("IDStorageQueueX::Close", 8);
        CloseInternal();
    }

    void* OPENSCARLETT_DSTORAGE_CALL GetErrorEvent() override {
        RecordOk("IDStorageQueueX::GetErrorEvent", 9);
#if defined(_WIN32)
        return error_event_;
#else
        return nullptr;
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL RetrieveErrorRecord(DSTORAGE_ERROR_RECORD* record) override {
        RecordOk("IDStorageQueueX::RetrieveErrorRecord", 10);
        if (!record) return;
        std::lock_guard<std::mutex> lk(error_mutex_);
        *record = error_record_;
        std::memset(&error_record_, 0, sizeof(error_record_));
#if defined(_WIN32)
        if (error_event_) {
            ResetEvent(error_event_);
        }
#endif
    }

    void OPENSCARLETT_DSTORAGE_CALL Query(DSTORAGE_QUEUE_INFO* info) override {
        RecordOk("IDStorageQueueX::Query", 11);
        if (!info) return;
        std::lock_guard<std::mutex> lk(mutex_);
        info->Desc = desc_;
        const size_t used = staged_.size() + active_.size();
        info->EmptySlotCount = (used < desc_.Capacity)
                                   ? static_cast<uint16_t>(desc_.Capacity - used)
                                   : 0u;
        info->RequestCountUntilAutoSubmit =
            (staged_request_count_ < auto_submit_threshold_)
                ? static_cast<uint16_t>(auto_submit_threshold_ - staged_request_count_)
                : 0u;
    }

    // Called by DStoragePriorityScheduler under its own lock to test readiness.
    bool HasActiveWork() const noexcept {
        return active_count_.load(std::memory_order_acquire) > 0u;
    }

    // Executes exactly 1 queued item from `active_` and returns true if an item was processed.
    bool ProcessOneActiveItem() {
        QueueItem item{};
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (active_.empty()) {
                return false;
            }
            item = active_.front();
            active_.pop_front();
            active_count_.fetch_sub(1u, std::memory_order_release);
            if (item.kind == QueueItem::Kind::Request) {
                executing_request_ = true;
                executing_tag_ = item.request.CancellationTag;
                executing_canceled_ = item.canceled;
                current_cancel_requested_.store(item.canceled, std::memory_order_release);
            }
        }
        space_cv_.notify_all();

        switch (item.kind) {
            case QueueItem::Kind::Request: {
                auto& rt = GetRuntime();
                switch (PriorityToBucket(desc_.Priority)) {
                    case 0u: rt.realtime_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    case 1u: rt.high_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    case 2u: rt.normal_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                    default: rt.low_dispatched.fetch_add(1u, std::memory_order_relaxed); break;
                }

                int32_t hr = OS_DSTORAGE_S_OK;
                if (!item.canceled) {
                    hr = ExecuteRequest(item.request);
                }
                bool was_canceled = item.canceled || (hr == OS_DSTORAGE_E_CANCELLED);
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    was_canceled = was_canceled || executing_canceled_;
                    executing_request_ = false;
                    executing_tag_ = 0u;
                    executing_canceled_ = false;
                    current_cancel_requested_.store(false, std::memory_order_release);
                }
                if (was_canceled) {
                    // Cancelled requests MUST NEVER poison RetrieveErrorRecord, error_event_, or batch_hr!
                    completed_canceled_count_.fetch_add(1u, std::memory_order_relaxed);
                } else if (hr < 0) {
                    completed_failed_count_.fetch_add(1u, std::memory_order_relaxed);
                    if (batch_hr_ >= 0) batch_hr_ = hr;
                    RecordFailure(hr, item.request);
                } else {
                    completed_ok_count_.fetch_add(1u, std::memory_order_relaxed);
                }
                if (item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                    item.request.Source.File.Source) {
                    item.request.Source.File.Source->Release();
                }
                break;
            }

            case QueueItem::Kind::Status: {
                if (item.status_array) {
                    item.status_array->MarkComplete(item.status_index, batch_hr_);
                    item.status_array->Release();
                }
                batch_hr_ = OS_DSTORAGE_S_OK;
                break;
            }

            case QueueItem::Kind::Event: {
#if defined(_WIN32)
                if (item.event_handle) {
                    SetEvent(static_cast<HANDLE>(item.event_handle));
                }
#endif
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_ && active_.empty()) {
                drain_cv_.notify_all();
            }
        }
        return true;
    }

private:
    struct QueueItem {
        enum class Kind { Request, Status, Event };
        Kind kind = Kind::Request;
        bool canceled = false;
        DSTORAGE_REQUEST request{};
        DStorageStatusArrayImpl* status_array = nullptr;
        uint32_t status_index = 0;
        void* event_handle = nullptr;
    };

    bool FlushStagedLocked() {
        if (staged_.empty()) {
            staged_request_count_ = 0u;
            return false;
        }
        const size_t moved = staged_.size();
        for (auto& it : staged_) {
            active_.push_back(it);
        }
        staged_.clear();
        staged_request_count_ = 0u;
        active_count_.fetch_add(moved, std::memory_order_release);
        return true;
    }

    void WaitForSlotLocked(std::unique_lock<std::mutex>& lk) {
        while (!closed_ && (staged_.size() + active_.size()) >= desc_.Capacity) {
            bool flushed = false;
            if (!staged_.empty()) {
                flushed = FlushStagedLocked();
            }
            if (flushed) {
                lk.unlock();
                DStoragePriorityScheduler::Global().NotifyWorkAvailable();
                lk.lock();
                if (closed_ || (staged_.size() + active_.size()) < desc_.Capacity) {
                    break;
                }
            }
            space_cv_.wait(lk, [&]() {
                return closed_ || (staged_.size() + active_.size()) < desc_.Capacity;
            });
        }
    }

    int32_t AcquireSourceBytes(
        const DSTORAGE_REQUEST& req,
        size_t staging_limit,
        std::vector<uint8_t>& file_buf,
        const uint8_t*& out_ptr,
        size_t& out_size) {
        out_ptr = nullptr;
        out_size = 0u;
        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE) {
            if (!req.Source.File.Source || req.Source.File.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.File.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            auto* file_impl = static_cast<DStorageFileImpl*>(req.Source.File.Source);
            const int32_t rd_hr = file_impl->ReadAt(
                req.Source.File.Offset, req.Source.File.Size, file_buf, &current_cancel_requested_);
            if (rd_hr < 0) return rd_hr;
            out_ptr = file_buf.data();
            out_size = file_buf.size();
            return OS_DSTORAGE_S_OK;
        }
        if (req.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY) {
            if (!req.Source.Memory.Source || req.Source.Memory.Size == 0u) {
                return OS_DSTORAGE_E_INVALIDARG;
            }
            if (req.Options.CompressionFormat != DSTORAGE_COMPRESSION_FORMAT_NONE &&
                req.Source.Memory.Size > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            out_ptr = static_cast<const uint8_t*>(req.Source.Memory.Source);
            out_size = req.Source.Memory.Size;
            return OS_DSTORAGE_S_OK;
        }
        return OS_DSTORAGE_E_INVALIDARG;
    }

    int32_t DecompressToVector(
        const DSTORAGE_REQUEST& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit,
        std::vector<uint8_t>& out_data) {
        out_data.clear();
        auto is_canceled = [&]() -> bool {
            return current_cancel_requested_.load(std::memory_order_acquire);
        };
        if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;

        const uint8_t fmt_u8 = static_cast<uint8_t>(req.Options.CompressionFormat);
        CustomDecompressorEntry custom{};
        if (GetRuntime().LookupCustom(fmt_u8, custom)) {
            const size_t expected_sz = (req.UncompressedSize != 0u) ? req.UncompressedSize : src_size;
            if (expected_sz == 0u) return OS_DSTORAGE_E_INVALIDARG;
            if (expected_sz > staging_limit) return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            out_data.resize(expected_sz);
            size_t actual_sz = 0u;
            const int32_t chr = custom.callback(
                fmt_u8, src_ptr, src_size, out_data.data(), out_data.size(), &actual_sz, custom.user_context);
            if (chr < 0) {
                out_data.clear();
                return chr;
            }
            if (actual_sz > out_data.size()) {
                out_data.clear();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (req.UncompressedSize != 0u && actual_sz != req.UncompressedSize) {
                out_data.clear();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            out_data.resize(actual_sz);
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(actual_sz, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::CustomDecompress");
            return OS_DSTORAGE_S_OK;
        }

        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (req.UncompressedSize != 0u && req.UncompressedSize != src_size) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                out_data.assign(src_ptr, src_ptr + src_size);
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t header_uncompressed = 0u;
                if (!openscarlett::GDeflateIsValidStream(src_ptr, src_size) ||
                    !openscarlett::GDeflateGetUncompressedSize(src_ptr, src_size, &header_uncompressed) ||
                    header_uncompressed == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(
                    static_cast<uint32_t>(std::min<size_t>(header_uncompressed, UINT32_MAX)),
                    std::memory_order_release);
                if (header_uncompressed > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && header_uncompressed != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                out_data.resize(header_uncompressed);
                size_t out_sz = 0u;
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, out_data.data(), out_data.size(), &out_sz);
                if (res != openscarlett::GDeflateResult::Ok ||
                    (req.UncompressedSize != 0u && out_sz != req.UncompressedSize)) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                out_data.resize(out_sz);
                return OS_DSTORAGE_S_OK;
            }
            case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) return OS_DSTORAGE_E_CANCELLED;
                if (!openscarlett::BcPackDecompress(src_ptr, src_size, out_data).ok()) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (req.UncompressedSize != 0u && out_data.size() != req.UncompressedSize) {
                    out_data.clear();
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                return OS_DSTORAGE_S_OK;
            }
            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

#if defined(_WIN32)
    int32_t ExecuteGpuBufferRequest(
        const DSTORAGE_REQUEST& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit) {
        auto* dst_res = static_cast<ID3D12Resource*>(req.Destination.Memory.Buffer);
        if (!dst_res) return OS_DSTORAGE_E_INVALIDARG;

        std::vector<uint8_t> unpacked;
        const int32_t dhr = DecompressToVector(req, src_ptr, src_size, staging_limit, unpacked);
        if (dhr < 0) return dhr;
        if (current_cancel_requested_.load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        const D3D12_RESOURCE_DESC rdesc = dst_res->GetDesc();
        if (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || unpacked.size() > rdesc.Width) {
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }
        if (req.Destination.Memory.Size != 0u && unpacked.size() > req.Destination.Memory.Size) {
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }

        ID3D12Device* dev = static_cast<ID3D12Device*>(desc_.Device);
        ID3D12Device* queried_dev = nullptr;
        if (!dev) {
            if (FAILED(dst_res->GetDevice(OS_UUIDOF(ID3D12Device),
                                          reinterpret_cast<void**>(&queried_dev))) ||
                !queried_dev) {
                return OS_DSTORAGE_E_FAIL;
            }
            dev = queried_dev;
        }

        int32_t result_hr = OS_DSTORAGE_S_OK;
        if (!gpu_ctx_.EnsureInitialized(dev)) {
            result_hr = OS_DSTORAGE_E_FAIL;
        } else {
            ID3D12Resource* up_buf = nullptr;
            if (FAILED(CreateUploadBufferLocal(dev, unpacked.size(), &up_buf)) || !up_buf) {
                result_hr = OS_DSTORAGE_E_OUTOFMEMORY;
            } else {
                void* mapped = nullptr;
                const D3D12_RANGE empty_r{0, 0};
                if (FAILED(up_buf->Map(0u, &empty_r, &mapped)) || !mapped) {
                    result_hr = OS_DSTORAGE_E_FAIL;
                } else {
                    std::memcpy(mapped, unpacked.data(), unpacked.size());
                    up_buf->Unmap(0u, nullptr);

                    if (FAILED(gpu_ctx_.cmd_alloc->Reset()) ||
                        FAILED(gpu_ctx_.cmd_list->Reset(gpu_ctx_.cmd_alloc, nullptr))) {
                        result_hr = OS_DSTORAGE_E_FAIL;
                    } else {
                        D3D12_RESOURCE_BARRIER b0{};
                        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b0.Transition = {dst_res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                         D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
                        gpu_ctx_.cmd_list->ResourceBarrier(1u, &b0);
                        gpu_ctx_.cmd_list->CopyBufferRegion(dst_res, 0ull, up_buf, 0ull, unpacked.size());
                        D3D12_RESOURCE_BARRIER b1{};
                        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b1.Transition = {dst_res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
                        gpu_ctx_.cmd_list->ResourceBarrier(1u, &b1);

                        if (!gpu_ctx_.ExecuteAndWait()) {
                            result_hr = OS_DSTORAGE_E_FAIL;
                        }
                    }
                }
                up_buf->Release();
            }
        }
        if (queried_dev) queried_dev->Release();
        return result_hr;
    }

    int32_t ExecuteGpuTextureRequest(
        const DSTORAGE_REQUEST& req,
        const uint8_t* src_ptr,
        size_t src_size,
        size_t staging_limit) {
        if (!req.Destination.Memory.Buffer) return OS_DSTORAGE_E_INVALIDARG;

        OpenScarlettDStorageTextureDestDesc local_desc{};
        OpenScarlettDStorageTextureDestDesc* caller_desc = nullptr;

        if (req.Destination.Memory.Size == sizeof(OpenScarlettDStorageTextureDestDesc)) {
            auto* candidate =
                static_cast<OpenScarlettDStorageTextureDestDesc*>(req.Destination.Memory.Buffer);
            if (candidate->struct_magic == OS_DSTORAGE_TEXTURE_DEST_MAGIC &&
                candidate->struct_size == sizeof(OpenScarlettDStorageTextureDestDesc)) {
                local_desc = *candidate;
                caller_desc = candidate;
            }
        }
        if (!caller_desc) {
            // Treat Buffer directly as ID3D12Resource* for raw linear subresource 0 upload.
            local_desc.d3d12_resource = req.Destination.Memory.Buffer;
            local_desc.d3d12_device = desc_.Device;
        }

        auto* dst_tex = static_cast<ID3D12Resource*>(local_desc.d3d12_resource);
        if (!dst_tex) return OS_DSTORAGE_E_INVALIDARG;

        // 1. Decompress source stream (BCPack / GDeflate / Custom / None).
        std::vector<uint8_t> unpacked;
        const int32_t dhr = DecompressToVector(req, src_ptr, src_size, staging_limit, unpacked);
        if (dhr < 0) return dhr;
        if (current_cancel_requested_.load(std::memory_order_acquire)) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        // 2. Resolve D3D12 device and target texture geometry.
        ID3D12Device* dev = static_cast<ID3D12Device*>(
            local_desc.d3d12_device ? local_desc.d3d12_device : desc_.Device);
        ID3D12Device* queried_dev = nullptr;
        if (!dev) {
            if (FAILED(dst_tex->GetDevice(OS_UUIDOF(ID3D12Device),
                                          reinterpret_cast<void**>(&queried_dev))) ||
                !queried_dev) {
                return OS_DSTORAGE_E_FAIL;
            }
            dev = queried_dev;
        }

        const D3D12_RESOURCE_DESC rdesc = dst_tex->GetDesc();
        const uint32_t tex_w = local_desc.width ? local_desc.width : static_cast<uint32_t>(rdesc.Width);
        const uint32_t tex_h = local_desc.height ? local_desc.height : rdesc.Height;
        const uint32_t dxgi_fmt = local_desc.dxgi_format
                                      ? local_desc.dxgi_format
                                      : static_cast<uint32_t>(rdesc.Format);

        if (!gpu_ctx_.EnsureInitialized(dev)) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_FAIL;
        }

        // 3. If source layout is GFX10 tiled, route automatically through XgGpuUntiler!
        std::vector<uint8_t> linear_buf;
        const uint8_t* upload_src = unpacked.data();
        size_t upload_src_size = unpacked.size();
        uint32_t src_row_pitch = local_desc.src_row_pitch;
        bool used_gpu_untiler = false;

        if (static_cast<XG_TILE_MODE>(local_desc.src_tile_mode) != XG_TILE_MODE_LINEAR) {
            openscarlett::XgGpuSurface2DRequest ureq{};
            ureq.desc.Width = tex_w;
            ureq.desc.Height = tex_h;
            ureq.desc.MipLevels = std::max<uint32_t>(rdesc.MipLevels, local_desc.mip_level + 1u);
            ureq.desc.ArraySize = std::max<uint32_t>(rdesc.DepthOrArraySize, local_desc.array_slice + 1u);
            ureq.desc.Format = dxgi_fmt;
            ureq.desc.SampleCount = 1u;
            ureq.desc.BindFlags = XG_BIND_SHADER_RESOURCE;
            ureq.desc.TileMode = static_cast<XG_TILE_MODE>(local_desc.src_tile_mode);
            ureq.plane = local_desc.plane_slice;
            ureq.mip_level = local_desc.mip_level;
            ureq.slice_index = local_desc.array_slice;
            ureq.direction = openscarlett::XgGpuDirection::Untile;
            ureq.linear_row_pitch_bytes = local_desc.src_row_pitch;
            ureq.metadata_mode = static_cast<openscarlett::XgGpuMetadataMode>(local_desc.metadata_mode);
            ureq.console_type = static_cast<XG_CONSOLE_TYPE>(local_desc.console_type);
            for (size_t i = 0u; i < 4u; ++i) {
                ureq.clear_reg_value[i] = local_desc.clear_reg_value[i];
            }

            openscarlett::XgGpuUntileParams uparams{};
            uint64_t linear_bytes = 0ull;
            uint64_t tiled_bytes = 0ull;
            if (!openscarlett::BuildXgGpuUntileParams2D(ureq, uparams, &linear_bytes, &tiled_bytes).ok()) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            if (unpacked.size() < tiled_bytes) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }

            linear_buf.assign(static_cast<size_t>(linear_bytes), 0u);
            std::vector<uint8_t> meta_copy;
            const std::vector<uint8_t>* meta_ptr = nullptr;
            if (local_desc.metadata_buffer && local_desc.metadata_size_bytes > 0u) {
                const auto* mb = static_cast<const uint8_t*>(local_desc.metadata_buffer);
                meta_copy.assign(mb, mb + local_desc.metadata_size_bytes);
                meta_ptr = &meta_copy;
            }

            bool untile_ok = false;
            if (local_desc.prefer_gpu_untiler != 0u && gpu_ctx_.EnsureUntilerReady()) {
                if (gpu_ctx_.untiler.ExecuteD3D12(uparams, unpacked, linear_buf, meta_ptr).ok()) {
                    untile_ok = true;
                    used_gpu_untiler = true;
                }
            }
            if (!untile_ok && local_desc.allow_cpu_untile_fallback != 0u) {
                if (openscarlett::xg::UntileSurface2DPlane(
                        ureq.desc,
                        ureq.plane,
                        ureq.mip_level,
                        ureq.slice_index,
                        unpacked.data(),
                        unpacked.size(),
                        linear_buf.data(),
                        uparams.linear_row_pitch)) {
                    untile_ok = true;
                }
            }
            if (!untile_ok) {
                if (queried_dev) queried_dev->Release();
                return OS_DSTORAGE_E_FAIL;
            }
            upload_src = linear_buf.data();
            upload_src_size = linear_buf.size();
            src_row_pitch = uparams.linear_row_pitch;
        }

        if (current_cancel_requested_.load(std::memory_order_acquire)) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_CANCELLED;
        }

        // 4. Upload linear texels/BC blocks into target ID3D12Resource texture via CopyTextureRegion.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0u;
        UINT64 row_size_bytes = 0ull;
        UINT64 total_upload_bytes = 0ull;
        dev->GetCopyableFootprints(
            &rdesc,
            local_desc.subresource_index,
            1u,
            0ull,
            &footprint,
            &num_rows,
            &row_size_bytes,
            &total_upload_bytes);

        if (num_rows == 0u || row_size_bytes == 0ull || total_upload_bytes == 0ull) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_INVALIDARG;
        }
        const uint64_t effective_src_pitch =
            (src_row_pitch != 0u) ? static_cast<uint64_t>(src_row_pitch) : row_size_bytes;
        if (effective_src_pitch < row_size_bytes ||
            effective_src_pitch * static_cast<uint64_t>(num_rows) > upload_src_size) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
        }

        ID3D12Resource* up_buf = nullptr;
        if (FAILED(CreateUploadBufferLocal(dev, total_upload_bytes, &up_buf)) || !up_buf) {
            if (queried_dev) queried_dev->Release();
            return OS_DSTORAGE_E_OUTOFMEMORY;
        }

        int32_t result_hr = OS_DSTORAGE_S_OK;
        void* mapped = nullptr;
        const D3D12_RANGE empty_r{0, 0};
        if (FAILED(up_buf->Map(0u, &empty_r, &mapped)) || !mapped) {
            result_hr = OS_DSTORAGE_E_FAIL;
        } else {
            auto* dst_base = static_cast<uint8_t*>(mapped) + footprint.Offset;
            for (UINT row = 0u; row < num_rows; ++row) {
                std::memcpy(
                    dst_base + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    upload_src + static_cast<size_t>(row) * static_cast<size_t>(effective_src_pitch),
                    static_cast<size_t>(row_size_bytes));
            }
            up_buf->Unmap(0u, nullptr);

            if (FAILED(gpu_ctx_.cmd_alloc->Reset()) ||
                FAILED(gpu_ctx_.cmd_list->Reset(gpu_ctx_.cmd_alloc, nullptr))) {
                result_hr = OS_DSTORAGE_E_FAIL;
            } else {
                const auto init_state = static_cast<D3D12_RESOURCE_STATES>(local_desc.initial_resource_state);
                const auto final_state = static_cast<D3D12_RESOURCE_STATES>(local_desc.final_resource_state);
                if (init_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b0{};
                    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b0.Transition = {dst_tex, local_desc.subresource_index,
                                     init_state, D3D12_RESOURCE_STATE_COPY_DEST};
                    gpu_ctx_.cmd_list->ResourceBarrier(1u, &b0);
                }

                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = dst_tex;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = local_desc.subresource_index;

                D3D12_TEXTURE_COPY_LOCATION src_loc{};
                src_loc.pResource = up_buf;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprint;

                gpu_ctx_.cmd_list->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);

                if (final_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b1{};
                    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b1.Transition = {dst_tex, local_desc.subresource_index,
                                     D3D12_RESOURCE_STATE_COPY_DEST, final_state};
                    gpu_ctx_.cmd_list->ResourceBarrier(1u, &b1);
                }

                if (!gpu_ctx_.ExecuteAndWait()) {
                    result_hr = OS_DSTORAGE_E_FAIL;
                }
            }
        }

        up_buf->Release();
        if (queried_dev) queried_dev->Release();

        if (result_hr == OS_DSTORAGE_S_OK) {
            GetRuntime().gpu_texture_uploads.fetch_add(1u, std::memory_order_relaxed);
            if (used_gpu_untiler) {
                GetRuntime().gpu_untiled_uploads.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::BCPackToXgGpuUntilerUpload");
            } else {
                RecordOk("DirectStorage::TextureUpload");
            }
            if (caller_desc) {
                caller_desc->out_used_gpu_untiler = used_gpu_untiler ? 1u : 0u;
                caller_desc->out_decompressed_bytes = static_cast<uint32_t>(unpacked.size());
                caller_desc->out_untiled_bytes = static_cast<uint32_t>(upload_src_size);
                caller_desc->out_uploaded_bytes = static_cast<uint32_t>(row_size_bytes * num_rows);
            }
        }
        return result_hr;
    }
#endif

    int32_t ExecuteRequest(const DSTORAGE_REQUEST& req) {
        auto is_canceled = [&]() -> bool {
            return current_cancel_requested_.load(std::memory_order_acquire);
        };
        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        const size_t staging_limit =
            static_cast<size_t>(staging_size_ref_->load(std::memory_order_acquire));

        // Handle GPU texture & buffer destinations (automatic BCPack/GDeflate -> XgGpuUntiler upload).
        if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_TILES ||
            req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
#if defined(_WIN32)
            std::vector<uint8_t> file_buf;
            const uint8_t* src_ptr = nullptr;
            size_t src_size = 0u;
            const int32_t src_hr = AcquireSourceBytes(req, staging_limit, file_buf, src_ptr, src_size);
            if (src_hr < 0) return src_hr;
            if (req.Options.DestinationType == DSTORAGE_REQUEST_DESTINATION_BUFFER) {
                return ExecuteGpuBufferRequest(req, src_ptr, src_size, staging_limit);
            }
            return ExecuteGpuTextureRequest(req, src_ptr, src_size, staging_limit);
#else
            return OS_DSTORAGE_E_INVALIDARG;
#endif
        }

        if (req.Options.DestinationType != DSTORAGE_REQUEST_DESTINATION_MEMORY ||
            !req.Destination.Memory.Buffer || req.Destination.Memory.Size == 0) {
            return OS_DSTORAGE_E_INVALIDARG;
        }

        auto* dst_ptr = static_cast<uint8_t*>(req.Destination.Memory.Buffer);
        const size_t dst_cap = req.Destination.Memory.Size;
        DestinationClassification dst_class{};
        const int32_t dst_hr = ValidateAndClassifyDestination(dst_ptr, dst_cap, dst_class);
        if (dst_hr < 0) {
            return dst_hr;
        }

        std::vector<uint8_t> file_buf;
        const uint8_t* src_ptr = nullptr;
        size_t src_size = 0;
        const int32_t src_hr = AcquireSourceBytes(req, staging_limit, file_buf, src_ptr, src_size);
        if (src_hr < 0) {
            return src_hr;
        }

        if (is_canceled()) {
            return OS_DSTORAGE_E_CANCELLED;
        }

        auto record_destination_write = [&]() {
            if (dst_class.is_physical_page) {
                physical_page_writes_.fetch_add(1u, std::memory_order_relaxed);
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            if (dst_class.is_xmem) {
                xmem_destination_writes_.fetch_add(1u, std::memory_order_relaxed);
            }
        };

        // Check registered custom decompressor callbacks first.
        const uint8_t fmt_u8 = static_cast<uint8_t>(req.Options.CompressionFormat);
        CustomDecompressorEntry custom{};
        if (GetRuntime().LookupCustom(fmt_u8, custom)) {
            if (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit) {
                return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
            }
            if (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap) {
                return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
            }
            size_t out_sz = 0u;
            const int32_t chr = custom.callback(
                fmt_u8, src_ptr, src_size, dst_ptr, dst_cap, &out_sz, custom.user_context);
            if (chr < 0) return chr;
            if (out_sz > dst_cap) return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
            if (req.UncompressedSize != 0u && out_sz != req.UncompressedSize) {
                return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
            }
            last_intermediate_size_.store(
                static_cast<uint32_t>(std::min<size_t>(out_sz, UINT32_MAX)),
                std::memory_order_release);
            GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
            RecordOk("DirectStorage::CustomDecompress");
            record_destination_write();
            return OS_DSTORAGE_S_OK;
        }

        switch (req.Options.CompressionFormat) {
            case DSTORAGE_COMPRESSION_FORMAT_NONE: {
                if (src_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && req.UncompressedSize != src_size) {
                    return OS_DSTORAGE_E_INVALIDARG;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                constexpr size_t kCopyChunk = 65536u;
                size_t copied = 0u;
                while (copied < src_size) {
                    if (is_canceled()) {
                        return OS_DSTORAGE_E_CANCELLED;
                    }
                    const size_t step = std::min(src_size - copied, kCopyChunk);
                    std::memcpy(dst_ptr + copied, src_ptr + copied, step);
                    copied += step;
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
                size_t header_uncompressed = 0u;
                if (!openscarlett::GDeflateIsValidStream(src_ptr, src_size) ||
                    !openscarlett::GDeflateGetUncompressedSize(src_ptr, src_size, &header_uncompressed) ||
                    header_uncompressed == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(
                    static_cast<uint32_t>(std::min<size_t>(header_uncompressed, UINT32_MAX)),
                    std::memory_order_release);
                if (header_uncompressed > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (header_uncompressed > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && header_uncompressed != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                size_t out_size = 0u;
                if (dst_class.is_physical_page) {
                    const auto res = openscarlett::GDeflateDecompress(
                        src_ptr, src_size, dst_ptr, dst_cap, &out_size);
                    if (res != openscarlett::GDeflateResult::Ok) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                        return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                    }
                    record_destination_write();
                    return OS_DSTORAGE_S_OK;
                }

                std::vector<uint8_t> staging(header_uncompressed);
                const auto res = openscarlett::GDeflateDecompress(
                    src_ptr, src_size, staging.data(), staging.size(), &out_size);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (out_size > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && out_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (out_size > 0u) {
                    std::memcpy(dst_ptr, staging.data(), out_size);
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
            case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
                openscarlett::BcPackHeader hdr{};
                if (!openscarlett::BcPackParseHeader(src_ptr, src_size, hdr).ok() ||
                    hdr.original_size == 0u) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                last_intermediate_size_.store(hdr.original_size, std::memory_order_release);
                if (static_cast<size_t>(hdr.original_size) > staging_limit ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > staging_limit)) {
                    return OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL;
                }
                if (static_cast<size_t>(hdr.original_size) > dst_cap ||
                    (req.UncompressedSize != 0u && req.UncompressedSize > dst_cap)) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && hdr.original_size != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }

                std::vector<uint8_t> unpacked;
                const auto st = openscarlett::BcPackDecompress(src_ptr, src_size, unpacked);
                if (!st.ok()) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (unpacked.size() > dst_cap) {
                    return OS_DSTORAGE_E_REQUEST_TOO_LARGE;
                }
                if (req.UncompressedSize != 0u && unpacked.size() != req.UncompressedSize) {
                    return OS_DSTORAGE_E_DECOMPRESSION_ERROR;
                }
                if (is_canceled()) {
                    return OS_DSTORAGE_E_CANCELLED;
                }
                if (!unpacked.empty()) {
                    std::memcpy(dst_ptr, unpacked.data(), unpacked.size());
                }
                record_destination_write();
                return OS_DSTORAGE_S_OK;
            }

            default:
                return OS_DSTORAGE_E_INVALIDARG;
        }
    }

    void RecordFailure(int32_t hr, const DSTORAGE_REQUEST& req) {
        std::lock_guard<std::mutex> lk(error_mutex_);
        if (error_record_.FailureCount == 0) {
            error_record_.FirstFailure.HResult = hr;
            error_record_.FirstFailure.CommandType = 0;
            error_record_.FirstFailure.FailedRequest = req;
        }
        ++error_record_.FailureCount;
#if defined(_WIN32)
        if (error_event_) {
            SetEvent(error_event_);
        }
#endif
    }

    void CloseInternal() {
        std::deque<QueueItem> leftover_staged;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (closed_) return;
            closed_ = true;
            leftover_staged.swap(staged_);
            staged_request_count_ = 0u;
            space_cv_.notify_all();
        }
        for (auto& item : leftover_staged) {
            if (item.kind == QueueItem::Kind::Request &&
                item.request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_FILE &&
                item.request.Source.File.Source) {
                item.request.Source.File.Source->Release();
            } else if (item.kind == QueueItem::Kind::Status && item.status_array) {
                item.status_array->MarkComplete(item.status_index, OS_DSTORAGE_E_FAIL);
                item.status_array->Release();
            }
        }

        // Wait until all already-submitted active items drain cleanly before unregistering.
        DStoragePriorityScheduler::Global().NotifyWorkAvailable();
        {
            std::unique_lock<std::mutex> lk(mutex_);
            drain_cv_.wait(lk, [&]() { return active_.empty(); });
        }
        DStoragePriorityScheduler::Global().UnregisterQueue(this);

#if defined(_WIN32)
        gpu_ctx_.Reset();
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    DSTORAGE_QUEUE_DESC desc_{};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_ref_;
    uint16_t auto_submit_threshold_ = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;
    std::string name_storage_;

    std::mutex mutex_;
    std::condition_variable space_cv_;
    std::condition_variable drain_cv_;
    bool closed_ = false;
    uint32_t staged_request_count_ = 0u;
    std::atomic<size_t> active_count_{0u};
    int32_t batch_hr_ = OS_DSTORAGE_S_OK;
    bool executing_request_ = false;
    uint64_t executing_tag_ = 0u;
    bool executing_canceled_ = false;
    std::atomic<bool> current_cancel_requested_{false};
    uint64_t staged_canceled_count_ = 0u;
    uint64_t submitted_canceled_count_ = 0u;
    uint64_t inflight_canceled_count_ = 0u;
    std::atomic<uint64_t> completed_ok_count_{0u};
    std::atomic<uint64_t> completed_failed_count_{0u};
    std::atomic<uint64_t> completed_canceled_count_{0u};
    std::atomic<uint64_t> physical_page_writes_{0u};
    std::atomic<uint64_t> xmem_destination_writes_{0u};
    std::atomic<uint32_t> last_intermediate_size_{0u};
    std::deque<QueueItem> staged_;
    std::deque<QueueItem> active_;

#if defined(_WIN32)
    DStorageGpuUploadContext gpu_ctx_;
#endif

    std::mutex error_mutex_;
    DSTORAGE_ERROR_RECORD error_record_{};
#if defined(_WIN32)
    HANDLE error_event_ = nullptr;
#endif
};

// ===========================================================================
// DStoragePriorityScheduler Method Definitions
// ===========================================================================
void DStoragePriorityScheduler::RegisterQueue(DStorageQueueImpl* q, DSTORAGE_PRIORITY priority) {
    std::lock_guard<std::mutex> lk(mutex_);
    slots_.push_back(Slot{q, PriorityToBucket(priority), false, false});
    if (!worker_running_) {
        stop_ = false;
        worker_running_ = true;
        worker_ = std::thread(&DStoragePriorityScheduler::RunLoop, this);
    }
    work_cv_.notify_one();
}

void DStoragePriorityScheduler::UnregisterQueue(DStorageQueueImpl* q) {
    std::thread to_join;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        for (auto& s : slots_) {
            if (s.queue == q) {
                s.removing = true;
                break;
            }
        }
        idle_cv_.wait(lk, [&]() {
            for (const auto& s : slots_) {
                if (s.queue == q) return !s.in_progress;
            }
            return true;
        });
        slots_.erase(
            std::remove_if(slots_.begin(), slots_.end(),
                           [q](const Slot& s) { return s.queue == q; }),
            slots_.end());

        if (slots_.empty() && worker_running_) {
            stop_ = true;
            worker_running_ = false;
            work_cv_.notify_all();
            if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
                to_join = std::move(worker_);
            } else if (worker_.joinable()) {
                worker_.detach();
            }
        }
    }
    if (to_join.joinable()) {
        to_join.join();
    }
}

void DStoragePriorityScheduler::NotifyWorkAvailable() {
    // Acquire scheduler mutex_ before notifying so the producer cannot race
    // between RunLoop's pick_next() check and entering work_cv_.wait() (lost wakeup).
    {
        std::lock_guard<std::mutex> lk(mutex_);
    }
    work_cv_.notify_one();
}

void DStoragePriorityScheduler::RunLoop() {
    for (;;) {
        DStorageQueueImpl* chosen = nullptr;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            auto pick_next = [&]() -> DStorageQueueImpl* {
                // Strictly scan REALTIME(0) > HIGH(1) > NORMAL(2) > LOW(3),
                // round-robin within each priority bucket for fairness.
                for (size_t b = 0u; b < 4u; ++b) {
                    const size_t n = slots_.size();
                    if (n == 0u) continue;
                    const size_t start = rr_cursor_[b] % n;
                    for (size_t off = 0u; off < n; ++off) {
                        const size_t idx = (start + off) % n;
                        Slot& s = slots_[idx];
                        if (s.bucket == b && !s.removing && !s.in_progress && s.queue->HasActiveWork()) {
                            s.in_progress = true;
                            rr_cursor_[b] = (idx + 1u) % n;
                            return s.queue;
                        }
                    }
                }
                return nullptr;
            };

            work_cv_.wait(lk, [&]() {
                if (stop_) return true;
                chosen = pick_next();
                return chosen != nullptr;
            });

            if (stop_ && !chosen) {
                return;
            }
        }

        if (chosen) {
            chosen->ProcessOneActiveItem();
            std::lock_guard<std::mutex> lk(mutex_);
            for (auto& s : slots_) {
                if (s.queue == chosen) {
                    s.in_progress = false;
                    if (s.removing) {
                        idle_cv_.notify_all();
                    }
                    break;
                }
            }
        }
    }
}

// ===========================================================================
// IDStorageFactoryX Implementation
// ===========================================================================
class DStorageFactoryImpl final : public IDStorageFactoryX {
public:
    int32_t OPENSCARLETT_DSTORAGE_CALL QueryInterface(
        const DStorageGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_DStorage_IUnknown) ||
            GuidEquals(riid, IID_IDStorageFactoryX) ||
            GuidEquals(riid, IID_IDStorageFactoryX1)) {
            AddRef();
            *ppvObject = static_cast<IDStorageFactoryX*>(this);
            return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_S_OK, 0);
        }
        return RecordHr("IDStorageFactoryX::QueryInterface", OS_DSTORAGE_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL AddRef() override {
        RecordOk("IDStorageFactoryX::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_DSTORAGE_CALL Release() override {
        RecordOk("IDStorageFactoryX::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc, const DStorageGuid* riid, void** ppv) override {
        if (!desc || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_INVALIDARG, 3);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageQueueX) &&
            !GuidEquals(riid, IID_IDStorageQueueX1) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_NOINTERFACE, 3);
        }
        auto* q = new (std::nothrow) DStorageQueueImpl(*desc, staging_size_);
        if (!q) {
            return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_E_OUTOFMEMORY, 3);
        }
        *ppv = static_cast<IDStorageQueueX*>(q);
        return RecordHr("IDStorageFactoryX::CreateQueue", OS_DSTORAGE_S_OK, 3);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL OpenFile(
        const wchar_t* path, const DStorageGuid* riid, void** ppv) override {
        if (!path || !*path || !ppv) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_INVALIDARG, 4);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageFileX) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_NOINTERFACE, 4);
        }
#if defined(_WIN32)
        HANDLE h = CreateFileW(
            path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FILE_NOT_FOUND, 4);
        }
        auto* f = new (std::nothrow) DStorageFileImpl(h);
        if (!f) {
            CloseHandle(h);
            return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_OUTOFMEMORY, 4);
        }
        *ppv = static_cast<IDStorageFileX*>(f);
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_S_OK, 4);
#else
        return RecordHr("IDStorageFactoryX::OpenFile", OS_DSTORAGE_E_FAIL, 4);
#endif
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL CreateStatusArray(
        uint32_t capacity, const char* /*name*/, const DStorageGuid* riid, void** ppv) override {
        if (capacity == 0 || !ppv) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_INVALIDARG, 5);
        }
        *ppv = nullptr;
        if (riid && !GuidEquals(riid, IID_IDStorageStatusArrayX) &&
            !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_NOINTERFACE, 5);
        }
        auto* arr = new (std::nothrow) DStorageStatusArrayImpl(capacity);
        if (!arr) {
            return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_E_OUTOFMEMORY, 5);
        }
        *ppv = static_cast<IDStorageStatusArrayX*>(arr);
        return RecordHr("IDStorageFactoryX::CreateStatusArray", OS_DSTORAGE_S_OK, 5);
    }

    void OPENSCARLETT_DSTORAGE_CALL SetDebugFlags(uint32_t flags) override {
        RecordOk("IDStorageFactoryX::SetDebugFlags", 6);
        debug_flags_.store(flags, std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_DSTORAGE_CALL SetStagingBufferSize(uint32_t size) override {
        if (size < DSTORAGE_MIN_STAGING_BUFFER_SIZE || size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
            return RecordHr("IDStorageFactoryX::SetStagingBufferSize",
                            OS_DSTORAGE_E_STAGING_BUFFER_INVALID, 7);
        }
        staging_size_->store(size, std::memory_order_release);
        return RecordHr("IDStorageFactoryX::SetStagingBufferSize", OS_DSTORAGE_S_OK, 7);
    }

    uint32_t GetStagingBufferSizeValue() const {
        return staging_size_->load(std::memory_order_acquire);
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> debug_flags_{0};
    std::shared_ptr<std::atomic<uint32_t>> staging_size_{
        std::make_shared<std::atomic<uint32_t>>(DSTORAGE_DEFAULT_STAGING_BUFFER_SIZE)};
};

struct GlobalDStorageConfig {
    std::mutex mutex;
    DSTORAGE_CONFIGURATION1 config{};
};

GlobalDStorageConfig& GetConfig() {
    static GlobalDStorageConfig cfg;
    return cfg;
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageGetFactory(const DStorageGuid* riid, void** ppv) {
    if (!ppv) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_POINTER);
    }
    *ppv = nullptr;
    if (riid && !GuidEquals(riid, IID_IDStorageFactoryX) &&
        !GuidEquals(riid, IID_IDStorageFactoryX1) &&
        !GuidEquals(riid, IID_OS_DStorage_IUnknown)) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_NOINTERFACE);
    }
    auto* f = new (std::nothrow) DStorageFactoryImpl();
    if (!f) {
        return RecordHr("DStorageGetFactory", OS_DSTORAGE_E_OUTOFMEMORY);
    }
    *ppv = static_cast<IDStorageFactoryX*>(f);
    return RecordHr("DStorageGetFactory", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration(
    const DSTORAGE_CONFIGURATION* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config.NumSubmitThreads = configuration->NumSubmitThreads;
    cfg.config.NumBuiltInCpuDecompressionThreads = configuration->NumBuiltInCpuDecompressionThreads;
    cfg.config.ForceMappingLayer = configuration->ForceMappingLayer;
    cfg.config.DisableBypassIO = configuration->DisableBypassIO;
    cfg.config.DisableTelemetry = configuration->DisableTelemetry;
    cfg.config.DisableGpuDecompressionMetacommand = configuration->DisableGpuDecompressionMetacommand;
    cfg.config.DisableGpuDecompression = configuration->DisableGpuDecompression;
    return RecordHr("DStorageSetConfiguration", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL DStorageSetConfiguration1(
    const DSTORAGE_CONFIGURATION1* configuration) {
    if (!configuration) {
        return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& cfg = GetConfig();
    std::lock_guard<std::mutex> lk(cfg.mutex);
    cfg.config = *configuration;
    return RecordHr("DStorageSetConfiguration1", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetQueueStats(
    IDStorageQueueX* queue, DSTORAGE_QUEUE_STATS* outStats) {
    if (!queue || !outStats) {
        return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_E_INVALIDARG);
    }
    std::memset(outStats, 0, sizeof(*outStats));
    auto* impl = static_cast<DStorageQueueImpl*>(queue);
    impl->GetQueueStats(*outStats);
    return RecordHr("OpenScarlettDStorageGetQueueStats", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetStagingBufferSize(
    IDStorageFactoryX* factory, uint32_t* outSize) {
    if (!factory || !outSize) {
        return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_E_INVALIDARG);
    }
    auto* impl = static_cast<DStorageFactoryImpl*>(factory);
    *outSize = impl->GetStagingBufferSizeValue();
    return RecordHr("OpenScarlettDStorageGetStagingBufferSize", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageRegisterCustomDecompressor(
    uint8_t format,
    PfnOpenScarlettDStorageCustomDecompress callback,
    void* user_context) {
    if (!callback) {
        return RecordHr("OpenScarlettDStorageRegisterCustomDecompressor", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.custom_mutex);
    rt.custom_decompressors[format] = CustomDecompressorEntry{callback, user_context};
    return RecordHr("OpenScarlettDStorageRegisterCustomDecompressor", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageUnregisterCustomDecompressor(uint8_t format) {
    auto& rt = GetRuntime();
    std::lock_guard<std::mutex> lk(rt.custom_mutex);
    rt.custom_decompressors.erase(format);
    return RecordHr("OpenScarlettDStorageUnregisterCustomDecompressor", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageGetPriorityStats(
    OpenScarlettDStoragePriorityStats* out_stats) {
    if (!out_stats) {
        return RecordHr("OpenScarlettDStorageGetPriorityStats", OS_DSTORAGE_E_INVALIDARG);
    }
    auto& rt = GetRuntime();
    out_stats->realtime_dispatched = rt.realtime_dispatched.load(std::memory_order_acquire);
    out_stats->high_dispatched = rt.high_dispatched.load(std::memory_order_acquire);
    out_stats->normal_dispatched = rt.normal_dispatched.load(std::memory_order_acquire);
    out_stats->low_dispatched = rt.low_dispatched.load(std::memory_order_acquire);
    out_stats->custom_decompressed = rt.custom_decompressed.load(std::memory_order_acquire);
    out_stats->gpu_texture_uploads = rt.gpu_texture_uploads.load(std::memory_order_acquire);
    out_stats->gpu_untiled_uploads = rt.gpu_untiled_uploads.load(std::memory_order_acquire);
    return RecordHr("OpenScarlettDStorageGetPriorityStats", OS_DSTORAGE_S_OK);
}

int32_t OPENSCARLETT_DSTORAGE_CALL OpenScarlettDStorageDecompressWithIntermediate(
    DSTORAGE_COMPRESSION_FORMAT format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t intermediate_size,
    size_t* out_decompressed_size) {
    if (out_decompressed_size) {
        *out_decompressed_size = 0u;
    }
    if (!src || !dst || src_size == 0u || dst_size == 0u) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_E_INVALIDARG);
    }
    if (intermediate_size == 0u || intermediate_size > DSTORAGE_MAX_STAGING_BUFFER_SIZE) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                        OS_DSTORAGE_E_STAGING_BUFFER_INVALID);
    }

    DestinationClassification dst_class{};
    const int32_t dst_hr = ValidateAndClassifyDestination(dst, dst_size, dst_class);
    if (dst_hr < 0) {
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", dst_hr);
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src);
    auto* dst_bytes = static_cast<uint8_t*>(dst);

    // Check custom decompressor registry first.
    CustomDecompressorEntry custom{};
    if (GetRuntime().LookupCustom(static_cast<uint8_t>(format), custom)) {
        std::vector<uint8_t> staging(intermediate_size);
        size_t out_sz = 0u;
        const int32_t chr = custom.callback(
            static_cast<uint8_t>(format),
            src_bytes, src_size,
            staging.data(), staging.size(),
            &out_sz,
            custom.user_context);
        if (chr < 0) {
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", chr);
        }
        if (out_sz > dst_size) {
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                            OS_DSTORAGE_E_REQUEST_TOO_LARGE);
        }
        if (out_sz > 0u) {
            std::memcpy(dst_bytes, staging.data(), out_sz);
        }
        if (out_decompressed_size) *out_decompressed_size = out_sz;
        GetRuntime().custom_decompressed.fetch_add(1u, std::memory_order_relaxed);
        return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
    }

    switch (format) {
        case DSTORAGE_COMPRESSION_FORMAT_NONE: {
            if (intermediate_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < src_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::memcpy(dst_bytes, src_bytes, src_size);
            if (out_decompressed_size) {
                *out_decompressed_size = src_size;
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE: {
            size_t required = 0u;
            if (!openscarlett::GDeflateIsValidStream(src_bytes, src_size) ||
                !openscarlett::GDeflateGetUncompressedSize(src_bytes, src_size, &required) ||
                required == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < required) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            size_t out_sz = 0u;
            if (dst_class.is_physical_page) {
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, dst_bytes, dst_size, &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            } else {
                std::vector<uint8_t> staging(intermediate_size);
                const auto res = openscarlett::GDeflateDecompress(
                    src_bytes, src_size, staging.data(), staging.size(), &out_sz);
                if (res != openscarlett::GDeflateResult::Ok) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_DECOMPRESSION_ERROR);
                }
                if (out_sz > dst_size) {
                    return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                    OS_DSTORAGE_E_REQUEST_TOO_LARGE);
                }
                std::memcpy(dst_bytes, staging.data(), out_sz);
            }
            if (out_decompressed_size) {
                *out_decompressed_size = out_sz;
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        case DSTORAGE_COMPRESSION_FORMAT_ZLIB:
        case DSTORAGE_COMPRESSION_FORMAT_BCPACK: {
            openscarlett::BcPackHeader hdr{};
            if (!openscarlett::BcPackParseHeader(src_bytes, src_size, hdr).ok() ||
                hdr.original_size == 0u) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (intermediate_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_STAGING_BUFFER_TOO_SMALL);
            }
            if (dst_size < static_cast<size_t>(hdr.original_size)) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            std::vector<uint8_t> unpacked;
            if (!openscarlett::BcPackDecompress(src_bytes, src_size, unpacked).ok()) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_DECOMPRESSION_ERROR);
            }
            if (unpacked.size() > dst_size) {
                return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                                OS_DSTORAGE_E_REQUEST_TOO_LARGE);
            }
            if (!unpacked.empty()) {
                std::memcpy(dst_bytes, unpacked.data(), unpacked.size());
            }
            if (out_decompressed_size) {
                *out_decompressed_size = unpacked.size();
            }
            if (dst_class.is_physical_page) {
                RecordOk("DirectStorage::DecompressToPhysicalPages");
            }
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate", OS_DSTORAGE_S_OK);
        }

        default:
            return RecordHr("OpenScarlettDStorageDecompressWithIntermediate",
                            OS_DSTORAGE_E_INVALIDARG);
    }
}

}  // extern "C"
