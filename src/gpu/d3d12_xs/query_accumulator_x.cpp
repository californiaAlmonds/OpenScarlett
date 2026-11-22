#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "openscarlett/gpu/query_accumulator_x.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"

#if defined(_WIN32)
#include <d3dcompiler.h>
#define OS_VKAPI_PTR __stdcall
#else
#define OS_VKAPI_PTR
#endif

namespace openscarlett::d3d12_xs {

namespace {

#if defined(_WIN32)

#if defined(_MSC_VER)
#define OS_QA_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_QA_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_QA_UUIDOF(T) IID_##T
#endif

template <typename T>
class LocalComPtr {
public:
    LocalComPtr() = default;
    ~LocalComPtr() { Reset(); }

    LocalComPtr(const LocalComPtr&) = delete;
    LocalComPtr& operator=(const LocalComPtr&) = delete;

    LocalComPtr(LocalComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    LocalComPtr& operator=(LocalComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }

    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*,
    D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob**,
    ID3DBlob**);

// SM 5.0 compute shader performing carry-safe 64-bit accumulation (Overwrite/Add/Max)
// over an arbitrary number of resolved 64-bit query slots.
constexpr const char* kAccumulateU64Hlsl = R"(
cbuffer AccumParams : register(b0) {
    uint count;
    uint op;
    uint dst_byte_off;
    uint pad0;
};
ByteAddressBuffer src_queries : register(t0);
RWByteAddressBuffer dst_buf : register(u0);

uint2 AddU64(uint2 a, uint2 b) {
    uint lo = a.x + b.x;
    uint carry = (lo < a.x) ? 1u : 0u;
    uint hi = a.y + b.y + carry;
    return uint2(lo, hi);
}

bool GreaterU64(uint2 a, uint2 b) {
    return (a.y > b.y) || (a.y == b.y && a.x > b.x);
}

[numthreads(1, 1, 1)]
void main() {
    uint2 acc = uint2(0u, 0u);
    if (op == 1u || op == 2u) {
        acc = dst_buf.Load2(dst_byte_off);
    }
    for (uint i = 0u; i < count; ++i) {
        uint2 v = src_queries.Load2(i * 8u);
        if (op == 2u) {
            if (GreaterU64(v, acc)) acc = v;
        } else {
            acc = AddU64(acc, v);
        }
    }
    dst_buf.Store2(dst_byte_off, acc);
}
)";

Status CreateBuffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    uint64_t byte_size,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    LocalComPtr<ID3D12Resource>& out_buf) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0ull;
    rd.Width = std::max<uint64_t>(8ull, byte_size);
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.SampleDesc.Quality = 0u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    const HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
        OS_QA_UUIDOF(ID3D12Resource), out_buf.PutVoid());
    if (FAILED(hr) || !out_buf) {
        std::ostringstream oss;
        oss << "CreateCommittedResource(query buf, size=" << byte_size << ") failed hr=0x"
            << std::hex << static_cast<uint32_t>(hr);
        return Status::Error(StatusCode::DeviceError, oss.str());
    }
    return Status::Ok();
}

void EmitTransition(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before_state,
    D3D12_RESOURCE_STATES after_state) noexcept {
    if (!cmd_list || !resource || before_state == after_state) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before_state;
    b.Transition.StateAfter = after_state;
    cmd_list->ResourceBarrier(1u, &b);
}

void EmitUavBarrier(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* resource) noexcept {
    if (!cmd_list) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = resource;
    cmd_list->ResourceBarrier(1u, &b);
}

#endif  // _WIN32

void FinalizeCalibrationStatistics(GpuClockCalibrationResult& res) noexcept {
    if (res.samples.empty()) return;

    double min_bracket_us = std::numeric_limits<double>::max();
    double max_bracket_us = 0.0;
    double min_offset_s = std::numeric_limits<double>::max();
    double max_offset_s = -std::numeric_limits<double>::max();
    size_t best_idx = 0u;

    for (size_t i = 0u; i < res.samples.size(); ++i) {
        const auto& s = res.samples[i];
        const double bracket_us = static_cast<double>(s.max_deviation_ns) * 1e-3;
        if (bracket_us < min_bracket_us) {
            min_bracket_us = bracket_us;
            best_idx = i;
        }
        if (bracket_us > max_bracket_us) {
            max_bracket_us = bracket_us;
        }
        if (s.offset_seconds < min_offset_s) min_offset_s = s.offset_seconds;
        if (s.offset_seconds > max_offset_s) max_offset_s = s.offset_seconds;
    }

    res.anchor = res.samples[best_idx];
    res.estimated_offset_seconds = res.anchor.offset_seconds;
    res.min_bracket_error_us = min_bracket_us;
    res.max_bracket_error_us = max_bracket_us;
    res.max_offset_spread_us = (max_offset_s - min_offset_s) * 1e6;

    res.estimated_drift_ppm = 0.0;
    if (res.samples.size() >= 2u) {
        const auto& first = res.samples.front();
        const auto& last = res.samples.back();
        const double dt_cpu = last.cpu_time_seconds - first.cpu_time_seconds;
        const double dt_gpu = last.gpu_time_seconds - first.gpu_time_seconds;
        if (dt_cpu > 1e-6) {
            res.estimated_drift_ppm = ((dt_gpu - dt_cpu) / dt_cpu) * 1e6;
        }
    }
}

// ---------------------------------------------------------------------------
// Minimal zero-SDK-header Vulkan C ABI declarations for VK_EXT_calibrated_timestamps
// ---------------------------------------------------------------------------
using VkFlags = uint32_t;
using VkBool32 = uint32_t;
using VkResult = int32_t;
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
using VkInstance = VkInstance_T*;
using VkPhysicalDevice = VkPhysicalDevice_T*;
using VkDevice = VkDevice_T*;

inline constexpr VkResult kVkSuccess = 0;
inline constexpr int32_t kVkStypeApplicationInfo = 0;
inline constexpr int32_t kVkStypeInstanceCreateInfo = 1;
inline constexpr int32_t kVkStypeDeviceQueueCreateInfo = 2;
inline constexpr int32_t kVkStypeDeviceCreateInfo = 3;
inline constexpr int32_t kVkStypeCalibratedTimestampInfoExt = 1000184000;

inline constexpr int32_t kVkTimeDomainDeviceExt = 0;
inline constexpr int32_t kVkTimeDomainQueryPerformanceCounterExt = 3;

struct VkApplicationInfoMin {
    int32_t sType;
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfoMin {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    const VkApplicationInfoMin* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct VkExtensionPropertiesMin {
    char extensionName[256];
    uint32_t specVersion;
};

struct VkDeviceQueueCreateInfoMin {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float* pQueuePriorities;
};

struct VkDeviceCreateInfoMin {
    int32_t sType;
    const void* pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfoMin* pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;
};

struct alignas(8) VkPhysicalDevicePropertiesMin {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint32_t pad_to_8;
    uint8_t limits_and_sparse[1024];
};
static_assert(offsetof(VkPhysicalDevicePropertiesMin, limits_and_sparse) == 296, "VkPhysicalDeviceProperties layout");

// Khronos Vulkan 1.0 VkPhysicalDeviceLimits byte offsets (x86-64 ABI):
// timestampComputeAndGraphics = offset 420 (VkBool32), timestampPeriod = offset 424 (float, ns/tick).
inline constexpr size_t kVkLimitsOffsetTimestampComputeAndGraphics = 420u;
inline constexpr size_t kVkLimitsOffsetTimestampPeriod = 424u;

struct VkCalibratedTimestampInfoExtMin {
    int32_t sType;
    const void* pNext;
    int32_t timeDomain;
};

using PFN_vkVoidFunction = void(OS_VKAPI_PTR*)();
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(OS_VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(OS_VKAPI_PTR*)(VkDevice, const char*);
using PFN_vkCreateInstance = VkResult(OS_VKAPI_PTR*)(const VkInstanceCreateInfoMin*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void(OS_VKAPI_PTR*)(VkInstance, const void*);
using PFN_vkEnumeratePhysicalDevices = VkResult(OS_VKAPI_PTR*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void(OS_VKAPI_PTR*)(VkPhysicalDevice, VkPhysicalDevicePropertiesMin*);
using PFN_vkEnumerateDeviceExtensionProperties = VkResult(OS_VKAPI_PTR*)(
    VkPhysicalDevice, const char*, uint32_t*, VkExtensionPropertiesMin*);
using PFN_vkCreateDevice = VkResult(OS_VKAPI_PTR*)(
    VkPhysicalDevice, const VkDeviceCreateInfoMin*, const void*, VkDevice*);
using PFN_vkDestroyDevice = void(OS_VKAPI_PTR*)(VkDevice, const void*);
using PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT = VkResult(OS_VKAPI_PTR*)(
    VkPhysicalDevice, uint32_t*, int32_t*);
using PFN_vkGetCalibratedTimestampsEXT = VkResult(OS_VKAPI_PTR*)(
    VkDevice, uint32_t, const VkCalibratedTimestampInfoExtMin*, uint64_t*, uint64_t*);

}  // namespace

// ---------------------------------------------------------------------------
// QueryAccumulatorX
// ---------------------------------------------------------------------------
struct QueryAccumulatorX::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    QueryAccumulatorConfig config{};
    uint64_t begin_count = 0ull;
    uint64_t end_count = 0ull;
    uint64_t resolve_count = 0ull;
    uint64_t gpu_accumulate_dispatches = 0ull;
    uint64_t active_frame_index = 0ull;

#if defined(_WIN32)
    LocalComPtr<ID3D12Device> host_device;
    LocalComPtr<ID3D12QueryHeap> occlusion_heap;
    LocalComPtr<ID3D12QueryHeap> timestamp_heap;
    LocalComPtr<ID3D12QueryHeap> pipeline_stats_heap;

    // GPU compute accumulation pipeline & scratch buffer.
    bool accum_compute_ready = false;
    LocalComPtr<ID3D12RootSignature> accum_root_sig;
    LocalComPtr<ID3D12PipelineState> accum_pso;
    LocalComPtr<ID3D12Resource> scratch_resolve_buf;
    uint64_t scratch_resolve_capacity = 0ull;
    std::vector<LocalComPtr<ID3D12Resource>> retired_scratch_bufs;

    // Synchronous readback objects.
    bool sync_ready = false;
    LocalComPtr<ID3D12CommandAllocator> rb_alloc;
    LocalComPtr<ID3D12GraphicsCommandList> rb_cmd;
    LocalComPtr<ID3D12Fence> rb_fence;
    HANDLE rb_event = nullptr;
    uint64_t rb_fence_val = 0ull;

    ID3D12QueryHeap* HeapForKindLocked(QueryKind kind) const noexcept {
        switch (kind) {
            case QueryKind::Occlusion:
            case QueryKind::BinaryOcclusion:
                return occlusion_heap.Get();
            case QueryKind::Timestamp:
                return timestamp_heap.Get();
            case QueryKind::PipelineStatistics:
                return pipeline_stats_heap.Get();
            default:
                return nullptr;
        }
    }

    uint32_t CapacityForKindLocked(QueryKind kind) const noexcept {
        switch (kind) {
            case QueryKind::Occlusion:
            case QueryKind::BinaryOcclusion:
                return config.occlusion_capacity;
            case QueryKind::Timestamp:
                return config.timestamp_capacity;
            case QueryKind::PipelineStatistics:
                return config.pipeline_stats_capacity;
            default:
                return 0u;
        }
    }

    Status EnsureAccumComputeLocked() {
        if (accum_compute_ready) return Status::Ok();
        if (!host_device) {
            return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
        }

        auto serialize_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
            ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        if (!serialize_fn) {
            return Status::Error(StatusCode::DeviceError, "Failed to resolve D3D12SerializeRootSignature");
        }

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0u;
        params[0].Constants.RegisterSpace = 0u;
        params[0].Constants.Num32BitValues = 4u;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0u;
        params[1].Descriptor.RegisterSpace = 0u;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0u;
        params[2].Descriptor.RegisterSpace = 0u;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3u;
        rsd.pParameters = params;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        LocalComPtr<ID3DBlob> sig_blob;
        LocalComPtr<ID3DBlob> err_blob;
        HRESULT hr = serialize_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put());
        if (FAILED(hr) || !sig_blob) {
            return Status::Error(StatusCode::DeviceError, "Serialize query accumulate root signature failed");
        }
        hr = host_device->CreateRootSignature(
            0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
            OS_QA_UUIDOF(ID3D12RootSignature), accum_root_sig.PutVoid());
        if (FAILED(hr) || !accum_root_sig) {
            return Status::Error(StatusCode::DeviceError, "CreateRootSignature(query accumulate) failed");
        }

        LocalComPtr<ID3DBlob> cs_blob;
        LocalComPtr<ID3DBlob> cs_err;
        hr = D3DCompile(
            kAccumulateU64Hlsl, std::strlen(kAccumulateU64Hlsl), "CSAccumulateU64",
            nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u,
            cs_blob.Put(), cs_err.Put());
        if (FAILED(hr) || !cs_blob) {
            std::string msg = "D3DCompile(CSAccumulateU64) failed";
            if (cs_err && cs_err->GetBufferPointer() && cs_err->GetBufferSize() > 0u) {
                msg += ": ";
                msg.append(static_cast<const char*>(cs_err->GetBufferPointer()), cs_err->GetBufferSize());
            }
            return Status::Error(StatusCode::ShaderError, msg);
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
        psd.pRootSignature = accum_root_sig.Get();
        psd.CS.pShaderBytecode = cs_blob->GetBufferPointer();
        psd.CS.BytecodeLength = cs_blob->GetBufferSize();
        hr = host_device->CreateComputePipelineState(
            &psd, OS_QA_UUIDOF(ID3D12PipelineState), accum_pso.PutVoid());
        if (FAILED(hr) || !accum_pso) {
            return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(query accumulate) failed");
        }

        accum_compute_ready = true;
        return Status::Ok();
    }

    Status EnsureScratchResolveBufferLocked(uint64_t required_bytes) {
        if (scratch_resolve_buf && scratch_resolve_capacity >= required_bytes) {
            return Status::Ok();
        }
        const uint64_t new_cap = std::max<uint64_t>(4096ull, required_bytes);
        LocalComPtr<ID3D12Resource> next_buf;
        Status st = CreateBuffer(
            host_device.Get(), D3D12_HEAP_TYPE_DEFAULT, new_cap,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, next_buf);
        if (!st.ok()) return st;
        if (scratch_resolve_buf) {
            retired_scratch_bufs.push_back(std::move(scratch_resolve_buf));
        }
        scratch_resolve_buf = std::move(next_buf);
        scratch_resolve_capacity = new_cap;
        return Status::Ok();
    }

    Status EnsureSyncObjectsLocked() {
        if (sync_ready) return Status::Ok();
        if (!host_device) {
            return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
        }
        HRESULT hr = host_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, OS_QA_UUIDOF(ID3D12CommandAllocator), rb_alloc.PutVoid());
        if (FAILED(hr) || !rb_alloc) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandAllocator(query readback) failed");
        }
        hr = host_device->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, rb_alloc.Get(), nullptr,
            OS_QA_UUIDOF(ID3D12GraphicsCommandList), rb_cmd.PutVoid());
        if (FAILED(hr) || !rb_cmd) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandList(query readback) failed");
        }
        rb_cmd->Close();

        hr = host_device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_QA_UUIDOF(ID3D12Fence), rb_fence.PutVoid());
        if (FAILED(hr) || !rb_fence) {
            return Status::Error(StatusCode::DeviceError, "CreateFence(query readback) failed");
        }
        rb_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!rb_event) {
            return Status::Error(StatusCode::DeviceError, "CreateEventA(query readback) failed");
        }
        sync_ready = true;
        return Status::Ok();
    }

    Status ExecuteAndWaitLocked(ID3D12CommandQueue* queue) {
        if (!queue) return Status::Error(StatusCode::InvalidArgument, "Null command queue");
        if (FAILED(rb_cmd->Close())) {
            return Status::Error(StatusCode::DeviceError, "rb_cmd->Close failed");
        }
        ID3D12CommandList* lists[1] = {rb_cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);
        const uint64_t val = ++rb_fence_val;
        if (FAILED(queue->Signal(rb_fence.Get(), val))) {
            return Status::Error(StatusCode::DeviceError, "queue->Signal failed");
        }
        if (rb_fence->GetCompletedValue() < val) {
            if (FAILED(rb_fence->SetEventOnCompletion(val, rb_event))) {
                return Status::Error(StatusCode::DeviceError, "SetEventOnCompletion failed");
            }
            if (WaitForSingleObject(rb_event, 15000u) != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "Query readback GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    void ReleaseAllLocked() {
        retired_scratch_bufs.clear();
        scratch_resolve_buf.Reset();
        scratch_resolve_capacity = 0ull;
        accum_pso.Reset();
        accum_root_sig.Reset();
        accum_compute_ready = false;

        rb_cmd.Reset();
        rb_alloc.Reset();
        rb_fence.Reset();
        if (rb_event) {
            CloseHandle(rb_event);
            rb_event = nullptr;
        }
        sync_ready = false;

        pipeline_stats_heap.Reset();
        timestamp_heap.Reset();
        occlusion_heap.Reset();
        host_device.Reset();
        initialized = false;
    }
#endif
};

QueryAccumulatorX::QueryAccumulatorX() : impl_(std::make_unique<Impl>()) {}
QueryAccumulatorX::~QueryAccumulatorX() { Shutdown(); }

void QueryAccumulatorX::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(_WIN32)
    impl_->ReleaseAllLocked();
#else
    impl_->initialized = false;
#endif
}

bool QueryAccumulatorX::IsInitialized() const noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized;
}

QueryAccumulatorStats QueryAccumulatorX::GetStats() const {
    QueryAccumulatorStats s{};
    if (!impl_) return s;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    s.occlusion_capacity = impl_->config.occlusion_capacity;
    s.timestamp_capacity = impl_->config.timestamp_capacity;
    s.pipeline_stats_capacity = impl_->config.pipeline_stats_capacity;
    s.begin_count = impl_->begin_count;
    s.end_count = impl_->end_count;
    s.resolve_count = impl_->resolve_count;
    s.gpu_accumulate_dispatches = impl_->gpu_accumulate_dispatches;
    s.active_frame_index = impl_->active_frame_index;
    return s;
}

void QueryAccumulatorX::BeginFrame(uint64_t frame_index) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->active_frame_index = frame_index;
}

uint64_t QueryAccumulatorX::CurrentFrame() const noexcept {
    if (!impl_) return 0ull;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->active_frame_index;
}

#if defined(_WIN32)

D3D12_QUERY_HEAP_TYPE QueryAccumulatorX::ToD3D12HeapType(QueryKind kind) noexcept {
    switch (kind) {
        case QueryKind::Occlusion:
        case QueryKind::BinaryOcclusion:
            return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        case QueryKind::Timestamp:
            return D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        case QueryKind::PipelineStatistics:
            return D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        default:
            return D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    }
}

D3D12_QUERY_TYPE QueryAccumulatorX::ToD3D12QueryType(QueryKind kind) noexcept {
    switch (kind) {
        case QueryKind::Occlusion:
            return D3D12_QUERY_TYPE_OCCLUSION;
        case QueryKind::BinaryOcclusion:
            return D3D12_QUERY_TYPE_BINARY_OCCLUSION;
        case QueryKind::Timestamp:
            return D3D12_QUERY_TYPE_TIMESTAMP;
        case QueryKind::PipelineStatistics:
            return D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
        default:
            return D3D12_QUERY_TYPE_OCCLUSION;
    }
}

uint32_t QueryAccumulatorX::QueryElementByteSize(QueryKind kind) noexcept {
    if (kind == QueryKind::PipelineStatistics) {
        return static_cast<uint32_t>(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    }
    return static_cast<uint32_t>(sizeof(uint64_t));
}

Status QueryAccumulatorX::Initialize(
    ID3D12Device* host_device,
    const QueryAccumulatorConfig& config) {
    if (!host_device) {
        return Status::Error(StatusCode::InvalidArgument, "QueryAccumulatorX::Initialize: null device");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ReleaseAllLocked();

    host_device->AddRef();
    impl_->host_device.Attach(host_device);
    impl_->config = config;

    auto create_heap = [&](D3D12_QUERY_HEAP_TYPE type, uint32_t count,
                           LocalComPtr<ID3D12QueryHeap>& out_heap) -> Status {
        if (count == 0u) return Status::Ok();
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = type;
        qd.Count = count;
        qd.NodeMask = 0u;
        const HRESULT hr = host_device->CreateQueryHeap(
            &qd, OS_QA_UUIDOF(ID3D12QueryHeap), out_heap.PutVoid());
        if (FAILED(hr) || !out_heap) {
            std::ostringstream oss;
            oss << "CreateQueryHeap(type=" << static_cast<uint32_t>(type)
                << ", count=" << count << ") failed hr=0x"
                << std::hex << static_cast<uint32_t>(hr);
            return Status::Error(StatusCode::DeviceError, oss.str());
        }
        return Status::Ok();
    };

    Status st = create_heap(D3D12_QUERY_HEAP_TYPE_OCCLUSION, config.occlusion_capacity, impl_->occlusion_heap);
    if (!st.ok()) {
        impl_->ReleaseAllLocked();
        return st;
    }
    st = create_heap(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, config.timestamp_capacity, impl_->timestamp_heap);
    if (!st.ok()) {
        impl_->ReleaseAllLocked();
        return st;
    }
    st = create_heap(
        D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, config.pipeline_stats_capacity, impl_->pipeline_stats_heap);
    if (!st.ok()) {
        impl_->ReleaseAllLocked();
        return st;
    }

    impl_->begin_count = 0ull;
    impl_->end_count = 0ull;
    impl_->resolve_count = 0ull;
    impl_->gpu_accumulate_dispatches = 0ull;
    impl_->initialized = true;
    return Status::Ok();
}

ID3D12QueryHeap* QueryAccumulatorX::GetQueryHeap(QueryKind kind) const noexcept {
    if (!impl_) return nullptr;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->HeapForKindLocked(kind);
}

Status QueryAccumulatorX::BeginQuery(
    ID3D12GraphicsCommandList* cmd_list,
    QueryKind kind,
    uint32_t index) {
    if (!cmd_list) return Status::Error(StatusCode::InvalidArgument, "BeginQuery: null command list");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
    }
    if (kind == QueryKind::Timestamp) {
        // Per D3D12 spec, TIMESTAMP queries only call EndQuery; BeginQuery is a safe no-op.
        return Status::Ok();
    }
    ID3D12QueryHeap* heap = impl_->HeapForKindLocked(kind);
    const uint32_t cap = impl_->CapacityForKindLocked(kind);
    if (!heap || index >= cap) {
        return Status::Error(StatusCode::InvalidArgument, "BeginQuery: query heap unavailable or index out of range");
    }
    cmd_list->BeginQuery(heap, ToD3D12QueryType(kind), index);
    ++impl_->begin_count;
    return Status::Ok();
}

Status QueryAccumulatorX::EndQuery(
    ID3D12GraphicsCommandList* cmd_list,
    QueryKind kind,
    uint32_t index) {
    if (!cmd_list) return Status::Error(StatusCode::InvalidArgument, "EndQuery: null command list");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
    }
    ID3D12QueryHeap* heap = impl_->HeapForKindLocked(kind);
    const uint32_t cap = impl_->CapacityForKindLocked(kind);
    if (!heap || index >= cap) {
        return Status::Error(StatusCode::InvalidArgument, "EndQuery: query heap unavailable or index out of range");
    }
    cmd_list->EndQuery(heap, ToD3D12QueryType(kind), index);
    ++impl_->end_count;
    return Status::Ok();
}

Status QueryAccumulatorX::ResolveQueryData(
    ID3D12GraphicsCommandList* cmd_list,
    QueryKind kind,
    uint32_t start_index,
    uint32_t num_queries,
    ID3D12Resource* dst_buffer,
    uint64_t aligned_dst_offset) {
    if (!cmd_list || !dst_buffer || num_queries == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveQueryData: invalid arguments");
    }
    if ((aligned_dst_offset % 8ull) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveQueryData: destination offset must be 8-byte aligned");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
    }
    ID3D12QueryHeap* heap = impl_->HeapForKindLocked(kind);
    const uint32_t cap = impl_->CapacityForKindLocked(kind);
    if (!heap || start_index > cap || num_queries > (cap - start_index)) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveQueryData: query range out of bounds");
    }
    cmd_list->ResolveQueryData(
        heap, ToD3D12QueryType(kind), start_index, num_queries, dst_buffer, aligned_dst_offset);
    ++impl_->resolve_count;
    return Status::Ok();
}

Status QueryAccumulatorX::ResolveAndAccumulateU64Gpu(
    ID3D12GraphicsCommandList* cmd_list,
    QueryKind kind,
    const std::vector<QueryRangeSpec>& ranges,
    ID3D12Resource* dst_buffer,
    uint64_t dst_offset_bytes,
    QueryAccumulationOp op,
    ID3D12Resource* predicate_buffer,
    uint64_t predicate_offset_bytes,
    D3D12_PREDICATION_OP predicate_op,
    D3D12_RESOURCE_STATES dst_state_before,
    D3D12_RESOURCE_STATES dst_state_after) {
    if (!cmd_list || !dst_buffer || ranges.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveAndAccumulateU64Gpu: invalid arguments");
    }
    if (kind == QueryKind::PipelineStatistics) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "ResolveAndAccumulateU64Gpu only supports 64-bit scalar query kinds");
    }
    if ((dst_offset_bytes % 8ull) != 0ull || dst_offset_bytes > 0xFFFFFFFFull) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "ResolveAndAccumulateU64Gpu: dst_offset_bytes must be 8-byte aligned and < 4 GiB");
    }
    if (predicate_buffer && (predicate_offset_bytes % 8ull) != 0ull) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "ResolveAndAccumulateU64Gpu: predicate_offset_bytes must be 8-byte aligned");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
    }

    ID3D12QueryHeap* heap = impl_->HeapForKindLocked(kind);
    const uint32_t cap = impl_->CapacityForKindLocked(kind);
    if (!heap) {
        return Status::Error(StatusCode::DeviceError, "Query heap not allocated for requested kind");
    }

    uint32_t total_queries = 0u;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        if (r.start_index > cap || r.count > (cap - r.start_index)) {
            return Status::Error(StatusCode::InvalidArgument, "ResolveAndAccumulateU64Gpu: range out of bounds");
        }
        total_queries += r.count;
    }
    if (total_queries == 0u) return Status::Ok();

    Status st = impl_->EnsureAccumComputeLocked();
    if (!st.ok()) return st;
    st = impl_->EnsureScratchResolveBufferLocked(static_cast<uint64_t>(total_queries) * sizeof(uint64_t));
    if (!st.ok()) return st;

    const D3D12_QUERY_TYPE d3d_type = ToD3D12QueryType(kind);
    uint64_t cur_off = 0ull;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        cmd_list->ResolveQueryData(
            heap, d3d_type, r.start_index, r.count, impl_->scratch_resolve_buf.Get(), cur_off);
        cur_off += static_cast<uint64_t>(r.count) * sizeof(uint64_t);
        ++impl_->resolve_count;
    }

    EmitTransition(
        cmd_list, impl_->scratch_resolve_buf.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    EmitTransition(cmd_list, dst_buffer, dst_state_before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (predicate_buffer) {
        cmd_list->SetPredication(predicate_buffer, predicate_offset_bytes, predicate_op);
    }

    const uint32_t constants[4] = {
        total_queries,
        static_cast<uint32_t>(op),
        static_cast<uint32_t>(dst_offset_bytes),
        0u,
    };
    cmd_list->SetComputeRootSignature(impl_->accum_root_sig.Get());
    cmd_list->SetPipelineState(impl_->accum_pso.Get());
    cmd_list->SetComputeRoot32BitConstants(0u, 4u, constants, 0u);
    cmd_list->SetComputeRootShaderResourceView(1u, impl_->scratch_resolve_buf->GetGPUVirtualAddress());
    cmd_list->SetComputeRootUnorderedAccessView(2u, dst_buffer->GetGPUVirtualAddress());
    cmd_list->Dispatch(1u, 1u, 1u);

    if (predicate_buffer) {
        cmd_list->SetPredication(nullptr, 0ull, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }

    EmitUavBarrier(cmd_list, dst_buffer);
    EmitTransition(cmd_list, dst_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dst_state_after);
    EmitTransition(
        cmd_list, impl_->scratch_resolve_buf.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    ++impl_->gpu_accumulate_dispatches;
    return Status::Ok();
}

Status QueryAccumulatorX::ReadbackU64Range(
    ID3D12CommandQueue* queue,
    QueryKind kind,
    uint32_t start_index,
    uint32_t num_queries,
    std::vector<uint64_t>& out_values) {
    out_values.clear();
    if (num_queries == 0u) return Status::Ok();
    uint64_t dummy_sum = 0ull;
    return ReadbackAndAccumulateU64(queue, kind, {{start_index, num_queries}}, dummy_sum, &out_values);
}

Status QueryAccumulatorX::ReadbackAndAccumulateU64(
    ID3D12CommandQueue* queue,
    QueryKind kind,
    const std::vector<QueryRangeSpec>& ranges,
    uint64_t& out_accumulated_value,
    std::vector<uint64_t>* out_individual_values) {
    out_accumulated_value = 0ull;
    if (out_individual_values) out_individual_values->clear();
    if (!queue || ranges.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "ReadbackAndAccumulateU64: invalid arguments");
    }
    if (kind == QueryKind::PipelineStatistics) {
        return Status::Error(StatusCode::InvalidArgument, "Use ReadbackPipelineStatistics for PipelineStatistics");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) {
        return Status::Error(StatusCode::DeviceError, "QueryAccumulatorX not initialized");
    }
    ID3D12QueryHeap* heap = impl_->HeapForKindLocked(kind);
    const uint32_t cap = impl_->CapacityForKindLocked(kind);
    if (!heap) {
        return Status::Error(StatusCode::DeviceError, "Query heap not allocated for requested kind");
    }

    uint32_t total_queries = 0u;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        if (r.start_index > cap || r.count > (cap - r.start_index)) {
            return Status::Error(StatusCode::InvalidArgument, "ReadbackAndAccumulateU64: range out of bounds");
        }
        total_queries += r.count;
    }
    if (total_queries == 0u) return Status::Ok();

    Status st = impl_->EnsureSyncObjectsLocked();
    if (!st.ok()) return st;

    const uint64_t byte_size = static_cast<uint64_t>(total_queries) * sizeof(uint64_t);
    LocalComPtr<ID3D12Resource> rb_buf;
    st = CreateBuffer(
        impl_->host_device.Get(), D3D12_HEAP_TYPE_READBACK, byte_size,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf);
    if (!st.ok()) return st;

    if (FAILED(impl_->rb_alloc->Reset()) ||
        FAILED(impl_->rb_cmd->Reset(impl_->rb_alloc.Get(), nullptr))) {
        return Status::Error(StatusCode::DeviceError, "Failed to reset query readback command list");
    }

    const D3D12_QUERY_TYPE d3d_type = ToD3D12QueryType(kind);
    uint64_t cur_off = 0ull;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        impl_->rb_cmd->ResolveQueryData(heap, d3d_type, r.start_index, r.count, rb_buf.Get(), cur_off);
        cur_off += static_cast<uint64_t>(r.count) * sizeof(uint64_t);
        ++impl_->resolve_count;
    }

    st = impl_->ExecuteAndWaitLocked(queue);
    if (!st.ok()) return st;

    void* mapped = nullptr;
    D3D12_RANGE read_range{0u, static_cast<SIZE_T>(byte_size)};
    if (FAILED(rb_buf->Map(0u, &read_range, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(query U64 readback) failed");
    }

    const auto* vals = static_cast<const uint64_t*>(mapped);
    uint64_t acc = 0ull;
    if (out_individual_values) {
        out_individual_values->assign(vals, vals + total_queries);
    }
    for (uint32_t i = 0u; i < total_queries; ++i) {
        if (kind == QueryKind::BinaryOcclusion) {
            acc = (acc != 0ull || vals[i] != 0ull) ? 1ull : 0ull;
        } else {
            acc += vals[i];
        }
    }
    D3D12_RANGE write_none{0u, 0u};
    rb_buf->Unmap(0u, &write_none);

    out_accumulated_value = acc;
    return Status::Ok();
}

Status QueryAccumulatorX::ReadbackPipelineStatistics(
    ID3D12CommandQueue* queue,
    const std::vector<QueryRangeSpec>& ranges,
    QueryPipelineStatsResult& out_accumulated,
    std::vector<QueryPipelineStatsResult>* out_individual) {
    out_accumulated = QueryPipelineStatsResult{};
    if (out_individual) out_individual->clear();
    if (!queue || ranges.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "ReadbackPipelineStatistics: invalid arguments");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->pipeline_stats_heap) {
        return Status::Error(StatusCode::DeviceError, "PipelineStatistics query heap unavailable");
    }

    const uint32_t cap = impl_->config.pipeline_stats_capacity;
    uint32_t total_queries = 0u;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        if (r.start_index > cap || r.count > (cap - r.start_index)) {
            return Status::Error(StatusCode::InvalidArgument, "ReadbackPipelineStatistics: range out of bounds");
        }
        total_queries += r.count;
    }
    if (total_queries == 0u) return Status::Ok();

    Status st = impl_->EnsureSyncObjectsLocked();
    if (!st.ok()) return st;

    const uint64_t elem_size = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    const uint64_t byte_size = static_cast<uint64_t>(total_queries) * elem_size;
    LocalComPtr<ID3D12Resource> rb_buf;
    st = CreateBuffer(
        impl_->host_device.Get(), D3D12_HEAP_TYPE_READBACK, byte_size,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf);
    if (!st.ok()) return st;

    if (FAILED(impl_->rb_alloc->Reset()) ||
        FAILED(impl_->rb_cmd->Reset(impl_->rb_alloc.Get(), nullptr))) {
        return Status::Error(StatusCode::DeviceError, "Failed to reset pipeline stats readback command list");
    }

    uint64_t cur_off = 0ull;
    for (const auto& r : ranges) {
        if (r.count == 0u) continue;
        impl_->rb_cmd->ResolveQueryData(
            impl_->pipeline_stats_heap.Get(),
            D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
            r.start_index,
            r.count,
            rb_buf.Get(),
            cur_off);
        cur_off += static_cast<uint64_t>(r.count) * elem_size;
        ++impl_->resolve_count;
    }

    st = impl_->ExecuteAndWaitLocked(queue);
    if (!st.ok()) return st;

    void* mapped = nullptr;
    D3D12_RANGE read_range{0u, static_cast<SIZE_T>(byte_size)};
    if (FAILED(rb_buf->Map(0u, &read_range, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(pipeline stats readback) failed");
    }

    const auto* stats = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mapped);
    for (uint32_t i = 0u; i < total_queries; ++i) {
        QueryPipelineStatsResult r{};
        r.ia_vertices = stats[i].IAVertices;
        r.ia_primitives = stats[i].IAPrimitives;
        r.vs_invocations = stats[i].VSInvocations;
        r.gs_invocations = stats[i].GSInvocations;
        r.gs_primitives = stats[i].GSPrimitives;
        r.c_invocations = stats[i].CInvocations;
        r.c_primitives = stats[i].CPrimitives;
        r.ps_invocations = stats[i].PSInvocations;
        r.hs_invocations = stats[i].HSInvocations;
        r.ds_invocations = stats[i].DSInvocations;
        r.cs_invocations = stats[i].CSInvocations;
        out_accumulated.AccumulateAdd(r);
        if (out_individual) out_individual->push_back(r);
    }

    D3D12_RANGE write_none{0u, 0u};
    rb_buf->Unmap(0u, &write_none);
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// PredicatedByteBufferCopy
// ---------------------------------------------------------------------------
Status PredicatedByteBufferCopy(
    ID3D12GraphicsCommandList* cmd_list,
    const PredicatedByteCopyDesc& desc,
    PredicatedByteCopyMetrics* out_metrics) {
    if (!cmd_list || !desc.dst_buffer || !desc.src_buffer) {
        return Status::Error(StatusCode::InvalidArgument, "PredicatedByteBufferCopy: null resource or command list");
    }
    if (desc.num_bytes == 0ull) {
        if (out_metrics) {
            *out_metrics = PredicatedByteCopyMetrics{};
        }
        return Status::Ok();
    }

    const D3D12_RESOURCE_DESC dst_desc = desc.dst_buffer->GetDesc();
    const D3D12_RESOURCE_DESC src_desc = desc.src_buffer->GetDesc();
    if (dst_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        src_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return Status::Error(StatusCode::InvalidArgument, "PredicatedByteBufferCopy: resources must be buffers");
    }
    if (desc.dst_offset_bytes > dst_desc.Width ||
        desc.num_bytes > (dst_desc.Width - desc.dst_offset_bytes)) {
        return Status::Error(StatusCode::InvalidArgument, "PredicatedByteBufferCopy: dst range out of bounds");
    }
    if (desc.src_offset_bytes > src_desc.Width ||
        desc.num_bytes > (src_desc.Width - desc.src_offset_bytes)) {
        return Status::Error(StatusCode::InvalidArgument, "PredicatedByteBufferCopy: src range out of bounds");
    }

    if (desc.predicate_buffer) {
        if ((desc.predicate_offset_bytes % 8ull) != 0ull) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "PredicatedByteBufferCopy: predicate_offset_bytes must be 8-byte aligned");
        }
        const D3D12_RESOURCE_DESC pred_desc = desc.predicate_buffer->GetDesc();
        if (pred_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            desc.predicate_offset_bytes > pred_desc.Width ||
            8ull > (pred_desc.Width - desc.predicate_offset_bytes)) {
            return Status::Error(
                StatusCode::InvalidArgument,
                "PredicatedByteBufferCopy: predicate range out of bounds");
        }
    }

    EmitTransition(cmd_list, desc.dst_buffer, desc.dst_state_before, D3D12_RESOURCE_STATE_COPY_DEST);
    EmitTransition(cmd_list, desc.src_buffer, desc.src_state_before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (desc.predicate_buffer) {
        EmitTransition(
            cmd_list, desc.predicate_buffer, desc.predicate_state_before, D3D12_RESOURCE_STATE_PREDICATION);
        cmd_list->SetPredication(desc.predicate_buffer, desc.predicate_offset_bytes, desc.predicate_op);
    }

    // D3D12 CopyBufferRegion natively handles exact 1-byte granularity for DstOffset, SrcOffset,
    // and NumBytes, and is directly governed by active SetPredication state.
    cmd_list->CopyBufferRegion(
        desc.dst_buffer,
        desc.dst_offset_bytes,
        desc.src_buffer,
        desc.src_offset_bytes,
        desc.num_bytes);

    if (desc.predicate_buffer) {
        cmd_list->SetPredication(nullptr, 0ull, D3D12_PREDICATION_OP_EQUAL_ZERO);
        EmitTransition(
            cmd_list, desc.predicate_buffer, D3D12_RESOURCE_STATE_PREDICATION, desc.predicate_state_after);
    }
    EmitTransition(cmd_list, desc.src_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, desc.src_state_after);
    EmitTransition(cmd_list, desc.dst_buffer, D3D12_RESOURCE_STATE_COPY_DEST, desc.dst_state_after);

    if (out_metrics) {
        out_metrics->used_native_predication = (desc.predicate_buffer != nullptr);
        out_metrics->sub_4byte_unaligned =
            ((desc.dst_offset_bytes % 4ull) != 0ull) ||
            ((desc.src_offset_bytes % 4ull) != 0ull) ||
            ((desc.num_bytes % 4ull) != 0ull);
        out_metrics->dst_offset_bytes = desc.dst_offset_bytes;
        out_metrics->src_offset_bytes = desc.src_offset_bytes;
        out_metrics->num_bytes = desc.num_bytes;
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// GPU-to-Win32 QPC Clock Calibration
// ---------------------------------------------------------------------------
uint64_t GpuClockCalibrationResult::GpuTicksToEstimatedQpc(uint64_t gpu_ticks) const noexcept {
    if (gpu_timestamp_frequency_hz == 0ull || cpu_qpc_frequency_hz == 0ull) return 0ull;
    const double delta_gpu_ticks =
        static_cast<double>(static_cast<int64_t>(gpu_ticks - anchor.gpu_timestamp_ticks));
    const double delta_seconds = delta_gpu_ticks / static_cast<double>(gpu_timestamp_frequency_hz);
    const double est_qpc =
        static_cast<double>(anchor.cpu_qpc_ticks) + delta_seconds * static_cast<double>(cpu_qpc_frequency_hz);
    return (est_qpc <= 0.0) ? 0ull : static_cast<uint64_t>(est_qpc + 0.5);
}

double GpuClockCalibrationResult::GpuDeltaTicksToMicroseconds(uint64_t delta_gpu_ticks) const noexcept {
    if (gpu_timestamp_frequency_hz == 0ull) return 0.0;
    return (static_cast<double>(delta_gpu_ticks) * 1e6) / static_cast<double>(gpu_timestamp_frequency_hz);
}

Status CalibrateGpuClockWithQpc(
    ID3D12CommandQueue* queue,
    GpuClockCalibrationResult& out_result,
    uint32_t num_samples,
    uint32_t inter_sample_delay_ms) {
    out_result = GpuClockCalibrationResult{};
    out_result.backend_name = "D3D12";
    if (!queue) {
        return Status::Error(StatusCode::InvalidArgument, "CalibrateGpuClockWithQpc: null command queue");
    }

    LARGE_INTEGER qpc_freq{};
    if (!QueryPerformanceFrequency(&qpc_freq) || qpc_freq.QuadPart <= 0) {
        return Status::Error(StatusCode::DeviceError, "QueryPerformanceFrequency failed");
    }

    UINT64 gpu_freq = 0ull;
    const HRESULT freq_hr = queue->GetTimestampFrequency(&gpu_freq);
    if (FAILED(freq_hr) || gpu_freq == 0ull) {
        std::ostringstream oss;
        oss << "ID3D12CommandQueue::GetTimestampFrequency failed hr=0x"
            << std::hex << static_cast<uint32_t>(freq_hr);
        return Status::Error(StatusCode::DeviceError, oss.str());
    }

    // Best-effort adapter identification for diagnostics.
    LocalComPtr<ID3D12Device> dev;
    if (SUCCEEDED(queue->GetDevice(OS_QA_UUIDOF(ID3D12Device), dev.PutVoid())) && dev) {
        D3D12AdapterInfo info{};
        if (QueryD3D12DeviceAdapterInfo(dev.Get(), nullptr, info).ok()) {
            out_result.device_name = info.description;
        }
    }

    out_result.gpu_timestamp_frequency_valid = true;
    out_result.gpu_timestamp_frequency_hz = gpu_freq;
    out_result.cpu_qpc_frequency_hz = static_cast<uint64_t>(qpc_freq.QuadPart);
    out_result.gpu_tick_period_ns = 1e9 / static_cast<double>(gpu_freq);
    out_result.cpu_qpc_period_ns = 1e9 / static_cast<double>(qpc_freq.QuadPart);

    const uint32_t clamped_samples = std::clamp<uint32_t>(num_samples, 1u, 64u);
    out_result.samples.reserve(clamped_samples);

    for (uint32_t i = 0u; i < clamped_samples; ++i) {
        LARGE_INTEGER qpc_before{};
        LARGE_INTEGER qpc_after{};
        UINT64 gpu_ts = 0ull;
        UINT64 cpu_ts = 0ull;

        QueryPerformanceCounter(&qpc_before);
        const HRESULT cal_hr = queue->GetClockCalibration(&gpu_ts, &cpu_ts);
        QueryPerformanceCounter(&qpc_after);

        if (FAILED(cal_hr)) {
            std::ostringstream oss;
            oss << "ID3D12CommandQueue::GetClockCalibration failed hr=0x"
                << std::hex << static_cast<uint32_t>(cal_hr);
            return Status::Error(StatusCode::DeviceError, oss.str());
        }

        const uint64_t before_u64 = static_cast<uint64_t>(qpc_before.QuadPart);
        const uint64_t after_u64 = static_cast<uint64_t>(qpc_after.QuadPart);
        const uint64_t bracket_ticks = (after_u64 >= before_u64) ? (after_u64 - before_u64) : 0ull;

        GpuClockCalibrationSample sample{};
        sample.gpu_timestamp_ticks = gpu_ts;
        sample.cpu_qpc_ticks = cpu_ts;
        sample.qpc_before_ticks = before_u64;
        sample.qpc_after_ticks = after_u64;
        sample.max_deviation_ns = static_cast<uint64_t>(
            static_cast<double>(bracket_ticks) * out_result.cpu_qpc_period_ns + 0.5);
        sample.gpu_time_seconds = static_cast<double>(gpu_ts) / static_cast<double>(gpu_freq);
        sample.cpu_time_seconds = static_cast<double>(cpu_ts) / static_cast<double>(qpc_freq.QuadPart);
        sample.offset_seconds = sample.cpu_time_seconds - sample.gpu_time_seconds;
        out_result.samples.push_back(sample);

        if (i + 1u < clamped_samples && inter_sample_delay_ms > 0u) {
            Sleep(inter_sample_delay_ms);
        }
    }

    FinalizeCalibrationStatistics(out_result);
    return Status::Ok();
}

#endif  // _WIN32

Status CalibrateVulkanGpuClockWithQpc(
    GpuClockCalibrationResult& out_result,
    uint32_t num_samples,
    uint32_t inter_sample_delay_ms) {
    out_result = GpuClockCalibrationResult{};
    out_result.backend_name = "Vulkan";

#if !defined(_WIN32)
    (void)num_samples;
    (void)inter_sample_delay_ms;
    return Status::Error(StatusCode::NotImplemented, "Win32 QPC calibration requires Windows");
#else
    LARGE_INTEGER qpc_freq{};
    if (!QueryPerformanceFrequency(&qpc_freq) || qpc_freq.QuadPart <= 0) {
        return Status::Error(StatusCode::DeviceError, "QueryPerformanceFrequency failed");
    }

    HMODULE vk_dll = LoadLibraryA("vulkan-1.dll");
    if (!vk_dll) {
        return Status::Error(StatusCode::NotImplemented, "vulkan-1.dll not available on host");
    }

    struct VkCleanup {
        HMODULE dll = nullptr;
        VkInstance instance = nullptr;
        VkDevice device = nullptr;
        PFN_vkDestroyDevice destroy_dev = nullptr;
        PFN_vkDestroyInstance destroy_inst = nullptr;
        ~VkCleanup() {
            if (device && destroy_dev) destroy_dev(device, nullptr);
            if (instance && destroy_inst) destroy_inst(instance, nullptr);
            if (dll) FreeLibrary(dll);
        }
    } cleanup;
    cleanup.dll = vk_dll;

    auto get_inst_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(vk_dll, "vkGetInstanceProcAddr"));
    if (!get_inst_proc) {
        return Status::Error(StatusCode::NotImplemented, "vkGetInstanceProcAddr not exported");
    }

    auto create_inst = reinterpret_cast<PFN_vkCreateInstance>(get_inst_proc(nullptr, "vkCreateInstance"));
    if (!create_inst) {
        return Status::Error(StatusCode::NotImplemented, "vkCreateInstance not available");
    }

    VkApplicationInfoMin app_info{};
    app_info.sType = kVkStypeApplicationInfo;
    app_info.pApplicationName = "OpenScarlettClockCalib";
    app_info.applicationVersion = 1u;
    app_info.pEngineName = "OpenScarlett";
    app_info.engineVersion = 1u;
    app_info.apiVersion = (1u << 22u) | (1u << 12u);  // Vulkan 1.1

    VkInstanceCreateInfoMin ici{};
    ici.sType = kVkStypeInstanceCreateInfo;
    ici.pApplicationInfo = &app_info;

    VkResult vr = create_inst(&ici, nullptr, &cleanup.instance);
    if (vr != kVkSuccess || !cleanup.instance) {
        return Status::Error(StatusCode::NotImplemented, "vkCreateInstance failed");
    }

    cleanup.destroy_inst = reinterpret_cast<PFN_vkDestroyInstance>(
        get_inst_proc(cleanup.instance, "vkDestroyInstance"));
    auto enum_phys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        get_inst_proc(cleanup.instance, "vkEnumeratePhysicalDevices"));
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get_inst_proc(cleanup.instance, "vkGetPhysicalDeviceProperties"));
    auto enum_dev_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        get_inst_proc(cleanup.instance, "vkEnumerateDeviceExtensionProperties"));
    auto create_dev = reinterpret_cast<PFN_vkCreateDevice>(
        get_inst_proc(cleanup.instance, "vkCreateDevice"));
    auto get_dev_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_inst_proc(cleanup.instance, "vkGetDeviceProcAddr"));
    cleanup.destroy_dev = reinterpret_cast<PFN_vkDestroyDevice>(
        get_inst_proc(cleanup.instance, "vkDestroyDevice"));

    if (!enum_phys || !get_props || !enum_dev_ext || !create_dev || !get_dev_proc) {
        return Status::Error(StatusCode::NotImplemented, "Required Vulkan core functions missing");
    }

    uint32_t phys_count = 0u;
    vr = enum_phys(cleanup.instance, &phys_count, nullptr);
    if (vr != kVkSuccess || phys_count == 0u) {
        return Status::Error(StatusCode::NotImplemented, "No Vulkan physical devices enumerated");
    }
    std::vector<VkPhysicalDevice> phys_list(phys_count, nullptr);
    enum_phys(cleanup.instance, &phys_count, phys_list.data());

    auto get_domains_ext = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
        get_inst_proc(cleanup.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    auto get_domains_khr = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
        get_inst_proc(cleanup.instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));

    VkPhysicalDevice chosen_phys = nullptr;
    const char* chosen_ext_name = nullptr;
    bool use_khr_variant = false;
    VkPhysicalDevicePropertiesMin chosen_props{};
    float chosen_ts_period_ns = 0.0f;

    for (VkPhysicalDevice phys : phys_list) {
        if (!phys) continue;
        uint32_t ext_count = 0u;
        if (enum_dev_ext(phys, nullptr, &ext_count, nullptr) != kVkSuccess || ext_count == 0u) continue;
        std::vector<VkExtensionPropertiesMin> exts(ext_count);
        if (enum_dev_ext(phys, nullptr, &ext_count, exts.data()) != kVkSuccess) continue;

        bool has_ext = false;
        bool has_khr = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, "VK_EXT_calibrated_timestamps") == 0) has_ext = true;
            if (std::strcmp(e.extensionName, "VK_KHR_calibrated_timestamps") == 0) has_khr = true;
        }
        if (!has_ext && !has_khr) continue;

        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domain_fn =
            (has_ext && get_domains_ext) ? get_domains_ext : get_domains_khr;
        if (!domain_fn) continue;

        uint32_t dom_count = 0u;
        if (domain_fn(phys, &dom_count, nullptr) != kVkSuccess || dom_count == 0u) continue;
        std::vector<int32_t> domains(dom_count, -1);
        if (domain_fn(phys, &dom_count, domains.data()) != kVkSuccess) continue;

        bool has_dev_dom = false;
        bool has_qpc_dom = false;
        for (int32_t d : domains) {
            if (d == kVkTimeDomainDeviceExt) has_dev_dom = true;
            if (d == kVkTimeDomainQueryPerformanceCounterExt) has_qpc_dom = true;
        }
        if (!has_dev_dom || !has_qpc_dom) continue;

        VkPhysicalDevicePropertiesMin props{};
        get_props(phys, &props);
        VkBool32 ts_cg = 0u;
        float ts_period = 0.0f;
        std::memcpy(&ts_cg, props.limits_and_sparse + kVkLimitsOffsetTimestampComputeAndGraphics, sizeof(ts_cg));
        std::memcpy(&ts_period, props.limits_and_sparse + kVkLimitsOffsetTimestampPeriod, sizeof(ts_period));
        if (ts_cg == 0u || !(ts_period > 0.0f) || !std::isfinite(ts_period)) continue;

        chosen_phys = phys;
        chosen_props = props;
        chosen_ts_period_ns = ts_period;
        if (has_ext && get_domains_ext) {
            chosen_ext_name = "VK_EXT_calibrated_timestamps";
            use_khr_variant = false;
        } else {
            chosen_ext_name = "VK_KHR_calibrated_timestamps";
            use_khr_variant = true;
        }
        if (props.deviceType == 2u) break;  // Prefer discrete GPU when available
    }

    if (!chosen_phys || !chosen_ext_name) {
        return Status::Error(
            StatusCode::NotImplemented,
            "No Vulkan physical device exposes VK_EXT_calibrated_timestamps / VK_KHR_calibrated_timestamps with QPC domain");
    }

    const float q_prio = 1.0f;
    VkDeviceQueueCreateInfoMin qci{};
    qci.sType = kVkStypeDeviceQueueCreateInfo;
    qci.queueFamilyIndex = 0u;
    qci.queueCount = 1u;
    qci.pQueuePriorities = &q_prio;

    const char* enabled_exts[1] = {chosen_ext_name};
    VkDeviceCreateInfoMin dci{};
    dci.sType = kVkStypeDeviceCreateInfo;
    dci.queueCreateInfoCount = 1u;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1u;
    dci.ppEnabledExtensionNames = enabled_exts;

    vr = create_dev(chosen_phys, &dci, nullptr, &cleanup.device);
    if (vr != kVkSuccess || !cleanup.device) {
        return Status::Error(StatusCode::DeviceError, "vkCreateDevice(calibrated_timestamps) failed");
    }

    auto get_cal_ts = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
        get_dev_proc(cleanup.device, use_khr_variant ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT"));
    if (!get_cal_ts) {
        return Status::Error(StatusCode::NotImplemented, "vkGetCalibratedTimestampsEXT/KHR symbol not resolved");
    }

    out_result.device_name = chosen_props.deviceName;
    out_result.calibrated_with_vulkan_ext = true;
    out_result.gpu_timestamp_frequency_valid = true;
    out_result.gpu_tick_period_ns = static_cast<double>(chosen_ts_period_ns);
    out_result.gpu_timestamp_frequency_hz =
        static_cast<uint64_t>((1e9 / static_cast<double>(chosen_ts_period_ns)) + 0.5);
    out_result.cpu_qpc_frequency_hz = static_cast<uint64_t>(qpc_freq.QuadPart);
    out_result.cpu_qpc_period_ns = 1e9 / static_cast<double>(qpc_freq.QuadPart);

    const VkCalibratedTimestampInfoExtMin infos[2] = {
        {kVkStypeCalibratedTimestampInfoExt, nullptr, kVkTimeDomainDeviceExt},
        {kVkStypeCalibratedTimestampInfoExt, nullptr, kVkTimeDomainQueryPerformanceCounterExt},
    };

    const uint32_t clamped_samples = std::clamp<uint32_t>(num_samples, 1u, 64u);
    out_result.samples.reserve(clamped_samples);

    for (uint32_t i = 0u; i < clamped_samples; ++i) {
        LARGE_INTEGER qpc_before{};
        LARGE_INTEGER qpc_after{};
        uint64_t timestamps[2] = {0ull, 0ull};
        uint64_t max_dev_ns = 0ull;

        QueryPerformanceCounter(&qpc_before);
        vr = get_cal_ts(cleanup.device, 2u, infos, timestamps, &max_dev_ns);
        QueryPerformanceCounter(&qpc_after);

        if (vr != kVkSuccess) {
            return Status::Error(StatusCode::DeviceError, "vkGetCalibratedTimestampsEXT failed");
        }

        GpuClockCalibrationSample sample{};
        sample.gpu_timestamp_ticks = timestamps[0];
        sample.cpu_qpc_ticks = timestamps[1];
        sample.qpc_before_ticks = static_cast<uint64_t>(qpc_before.QuadPart);
        sample.qpc_after_ticks = static_cast<uint64_t>(qpc_after.QuadPart);
        sample.max_deviation_ns = max_dev_ns;
        sample.gpu_time_seconds =
            static_cast<double>(timestamps[0]) * (static_cast<double>(chosen_ts_period_ns) * 1e-9);
        sample.cpu_time_seconds =
            static_cast<double>(timestamps[1]) / static_cast<double>(qpc_freq.QuadPart);
        sample.offset_seconds = sample.cpu_time_seconds - sample.gpu_time_seconds;
        out_result.samples.push_back(sample);

        if (i + 1u < clamped_samples && inter_sample_delay_ms > 0u) {
            Sleep(inter_sample_delay_ms);
        }
    }

    FinalizeCalibrationStatistics(out_result);
    return Status::Ok();
#endif
}

}  // namespace openscarlett::d3d12_xs
