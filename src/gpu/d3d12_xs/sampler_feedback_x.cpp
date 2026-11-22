#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "openscarlett/gpu/sampler_feedback_x.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"

#if defined(_WIN32)
#include <d3dcompiler.h>
#endif

namespace openscarlett::d3d12_xs {

namespace {

#if defined(_WIN32)

#if defined(_MSC_VER)
#define OS_SF_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_SF_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_SF_UUIDOF(T) IID_##T
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

    T* Detach() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
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

// ABI-exact mirror of D3D12_FEATURE_DATA_D3D12_OPTIONS7 (works on all Windows SDK / MinGW versions).
struct LocalD3D12Options7 {
    uint32_t MeshShaderTier = 0u;
    uint32_t SamplerFeedbackTier = 0u;
};

inline constexpr uint32_t kD3D12FeatureOptions7Value = 32u;
inline constexpr uint32_t kD3D12ResolveModeEncodeSamplerFeedback = 4u;
inline constexpr uint32_t kD3D12ResolveModeDecodeSamplerFeedback = 5u;

constexpr bool IsPowerOfTwoU32(uint32_t v) noexcept {
    return v != 0u && (v & (v - 1u)) == 0u;
}

constexpr uint32_t DivRoundUpU32(uint32_t num, uint32_t den) noexcept {
    return (den == 0u) ? 0u : ((num + den - 1u) / den);
}

// HLSL SM 5.0 compute shaders for GPU-backed fallback feedback clear and atomic writes.
// Uses root constants + root SRV/UAV so caller descriptor heaps are never disturbed.
constexpr const char* kClearFeedbackHlsl = R"(
cbuffer ClearParams : register(b0) {
    uint element_count;
    uint clear_value;
    uint pad0;
    uint pad1;
};
RWByteAddressBuffer feedback_buf : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < element_count) {
        feedback_buf.Store(tid.x * 4u, clear_value);
    }
}
)";

constexpr const char* kWriteFeedbackHlsl = R"(
cbuffer WriteParams : register(b0) {
    uint sample_count;
    uint tiles_x;
    uint tiles_y;
    uint array_size;
    uint kind;
    uint max_mip;
    uint pad0;
    uint pad1;
};
ByteAddressBuffer samples_buf : register(t0);
RWByteAddressBuffer feedback_buf : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= sample_count) return;
    uint4 s = samples_buf.Load4(tid.x * 16u);
    uint tx = s.x;
    uint ty = s.y;
    uint slice = s.z;
    uint mip = min(s.w, max_mip);
    if (tx >= tiles_x || ty >= tiles_y || slice >= array_size) return;
    uint idx = (slice * tiles_y + ty) * tiles_x + tx;
    uint byte_off = idx * 4u;
    if (kind == 0u) {
        feedback_buf.InterlockedMin(byte_off, mip);
    } else {
        uint bit_index = min(mip, 31u);
        feedback_buf.InterlockedOr(byte_off, (1u << bit_index));
    }
}
)";

struct PackedFeedbackSample {
    uint32_t tile_x = 0u;
    uint32_t tile_y = 0u;
    uint32_t array_slice = 0u;
    uint32_t mip_level = 0u;
};

Status CreateSimpleBuffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    uint64_t byte_size,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    LocalComPtr<ID3D12Resource>& out_res) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0ull;
    rd.Width = std::max<uint64_t>(4ull, byte_size);
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
        OS_SF_UUIDOF(ID3D12Resource), out_res.PutVoid());
    if (FAILED(hr) || !out_res) {
        std::ostringstream oss;
        oss << "CreateCommittedResource(buffer, size=" << byte_size << ") failed hr=0x"
            << std::hex << static_cast<uint32_t>(hr);
        return Status::Error(StatusCode::DeviceError, oss.str());
    }
    return Status::Ok();
}

Status CreateR8Texture2D(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    uint16_t array_size,
    uint16_t mip_levels,
    D3D12_RESOURCE_STATES initial_state,
    LocalComPtr<ID3D12Resource>& out_tex) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment = 0ull;
    rd.Width = std::max<uint32_t>(1u, width);
    rd.Height = std::max<uint32_t>(1u, height);
    rd.DepthOrArraySize = std::max<uint16_t>(1u, array_size);
    rd.MipLevels = std::max<uint16_t>(1u, mip_levels);
    rd.Format = DXGI_FORMAT_R8_UINT;
    rd.SampleDesc.Count = 1u;
    rd.SampleDesc.Quality = 0u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
        OS_SF_UUIDOF(ID3D12Resource), out_tex.PutVoid());
    if (FAILED(hr) || !out_tex) {
        std::ostringstream oss;
        oss << "CreateCommittedResource(R8_UINT " << width << "x" << height
            << ") failed hr=0x" << std::hex << static_cast<uint32_t>(hr);
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

void EmitUavBarrier(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* resource) noexcept {
    if (!cmd_list) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = resource;
    cmd_list->ResourceBarrier(1u, &b);
}

Status CompileCs50(
    const char* hlsl,
    const char* name,
    LocalComPtr<ID3DBlob>& out_blob) {
    LocalComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        hlsl, std::strlen(hlsl), name, nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, out_blob.Put(), err.Put());
    if (FAILED(hr) || !out_blob) {
        std::string msg = std::string("D3DCompile(") + name + ") failed";
        if (err && err->GetBufferPointer() && err->GetBufferSize() > 0u) {
            msg += ": ";
            msg.append(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        }
        return Status::Error(StatusCode::ShaderError, msg);
    }
    return Status::Ok();
}

struct TrackedFeedbackMap {
    uint64_t map_id = 0ull;
    SamplerFeedbackMapDesc desc{};
    SamplerFeedbackTier host_tier = SamplerFeedbackTier::NotSupported;
    bool using_native_hardware = false;
    uint32_t map_tiles_x = 0u;
    uint32_t map_tiles_y = 0u;
    uint32_t total_base_elements = 0u;
    uint64_t clear_count = 0ull;
    uint64_t resolve_count = 0ull;
    uint64_t view_count = 0ull;

    // Owning ref to the feedback resource (native opaque texture or fallback GPU UAV buffer).
    LocalComPtr<ID3D12Resource> feedback_res;
    // Non-owning pointer to paired target texture (cleared on NotifyResourceDestroyed).
    ID3D12Resource* paired_target = nullptr;

    // Optional GPU UAV mirror buffer used when a native MIP_REGION_USED_OPAQUE map is decoded
    // (D3D12 DECODE_SAMPLER_FEEDBACK directly targets R8_UINT for MIN_MIP_OPAQUE).
    LocalComPtr<ID3D12Resource> aux_fallback_uav_buf;

    // Cached dedicated 1-descriptor heaps for native ClearUnorderedAccessViewUint.
    LocalComPtr<ID3D12DescriptorHeap> native_gpu_uav_heap;
    LocalComPtr<ID3D12DescriptorHeap> native_cpu_uav_heap;
    bool native_clear_uavs_created = false;

    // Shadow state for native ENCODE_SAMPLER_FEEDBACK sample synthesis on MIN_MIP_OPAQUE.
    std::vector<uint8_t> native_min_mip_shadow;
};

#endif  // _WIN32

}  // namespace

struct SamplerFeedbackManager::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    SamplerFeedbackTier host_tier = SamplerFeedbackTier::NotSupported;
    bool has_device8 = false;
    uint64_t next_map_id = 1ull;

#if defined(_WIN32)
    LocalComPtr<ID3D12Device> host_device;
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    LocalComPtr<ID3D12Device8> host_device8;
#endif

    // Lazily created compute pipelines for GPU fallback clear & sample writes.
    bool compute_ready = false;
    LocalComPtr<ID3D12RootSignature> clear_root_sig;
    LocalComPtr<ID3D12PipelineState> clear_pso;
    LocalComPtr<ID3D12RootSignature> write_root_sig;
    LocalComPtr<ID3D12PipelineState> write_pso;

    // Internal sync/command objects for synchronous ResolveAndReadback* helpers.
    bool sync_ready = false;
    LocalComPtr<ID3D12CommandAllocator> readback_alloc;
    LocalComPtr<ID3D12GraphicsCommandList> readback_cmd;
    LocalComPtr<ID3D12Fence> readback_fence;
    HANDLE readback_event = nullptr;
    uint64_t readback_fence_val = 0ull;

    std::unordered_map<ID3D12Resource*, TrackedFeedbackMap> maps;
    std::unordered_map<size_t, SamplerFeedbackViewRecord> views_by_cpu_handle;
    // Keeps transient upload/staging resources alive for caller-recorded command lists until Shutdown.
    std::vector<LocalComPtr<ID3D12Resource>> transient_resources;

    Status EnsureComputePipelinesLocked() {
        if (compute_ready) return Status::Ok();
        if (!host_device) {
            return Status::Error(StatusCode::DeviceError, "SamplerFeedbackManager not initialized");
        }

        auto serialize_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
            ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        if (!serialize_fn) {
            return Status::Error(StatusCode::DeviceError, "Failed to resolve D3D12SerializeRootSignature");
        }

        // 1. Clear pipeline: [0] 4 root constants b0, [1] root UAV u0
        {
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.ShaderRegister = 0u;
            params[0].Constants.RegisterSpace = 0u;
            params[0].Constants.Num32BitValues = 4u;
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[1].Descriptor.ShaderRegister = 0u;
            params[1].Descriptor.RegisterSpace = 0u;
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2u;
            rsd.pParameters = params;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            LocalComPtr<ID3DBlob> sig_blob;
            LocalComPtr<ID3DBlob> err_blob;
            HRESULT hr = serialize_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put());
            if (FAILED(hr) || !sig_blob) {
                return Status::Error(StatusCode::DeviceError, "Serialize clear feedback root signature failed");
            }
            hr = host_device->CreateRootSignature(
                0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                OS_SF_UUIDOF(ID3D12RootSignature), clear_root_sig.PutVoid());
            if (FAILED(hr) || !clear_root_sig) {
                return Status::Error(StatusCode::DeviceError, "CreateRootSignature(clear feedback) failed");
            }

            LocalComPtr<ID3DBlob> cs_blob;
            Status st = CompileCs50(kClearFeedbackHlsl, "CSClearFeedback", cs_blob);
            if (!st.ok()) return st;

            D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
            psd.pRootSignature = clear_root_sig.Get();
            psd.CS.pShaderBytecode = cs_blob->GetBufferPointer();
            psd.CS.BytecodeLength = cs_blob->GetBufferSize();
            hr = host_device->CreateComputePipelineState(
                &psd, OS_SF_UUIDOF(ID3D12PipelineState), clear_pso.PutVoid());
            if (FAILED(hr) || !clear_pso) {
                return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(clear feedback) failed");
            }
        }

        // 2. Write pipeline: [0] 8 root constants b0, [1] root SRV t0, [2] root UAV u0
        {
            D3D12_ROOT_PARAMETER params[3]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants.ShaderRegister = 0u;
            params[0].Constants.RegisterSpace = 0u;
            params[0].Constants.Num32BitValues = 8u;
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
                return Status::Error(StatusCode::DeviceError, "Serialize write feedback root signature failed");
            }
            hr = host_device->CreateRootSignature(
                0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                OS_SF_UUIDOF(ID3D12RootSignature), write_root_sig.PutVoid());
            if (FAILED(hr) || !write_root_sig) {
                return Status::Error(StatusCode::DeviceError, "CreateRootSignature(write feedback) failed");
            }

            LocalComPtr<ID3DBlob> cs_blob;
            Status st = CompileCs50(kWriteFeedbackHlsl, "CSWriteFeedback", cs_blob);
            if (!st.ok()) return st;

            D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
            psd.pRootSignature = write_root_sig.Get();
            psd.CS.pShaderBytecode = cs_blob->GetBufferPointer();
            psd.CS.BytecodeLength = cs_blob->GetBufferSize();
            hr = host_device->CreateComputePipelineState(
                &psd, OS_SF_UUIDOF(ID3D12PipelineState), write_pso.PutVoid());
            if (FAILED(hr) || !write_pso) {
                return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(write feedback) failed");
            }
        }

        compute_ready = true;
        return Status::Ok();
    }

    Status EnsureSyncObjectsLocked() {
        if (sync_ready) return Status::Ok();
        if (!host_device) {
            return Status::Error(StatusCode::DeviceError, "SamplerFeedbackManager not initialized");
        }
        HRESULT hr = host_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            OS_SF_UUIDOF(ID3D12CommandAllocator),
            readback_alloc.PutVoid());
        if (FAILED(hr) || !readback_alloc) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandAllocator(feedback readback) failed");
        }
        hr = host_device->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, readback_alloc.Get(), nullptr,
            OS_SF_UUIDOF(ID3D12GraphicsCommandList), readback_cmd.PutVoid());
        if (FAILED(hr) || !readback_cmd) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandList(feedback readback) failed");
        }
        readback_cmd->Close();

        hr = host_device->CreateFence(
            0ull, D3D12_FENCE_FLAG_NONE, OS_SF_UUIDOF(ID3D12Fence), readback_fence.PutVoid());
        if (FAILED(hr) || !readback_fence) {
            return Status::Error(StatusCode::DeviceError, "CreateFence(feedback readback) failed");
        }
        readback_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!readback_event) {
            return Status::Error(StatusCode::DeviceError, "CreateEventA(feedback readback) failed");
        }
        sync_ready = true;
        return Status::Ok();
    }

    Status ExecuteAndWaitLocked(ID3D12CommandQueue* queue) {
        if (!queue) return Status::Error(StatusCode::InvalidArgument, "Null command queue");
        const HRESULT close_hr = readback_cmd->Close();
        if (FAILED(close_hr)) {
            return Status::Error(StatusCode::DeviceError, "readback_cmd->Close failed");
        }
        ID3D12CommandList* lists[1] = {readback_cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);
        const uint64_t target_val = ++readback_fence_val;
        const HRESULT sig_hr = queue->Signal(readback_fence.Get(), target_val);
        if (FAILED(sig_hr)) {
            return Status::Error(StatusCode::DeviceError, "queue->Signal failed");
        }
        if (readback_fence->GetCompletedValue() < target_val) {
            const HRESULT ev_hr = readback_fence->SetEventOnCompletion(target_val, readback_event);
            if (FAILED(ev_hr)) {
                return Status::Error(StatusCode::DeviceError, "SetEventOnCompletion failed");
            }
            const DWORD w = WaitForSingleObject(readback_event, 15000u);
            if (w != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    void PopulateMapInfo(const TrackedFeedbackMap& entry, SamplerFeedbackMapInfo& out) const noexcept {
        out = SamplerFeedbackMapInfo{};
        out.map_id = entry.map_id;
        out.kind = entry.desc.kind;
        out.host_tier = entry.host_tier;
        out.using_native_hardware = entry.using_native_hardware;
        out.target_width = entry.desc.target_width;
        out.target_height = entry.desc.target_height;
        out.target_array_size = entry.desc.target_array_size;
        out.target_mip_levels = entry.desc.target_mip_levels;
        out.mip_region = entry.desc.mip_region;
        out.map_tiles_x = entry.map_tiles_x;
        out.map_tiles_y = entry.map_tiles_y;
        out.clear_count = entry.clear_count;
        out.resolve_count = entry.resolve_count;
        out.view_count = entry.view_count;
        out.feedback_resource = entry.feedback_res.Get();
        out.paired_target_resource = entry.paired_target;
    }

    Status EnsureNativeClearHeapsLocked(TrackedFeedbackMap& entry) {
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
        if (entry.native_clear_uavs_created) return Status::Ok();
        if (!host_device || !host_device8 || !entry.feedback_res) {
            return Status::Error(StatusCode::DeviceError, "ID3D12Device8 unavailable for native clear UAV");
        }
        D3D12_DESCRIPTOR_HEAP_DESC gpu_hd{};
        gpu_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        gpu_hd.NumDescriptors = 1u;
        gpu_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = host_device->CreateDescriptorHeap(
            &gpu_hd, OS_SF_UUIDOF(ID3D12DescriptorHeap), entry.native_gpu_uav_heap.PutVoid());
        if (FAILED(hr) || !entry.native_gpu_uav_heap) {
            return Status::Error(StatusCode::DeviceError, "CreateDescriptorHeap(native feedback GPU) failed");
        }

        D3D12_DESCRIPTOR_HEAP_DESC cpu_hd{};
        cpu_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cpu_hd.NumDescriptors = 1u;
        cpu_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = host_device->CreateDescriptorHeap(
            &cpu_hd, OS_SF_UUIDOF(ID3D12DescriptorHeap), entry.native_cpu_uav_heap.PutVoid());
        if (FAILED(hr) || !entry.native_cpu_uav_heap) {
            return Status::Error(StatusCode::DeviceError, "CreateDescriptorHeap(native feedback CPU) failed");
        }

        host_device8->CreateSamplerFeedbackUnorderedAccessView(
            entry.paired_target,
            entry.feedback_res.Get(),
            entry.native_gpu_uav_heap->GetCPUDescriptorHandleForHeapStart());
        host_device8->CreateSamplerFeedbackUnorderedAccessView(
            entry.paired_target,
            entry.feedback_res.Get(),
            entry.native_cpu_uav_heap->GetCPUDescriptorHandleForHeapStart());

        entry.native_clear_uavs_created = true;
        return Status::Ok();
#else
        (void)entry;
        return Status::Error(StatusCode::NotImplemented, "ID3D12Device8 unavailable at build time");
#endif
    }

    Status RecordFallbackClearLocked(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* uav_buf,
        uint32_t element_count,
        uint32_t clear_value,
        D3D12_RESOURCE_STATES state_before,
        D3D12_RESOURCE_STATES state_after) {
        Status st = EnsureComputePipelinesLocked();
        if (!st.ok()) return st;

        EmitTransition(cmd_list, uav_buf, state_before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const uint32_t constants[4] = {element_count, clear_value, 0u, 0u};
        cmd_list->SetComputeRootSignature(clear_root_sig.Get());
        cmd_list->SetPipelineState(clear_pso.Get());
        cmd_list->SetComputeRoot32BitConstants(0u, 4u, constants, 0u);
        cmd_list->SetComputeRootUnorderedAccessView(1u, uav_buf->GetGPUVirtualAddress());

        const uint32_t groups = std::max<uint32_t>(1u, DivRoundUpU32(element_count, 64u));
        cmd_list->Dispatch(groups, 1u, 1u);
        EmitUavBarrier(cmd_list, uav_buf);
        EmitTransition(cmd_list, uav_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state_after);
        return Status::Ok();
    }

    Status RecordFallbackWritesLocked(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* uav_buf,
        const TrackedFeedbackMap& entry,
        const std::vector<SamplerFeedbackSampleWrite>& samples,
        D3D12_RESOURCE_STATES state_before,
        D3D12_RESOURCE_STATES state_after) {
        if (samples.empty()) return Status::Ok();
        Status st = EnsureComputePipelinesLocked();
        if (!st.ok()) return st;

        const uint64_t upload_bytes = static_cast<uint64_t>(samples.size()) * sizeof(PackedFeedbackSample);
        LocalComPtr<ID3D12Resource> upload_buf;
        st = CreateSimpleBuffer(
            host_device.Get(), D3D12_HEAP_TYPE_UPLOAD, upload_bytes,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload_buf);
        if (!st.ok()) return st;

        void* mapped = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        const HRESULT map_hr = upload_buf->Map(0u, &read_none, &mapped);
        if (FAILED(map_hr) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "Map(feedback sample upload) failed");
        }
        auto* dst_samples = static_cast<PackedFeedbackSample*>(mapped);
        for (size_t i = 0u; i < samples.size(); ++i) {
            dst_samples[i].tile_x = samples[i].tile_x;
            dst_samples[i].tile_y = samples[i].tile_y;
            dst_samples[i].array_slice = samples[i].array_slice;
            dst_samples[i].mip_level = samples[i].mip_level;
        }
        D3D12_RANGE written{0u, static_cast<SIZE_T>(upload_bytes)};
        upload_buf->Unmap(0u, &written);

        EmitTransition(cmd_list, uav_buf, state_before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const uint32_t max_mip = (entry.desc.target_mip_levels > 0u)
                                     ? static_cast<uint32_t>(entry.desc.target_mip_levels - 1u)
                                     : 0u;
        const uint32_t constants[8] = {
            static_cast<uint32_t>(samples.size()),
            entry.map_tiles_x,
            entry.map_tiles_y,
            static_cast<uint32_t>(entry.desc.target_array_size),
            (entry.desc.kind == SamplerFeedbackKind::MinMipOpaque) ? 0u : 1u,
            max_mip,
            0u,
            0u,
        };
        cmd_list->SetComputeRootSignature(write_root_sig.Get());
        cmd_list->SetPipelineState(write_pso.Get());
        cmd_list->SetComputeRoot32BitConstants(0u, 8u, constants, 0u);
        cmd_list->SetComputeRootShaderResourceView(1u, upload_buf->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(2u, uav_buf->GetGPUVirtualAddress());

        const uint32_t groups = std::max<uint32_t>(
            1u, DivRoundUpU32(static_cast<uint32_t>(samples.size()), 64u));
        cmd_list->Dispatch(groups, 1u, 1u);
        EmitUavBarrier(cmd_list, uav_buf);
        EmitTransition(cmd_list, uav_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state_after);

        transient_resources.push_back(std::move(upload_buf));
        return Status::Ok();
    }

    void ReleaseAllLocked() {
        transient_resources.clear();
        views_by_cpu_handle.clear();
        maps.clear();
        readback_cmd.Reset();
        readback_alloc.Reset();
        readback_fence.Reset();
        if (readback_event) {
            CloseHandle(readback_event);
            readback_event = nullptr;
        }
        sync_ready = false;
        write_pso.Reset();
        write_root_sig.Reset();
        clear_pso.Reset();
        clear_root_sig.Reset();
        compute_ready = false;
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
        host_device8.Reset();
#endif
        host_device.Reset();
        host_tier = SamplerFeedbackTier::NotSupported;
        has_device8 = false;
        initialized = false;
    }
#endif
};

SamplerFeedbackManager::SamplerFeedbackManager() : impl_(std::make_unique<Impl>()) {}

SamplerFeedbackManager::~SamplerFeedbackManager() {
    Shutdown();
}

void SamplerFeedbackManager::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(_WIN32)
    impl_->ReleaseAllLocked();
#else
    impl_->initialized = false;
#endif
}

bool SamplerFeedbackManager::IsInitialized() const noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized;
}

SamplerFeedbackTier SamplerFeedbackManager::HostTier() const noexcept {
    if (!impl_) return SamplerFeedbackTier::NotSupported;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->host_tier;
}

bool SamplerFeedbackManager::HasNativeSamplerFeedback() const noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_device8 && impl_->host_tier != SamplerFeedbackTier::NotSupported;
}

bool SamplerFeedbackManager::HasDevice8() const noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_device8;
}

SamplerFeedbackManagerStats SamplerFeedbackManager::GetStats() const {
    SamplerFeedbackManagerStats st{};
    if (!impl_) return st;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    st.host_tier = impl_->host_tier;
    st.has_device8 = impl_->has_device8;
#if defined(_WIN32)
    st.active_maps = static_cast<uint32_t>(impl_->maps.size());
    st.preserved_views = static_cast<uint32_t>(impl_->views_by_cpu_handle.size());
    for (const auto& [res, entry] : impl_->maps) {
        (void)res;
        if (entry.using_native_hardware) {
            ++st.native_maps;
        } else {
            ++st.fallback_maps;
        }
        st.total_clears += entry.clear_count;
        st.total_resolves += entry.resolve_count;
    }
#endif
    return st;
}

#if defined(_WIN32)

SamplerFeedbackTier SamplerFeedbackManager::ProbeHostSamplerFeedbackTier(
    ID3D12Device* host_device) noexcept {
    if (!host_device) return SamplerFeedbackTier::NotSupported;
    LocalD3D12Options7 opts7{};
    const HRESULT hr = host_device->CheckFeatureSupport(
        static_cast<D3D12_FEATURE>(kD3D12FeatureOptions7Value),
        &opts7,
        static_cast<UINT>(sizeof(opts7)));
    if (FAILED(hr)) return SamplerFeedbackTier::NotSupported;
    if (opts7.SamplerFeedbackTier >= static_cast<uint32_t>(SamplerFeedbackTier::Tier1_0)) {
        return SamplerFeedbackTier::Tier1_0;
    }
    if (opts7.SamplerFeedbackTier >= static_cast<uint32_t>(SamplerFeedbackTier::Tier0_9)) {
        return SamplerFeedbackTier::Tier0_9;
    }
    return SamplerFeedbackTier::NotSupported;
}

Status SamplerFeedbackManager::Initialize(ID3D12Device* host_device) {
    if (!host_device) {
        return Status::Error(StatusCode::InvalidArgument, "SamplerFeedbackManager::Initialize: null device");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ReleaseAllLocked();

    host_device->AddRef();
    impl_->host_device.Attach(host_device);
    impl_->host_tier = ProbeHostSamplerFeedbackTier(host_device);

#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    if (SUCCEEDED(host_device->QueryInterface(
            OS_SF_UUIDOF(ID3D12Device8), impl_->host_device8.PutVoid())) &&
        impl_->host_device8) {
        impl_->has_device8 = true;
    }
#endif

    impl_->initialized = true;
    return Status::Ok();
}

Status SamplerFeedbackManager::CreateFeedbackMap(
    const SamplerFeedbackMapDesc& desc,
    ID3D12Resource* paired_target,
    ID3D12Resource** out_feedback_resource,
    SamplerFeedbackMapInfo* out_info) {
    if (!out_feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "CreateFeedbackMap: null output pointer");
    }
    *out_feedback_resource = nullptr;

    SamplerFeedbackMapDesc norm = desc;
    if (norm.target_width == 0u || norm.target_height == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "CreateFeedbackMap: target dimensions must be > 0");
    }
    if (norm.target_array_size == 0u) norm.target_array_size = 1u;
    if (norm.target_mip_levels == 0u) norm.target_mip_levels = 1u;
    if (norm.mip_region.width == 0u) norm.mip_region.width = 16u;
    if (norm.mip_region.height == 0u) norm.mip_region.height = 16u;
    if (norm.mip_region.depth == 0u) norm.mip_region.depth = 1u;

    if (!IsPowerOfTwoU32(norm.mip_region.width) || norm.mip_region.width < 4u ||
        !IsPowerOfTwoU32(norm.mip_region.height) || norm.mip_region.height < 4u) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "CreateFeedbackMap: mip_region width/height must be power-of-two >= 4");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->host_device) {
        return Status::Error(StatusCode::DeviceError, "SamplerFeedbackManager not initialized");
    }

    const uint32_t tiles_x = std::max<uint32_t>(1u, DivRoundUpU32(norm.target_width, norm.mip_region.width));
    const uint32_t tiles_y = std::max<uint32_t>(1u, DivRoundUpU32(norm.target_height, norm.mip_region.height));
    const uint32_t total_elems = tiles_x * tiles_y * static_cast<uint32_t>(norm.target_array_size);

    TrackedFeedbackMap entry{};
    entry.map_id = impl_->next_map_id++;
    entry.desc = norm;
    entry.host_tier = impl_->host_tier;
    entry.map_tiles_x = tiles_x;
    entry.map_tiles_y = tiles_y;
    entry.total_base_elements = total_elems;
    entry.paired_target = paired_target;

    bool try_native = !norm.force_software_fallback &&
                      impl_->has_device8 &&
                      impl_->host_tier != SamplerFeedbackTier::NotSupported;

#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    if (try_native && impl_->host_device8) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1u;
        hp.VisibleNodeMask = 1u;

        D3D12_RESOURCE_DESC1 rd1{};
        rd1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd1.Alignment = 0ull;
        rd1.Width = norm.target_width;
        rd1.Height = norm.target_height;
        rd1.DepthOrArraySize = norm.target_array_size;
        rd1.MipLevels = norm.target_mip_levels;
        rd1.Format = (norm.kind == SamplerFeedbackKind::MinMipOpaque)
                         ? kDxgiFormatSamplerFeedbackMinMipOpaque
                         : kDxgiFormatSamplerFeedbackMipRegionUsedOpaque;
        rd1.SampleDesc.Count = 1u;
        rd1.SampleDesc.Quality = 0u;
        rd1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd1.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        rd1.SamplerFeedbackMipRegion.Width = norm.mip_region.width;
        rd1.SamplerFeedbackMipRegion.Height = norm.mip_region.height;
        rd1.SamplerFeedbackMipRegion.Depth = norm.mip_region.depth;

        LocalComPtr<ID3D12Resource> native_res;
        const HRESULT hr = impl_->host_device8->CreateCommittedResource2(
            &hp,
            D3D12_HEAP_FLAG_NONE,
            &rd1,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            nullptr,
            OS_SF_UUIDOF(ID3D12Resource),
            native_res.PutVoid());
        if (SUCCEEDED(hr) && native_res) {
            entry.using_native_hardware = true;
            entry.feedback_res = std::move(native_res);
            if (norm.kind == SamplerFeedbackKind::MinMipOpaque) {
                entry.native_min_mip_shadow.assign(total_elems, kSamplerFeedbackUnreferencedMinMip);
            } else {
                // Paired GPU UAV mirror for deterministic MipRegionUsed readback across all tiers.
                const uint64_t buf_bytes = static_cast<uint64_t>(total_elems) * sizeof(uint32_t);
                Status aux_st = CreateSimpleBuffer(
                    impl_->host_device.Get(), D3D12_HEAP_TYPE_DEFAULT, buf_bytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    entry.aux_fallback_uav_buf);
                if (!aux_st.ok()) return aux_st;
            }
        } else {
            // Gracefully fall back if driver rejects a specific dimension/region combination.
            try_native = false;
        }
    }
#else
    try_native = false;
#endif

    if (!entry.using_native_hardware) {
        (void)try_native;
        const uint64_t buf_bytes = static_cast<uint64_t>(total_elems) * sizeof(uint32_t);
        Status st = CreateSimpleBuffer(
            impl_->host_device.Get(),
            D3D12_HEAP_TYPE_DEFAULT,
            buf_bytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            entry.feedback_res);
        if (!st.ok()) return st;
    }

    ID3D12Resource* key = entry.feedback_res.Get();
    key->AddRef();
    *out_feedback_resource = key;

    if (out_info) {
        impl_->PopulateMapInfo(entry, *out_info);
    }
    impl_->maps[key] = std::move(entry);
    return Status::Ok();
}

Status SamplerFeedbackManager::RegisterExternalFeedbackMap(
    ID3D12Resource* feedback_resource,
    ID3D12Resource* paired_target,
    const SamplerFeedbackMapDesc& desc,
    bool is_native_resource,
    SamplerFeedbackMapInfo* out_info) {
    if (!feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "RegisterExternalFeedbackMap: null feedback resource");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->host_device) {
        return Status::Error(StatusCode::DeviceError, "SamplerFeedbackManager not initialized");
    }

    SamplerFeedbackMapDesc norm = desc;
    if (norm.target_width == 0u) norm.target_width = 1u;
    if (norm.target_height == 0u) norm.target_height = 1u;
    if (norm.target_array_size == 0u) norm.target_array_size = 1u;
    if (norm.target_mip_levels == 0u) norm.target_mip_levels = 1u;
    if (norm.mip_region.width == 0u) norm.mip_region.width = 16u;
    if (norm.mip_region.height == 0u) norm.mip_region.height = 16u;
    if (norm.mip_region.depth == 0u) norm.mip_region.depth = 1u;

    const uint32_t tiles_x = std::max<uint32_t>(1u, DivRoundUpU32(norm.target_width, norm.mip_region.width));
    const uint32_t tiles_y = std::max<uint32_t>(1u, DivRoundUpU32(norm.target_height, norm.mip_region.height));
    const uint32_t total_elems = tiles_x * tiles_y * static_cast<uint32_t>(norm.target_array_size);

    TrackedFeedbackMap entry{};
    entry.map_id = impl_->next_map_id++;
    entry.desc = norm;
    entry.host_tier = impl_->host_tier;
    entry.using_native_hardware = is_native_resource;
    entry.map_tiles_x = tiles_x;
    entry.map_tiles_y = tiles_y;
    entry.total_base_elements = total_elems;
    entry.paired_target = paired_target;
    feedback_resource->AddRef();
    entry.feedback_res.Attach(feedback_resource);
    if (is_native_resource && norm.kind == SamplerFeedbackKind::MinMipOpaque) {
        entry.native_min_mip_shadow.assign(total_elems, kSamplerFeedbackUnreferencedMinMip);
    }

    if (out_info) {
        impl_->PopulateMapInfo(entry, *out_info);
    }
    impl_->maps[feedback_resource] = std::move(entry);
    return Status::Ok();
}

Status SamplerFeedbackManager::UnregisterFeedbackMap(ID3D12Resource* feedback_resource) {
    if (!feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "UnregisterFeedbackMap: null resource");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "Feedback resource not tracked");
    }
    for (auto vit = impl_->views_by_cpu_handle.begin(); vit != impl_->views_by_cpu_handle.end(); ) {
        if (vit->second.feedback_resource == feedback_resource) {
            vit = impl_->views_by_cpu_handle.erase(vit);
        } else {
            ++vit;
        }
    }
    impl_->maps.erase(it);
    return Status::Ok();
}

void SamplerFeedbackManager::NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept {
    if (!host_resource || !impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->maps.erase(host_resource);
    for (auto& [key, entry] : impl_->maps) {
        (void)key;
        if (entry.paired_target == host_resource) {
            entry.paired_target = nullptr;
        }
    }
    for (auto vit = impl_->views_by_cpu_handle.begin(); vit != impl_->views_by_cpu_handle.end(); ) {
        if (vit->second.feedback_resource == host_resource ||
            vit->second.paired_target_resource == host_resource) {
            vit = impl_->views_by_cpu_handle.erase(vit);
        } else {
            ++vit;
        }
    }
}

bool SamplerFeedbackManager::LookupFeedbackMap(
    ID3D12Resource* feedback_resource,
    SamplerFeedbackMapInfo& out_info) const {
    if (!feedback_resource || !impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) return false;
    impl_->PopulateMapInfo(it->second, out_info);
    return true;
}

Status SamplerFeedbackManager::CreateSamplerFeedbackUnorderedAccessView(
    ID3D12Resource* targeted_resource,
    ID3D12Resource* feedback_resource,
    D3D12_CPU_DESCRIPTOR_HANDLE dest_descriptor,
    SamplerFeedbackViewRecord* out_record) {
    if (!feedback_resource || dest_descriptor.ptr == 0u) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "CreateSamplerFeedbackUnorderedAccessView: null feedback resource or descriptor handle");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->host_device) {
        return Status::Error(StatusCode::DeviceError, "SamplerFeedbackManager not initialized");
    }

    auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "Feedback resource is not tracked by SamplerFeedbackManager");
    }

    TrackedFeedbackMap& entry = it->second;
    ID3D12Resource* effective_target = targeted_resource ? targeted_resource : entry.paired_target;
    if (targeted_resource && !entry.paired_target) {
        entry.paired_target = targeted_resource;
    }

    bool created_native = false;
#if defined(__ID3D12Device8_INTERFACE_DEFINED__)
    if (entry.using_native_hardware && impl_->host_device8) {
        if (effective_target) {
            const D3D12_RESOURCE_DESC tdesc = effective_target->GetDesc();
            if (tdesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                tdesc.Width != entry.desc.target_width ||
                tdesc.Height != entry.desc.target_height ||
                tdesc.DepthOrArraySize != entry.desc.target_array_size ||
                tdesc.MipLevels != entry.desc.target_mip_levels) {
                return Status::Error(
                    StatusCode::InvalidArgument,
                    "CreateSamplerFeedbackUnorderedAccessView: paired target texture dimensions/array/mips mismatch");
            }
        }
        impl_->host_device8->CreateSamplerFeedbackUnorderedAccessView(
            effective_target, feedback_resource, dest_descriptor);
        created_native = true;
    }
#endif

    if (!created_native) {
        // Fallback path: write a valid typed R32_UINT buffer UAV so the descriptor slot stays
        // completely valid for desktop D3D12 validation & shader binding.
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0ull;
        uav.Buffer.NumElements = entry.total_base_elements;
        uav.Buffer.StructureByteStride = 0u;
        uav.Buffer.CounterOffsetInBytes = 0ull;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        impl_->host_device->CreateUnorderedAccessView(
            feedback_resource, nullptr, &uav, dest_descriptor);
    }

    ++entry.view_count;

    SamplerFeedbackViewRecord rec{};
    rec.map_id = entry.map_id;
    rec.dest_cpu_descriptor_ptr = dest_descriptor.ptr;
    rec.is_native_view = created_native;
    rec.kind = entry.desc.kind;
    rec.feedback_resource = feedback_resource;
    rec.paired_target_resource = effective_target;

    impl_->views_by_cpu_handle[dest_descriptor.ptr] = rec;
    if (out_record) *out_record = rec;
    return Status::Ok();
}

bool SamplerFeedbackManager::LookupPreservedView(
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
    SamplerFeedbackViewRecord& out_record) const {
    if (!impl_ || cpu_handle.ptr == 0u) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->views_by_cpu_handle.find(cpu_handle.ptr);
    if (it == impl_->views_by_cpu_handle.end()) return false;
    out_record = it->second;
    return true;
}

void SamplerFeedbackManager::CopyPreservedView(
    D3D12_CPU_DESCRIPTOR_HANDLE dst_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE src_handle) {
    if (!impl_ || dst_handle.ptr == 0u || src_handle.ptr == 0u) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->views_by_cpu_handle.find(src_handle.ptr);
    if (it == impl_->views_by_cpu_handle.end()) return;
    SamplerFeedbackViewRecord copied = it->second;
    copied.dest_cpu_descriptor_ptr = dst_handle.ptr;
    impl_->views_by_cpu_handle[dst_handle.ptr] = copied;
}

Status SamplerFeedbackManager::ClearSamplerFeedbackMap(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* feedback_resource,
    D3D12_RESOURCE_STATES state_before,
    D3D12_RESOURCE_STATES state_after) {
    if (!cmd_list || !feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "ClearSamplerFeedbackMap: null argument");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "ClearSamplerFeedbackMap: feedback resource not tracked");
    }
    TrackedFeedbackMap& entry = it->second;

    const uint32_t clear_u32 = (entry.desc.kind == SamplerFeedbackKind::MinMipOpaque)
                                   ? static_cast<uint32_t>(kSamplerFeedbackUnreferencedMinMip)
                                   : static_cast<uint32_t>(kSamplerFeedbackRegionUnused);

    if (entry.using_native_hardware) {
        Status st = impl_->EnsureNativeClearHeapsLocked(entry);
        if (!st.ok()) return st;

        EmitTransition(cmd_list, feedback_resource, state_before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* heaps[1] = {entry.native_gpu_uav_heap.Get()};
        cmd_list->SetDescriptorHeaps(1u, heaps);

        const UINT clear_vals[4] = {clear_u32, clear_u32, clear_u32, clear_u32};
        cmd_list->ClearUnorderedAccessViewUint(
            entry.native_gpu_uav_heap->GetGPUDescriptorHandleForHeapStart(),
            entry.native_cpu_uav_heap->GetCPUDescriptorHandleForHeapStart(),
            feedback_resource,
            clear_vals,
            0u,
            nullptr);
        EmitUavBarrier(cmd_list, feedback_resource);
        EmitTransition(cmd_list, feedback_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state_after);

        if (entry.desc.kind == SamplerFeedbackKind::MinMipOpaque) {
            std::fill(entry.native_min_mip_shadow.begin(),
                      entry.native_min_mip_shadow.end(),
                      kSamplerFeedbackUnreferencedMinMip);
        } else if (entry.aux_fallback_uav_buf) {
            Status aux_st = impl_->RecordFallbackClearLocked(
                cmd_list, entry.aux_fallback_uav_buf.Get(), entry.total_base_elements,
                clear_u32, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!aux_st.ok()) return aux_st;
        }
    } else {
        Status st = impl_->RecordFallbackClearLocked(
            cmd_list, feedback_resource, entry.total_base_elements, clear_u32, state_before, state_after);
        if (!st.ok()) return st;
    }

    ++entry.clear_count;
    return Status::Ok();
}

Status SamplerFeedbackManager::RecordFeedbackSampleWrites(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* feedback_resource,
    const std::vector<SamplerFeedbackSampleWrite>& samples,
    D3D12_RESOURCE_STATES state_before,
    D3D12_RESOURCE_STATES state_after) {
    if (!cmd_list || !feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "RecordFeedbackSampleWrites: null argument");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "RecordFeedbackSampleWrites: feedback resource not tracked");
    }
    TrackedFeedbackMap& entry = it->second;
    if (samples.empty()) return Status::Ok();

    if (entry.using_native_hardware) {
        if (entry.desc.kind == SamplerFeedbackKind::MinMipOpaque) {
            // Update shadow min-mip map, upload into R8_UINT texture, and invoke native
            // ID3D12GraphicsCommandList1::ResolveSubresourceRegion(ENCODE_SAMPLER_FEEDBACK).
            const uint8_t max_mip = (entry.desc.target_mip_levels > 0u)
                                        ? static_cast<uint8_t>(entry.desc.target_mip_levels - 1u)
                                        : 0u;
            for (const auto& s : samples) {
                if (s.tile_x >= entry.map_tiles_x ||
                    s.tile_y >= entry.map_tiles_y ||
                    s.array_slice >= entry.desc.target_array_size) {
                    continue;
                }
                const size_t idx =
                    (static_cast<size_t>(s.array_slice) * entry.map_tiles_y + s.tile_y) * entry.map_tiles_x + s.tile_x;
                const uint8_t clamped_mip = std::min<uint8_t>(s.mip_level, max_mip);
                entry.native_min_mip_shadow[idx] = std::min<uint8_t>(entry.native_min_mip_shadow[idx], clamped_mip);
            }

            LocalComPtr<ID3D12GraphicsCommandList1> list1;
            if (FAILED(cmd_list->QueryInterface(OS_SF_UUIDOF(ID3D12GraphicsCommandList1), list1.PutVoid())) || !list1) {
                return Status::Error(StatusCode::DeviceError, "ID3D12GraphicsCommandList1 unavailable for ENCODE resolve");
            }

            LocalComPtr<ID3D12Resource> src_r8_tex;
            Status st = CreateR8Texture2D(
                impl_->host_device.Get(),
                entry.map_tiles_x,
                entry.map_tiles_y,
                entry.desc.target_array_size,
                1u,
                D3D12_RESOURCE_STATE_COPY_DEST,
                src_r8_tex);
            if (!st.ok()) return st;

            const D3D12_RESOURCE_DESC r8_desc = src_r8_tex->GetDesc();
            const uint32_t num_subs = entry.desc.target_array_size;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(num_subs);
            std::vector<UINT> num_rows(num_subs, 0u);
            std::vector<UINT64> row_sizes(num_subs, 0ull);
            UINT64 total_bytes = 0ull;
            impl_->host_device->GetCopyableFootprints(
                &r8_desc, 0u, num_subs, 0ull, footprints.data(), num_rows.data(), row_sizes.data(), &total_bytes);

            LocalComPtr<ID3D12Resource> upload_buf;
            st = CreateSimpleBuffer(
                impl_->host_device.Get(), D3D12_HEAP_TYPE_UPLOAD, total_bytes,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload_buf);
            if (!st.ok()) return st;

            void* mapped = nullptr;
            D3D12_RANGE read_none{0u, 0u};
            if (FAILED(upload_buf->Map(0u, &read_none, &mapped)) || !mapped) {
                return Status::Error(StatusCode::DeviceError, "Map(native feedback encode upload) failed");
            }
            auto* dst_bytes = static_cast<uint8_t*>(mapped);
            for (uint32_t slice = 0u; slice < num_subs; ++slice) {
                const uint64_t base_off = footprints[slice].Offset;
                const uint32_t pitch = footprints[slice].Footprint.RowPitch;
                for (uint32_t y = 0u; y < entry.map_tiles_y; ++y) {
                    const uint8_t* src_row =
                        entry.native_min_mip_shadow.data() +
                        (static_cast<size_t>(slice) * entry.map_tiles_y + y) * entry.map_tiles_x;
                    std::memcpy(dst_bytes + base_off + static_cast<uint64_t>(y) * pitch, src_row, entry.map_tiles_x);
                }
            }
            D3D12_RANGE written{0u, static_cast<SIZE_T>(total_bytes)};
            upload_buf->Unmap(0u, &written);

            for (uint32_t slice = 0u; slice < num_subs; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                dst_loc.pResource = src_r8_tex.Get();
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = slice;

                D3D12_TEXTURE_COPY_LOCATION src_loc{};
                src_loc.pResource = upload_buf.Get();
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprints[slice];

                cmd_list->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
            }

            EmitTransition(cmd_list, src_r8_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            EmitTransition(cmd_list, feedback_resource, state_before, D3D12_RESOURCE_STATE_RESOLVE_DEST);

            // Per D3D12 Sampler Feedback spec, UINT_MAX transcodes all array slices of
            // MIN_MIP_OPAQUE in a single ResolveSubresourceRegion call.
            list1->ResolveSubresourceRegion(
                feedback_resource,
                UINT_MAX,
                0u,
                0u,
                src_r8_tex.Get(),
                UINT_MAX,
                nullptr,
                DXGI_FORMAT_R8_UINT,
                static_cast<D3D12_RESOLVE_MODE>(kD3D12ResolveModeEncodeSamplerFeedback));

            EmitTransition(cmd_list, feedback_resource, D3D12_RESOURCE_STATE_RESOLVE_DEST, state_after);

            impl_->transient_resources.push_back(std::move(upload_buf));
            impl_->transient_resources.push_back(std::move(src_r8_tex));
            return Status::Ok();
        }

        // Native MIP_REGION_USED_OPAQUE: record onto paired GPU UAV mirror buffer for deterministic decode.
        if (entry.aux_fallback_uav_buf) {
            return impl_->RecordFallbackWritesLocked(
                cmd_list, entry.aux_fallback_uav_buf.Get(), entry, samples,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        return Status::Ok();
    }

    return impl_->RecordFallbackWritesLocked(
        cmd_list, feedback_resource, entry, samples, state_before, state_after);
}

Status SamplerFeedbackManager::RecordResolveToR8Texture(
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* feedback_resource,
    ID3D12Resource* dst_r8_texture,
    D3D12_RESOURCE_STATES feedback_state_before,
    D3D12_RESOURCE_STATES feedback_state_after,
    D3D12_RESOURCE_STATES dst_state_before,
    D3D12_RESOURCE_STATES dst_state_after) {
    if (!cmd_list || !feedback_resource || !dst_r8_texture) {
        return Status::Error(StatusCode::InvalidArgument, "RecordResolveToR8Texture: null argument");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "RecordResolveToR8Texture: feedback resource not tracked");
    }
    TrackedFeedbackMap& entry = it->second;
    if (!entry.using_native_hardware) {
        return Status::Error(
            StatusCode::UnsupportedFormat,
            "RecordResolveToR8Texture requires a native hardware sampler feedback map; use ResolveAndReadback*");
    }

    LocalComPtr<ID3D12GraphicsCommandList1> list1;
    if (FAILED(cmd_list->QueryInterface(OS_SF_UUIDOF(ID3D12GraphicsCommandList1), list1.PutVoid())) || !list1) {
        return Status::Error(StatusCode::DeviceError, "ID3D12GraphicsCommandList1 unavailable");
    }

    EmitTransition(cmd_list, feedback_resource, feedback_state_before, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    EmitTransition(cmd_list, dst_r8_texture, dst_state_before, D3D12_RESOURCE_STATE_RESOLVE_DEST);

    list1->ResolveSubresourceRegion(
        dst_r8_texture,
        UINT_MAX,
        0u,
        0u,
        feedback_resource,
        UINT_MAX,
        nullptr,
        DXGI_FORMAT_R8_UINT,
        static_cast<D3D12_RESOLVE_MODE>(kD3D12ResolveModeDecodeSamplerFeedback));

    EmitTransition(cmd_list, feedback_resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, feedback_state_after);
    EmitTransition(cmd_list, dst_r8_texture, D3D12_RESOURCE_STATE_RESOLVE_DEST, dst_state_after);

    ++entry.resolve_count;
    return Status::Ok();
}

Status SamplerFeedbackManager::ResolveAndReadbackMinMip(
    ID3D12CommandQueue* queue,
    ID3D12Resource* feedback_resource,
    SamplerFeedbackMinMipReadback& out_readback,
    D3D12_RESOURCE_STATES resource_state) {
    out_readback = SamplerFeedbackMinMipReadback{};
    if (!queue || !feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveAndReadbackMinMip: null argument");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "ResolveAndReadbackMinMip: feedback resource not tracked");
    }
    TrackedFeedbackMap& entry = it->second;
    if (entry.desc.kind != SamplerFeedbackKind::MinMipOpaque) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveAndReadbackMinMip: resource is not MinMipOpaque");
    }

    Status st = impl_->EnsureSyncObjectsLocked();
    if (!st.ok()) return st;

    if (FAILED(impl_->readback_alloc->Reset()) ||
        FAILED(impl_->readback_cmd->Reset(impl_->readback_alloc.Get(), nullptr))) {
        return Status::Error(StatusCode::DeviceError, "Failed to reset internal readback command list");
    }

    out_readback.tiles_x = entry.map_tiles_x;
    out_readback.tiles_y = entry.map_tiles_y;
    out_readback.array_size = entry.desc.target_array_size;
    out_readback.used_native_resolve = entry.using_native_hardware;
    out_readback.min_mip_tiles.assign(entry.total_base_elements, kSamplerFeedbackUnreferencedMinMip);

    if (entry.using_native_hardware) {
        LocalComPtr<ID3D12GraphicsCommandList1> list1;
        if (FAILED(impl_->readback_cmd->QueryInterface(OS_SF_UUIDOF(ID3D12GraphicsCommandList1), list1.PutVoid())) ||
            !list1) {
            impl_->readback_cmd->Close();
            return Status::Error(StatusCode::DeviceError, "ID3D12GraphicsCommandList1 unavailable");
        }

        LocalComPtr<ID3D12Resource> dst_r8_tex;
        st = CreateR8Texture2D(
            impl_->host_device.Get(),
            entry.map_tiles_x,
            entry.map_tiles_y,
            entry.desc.target_array_size,
            1u,
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
            dst_r8_tex);
        if (!st.ok()) {
            impl_->readback_cmd->Close();
            return st;
        }

        EmitTransition(impl_->readback_cmd.Get(), feedback_resource, resource_state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        const uint32_t num_subs = entry.desc.target_array_size;
        list1->ResolveSubresourceRegion(
            dst_r8_tex.Get(),
            UINT_MAX,
            0u,
            0u,
            feedback_resource,
            UINT_MAX,
            nullptr,
            DXGI_FORMAT_R8_UINT,
            static_cast<D3D12_RESOLVE_MODE>(kD3D12ResolveModeDecodeSamplerFeedback));
        EmitTransition(impl_->readback_cmd.Get(), feedback_resource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, resource_state);
        EmitTransition(impl_->readback_cmd.Get(), dst_r8_tex.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

        const D3D12_RESOURCE_DESC r8_desc = dst_r8_tex->GetDesc();
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(num_subs);
        std::vector<UINT> num_rows(num_subs, 0u);
        std::vector<UINT64> row_sizes(num_subs, 0ull);
        UINT64 total_bytes = 0ull;
        impl_->host_device->GetCopyableFootprints(
            &r8_desc, 0u, num_subs, 0ull, footprints.data(), num_rows.data(), row_sizes.data(), &total_bytes);

        LocalComPtr<ID3D12Resource> rb_buf;
        st = CreateSimpleBuffer(
            impl_->host_device.Get(), D3D12_HEAP_TYPE_READBACK, total_bytes,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf);
        if (!st.ok()) {
            impl_->readback_cmd->Close();
            return st;
        }

        for (uint32_t slice = 0u; slice < num_subs; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = rb_buf.Get();
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint = footprints[slice];

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = dst_r8_tex.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_loc.SubresourceIndex = slice;

            impl_->readback_cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
        }

        st = impl_->ExecuteAndWaitLocked(queue);
        if (!st.ok()) return st;

        void* mapped = nullptr;
        D3D12_RANGE read_range{0u, static_cast<SIZE_T>(total_bytes)};
        if (FAILED(rb_buf->Map(0u, &read_range, &mapped)) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "Map(native min-mip readback) failed");
        }
        const auto* src_bytes = static_cast<const uint8_t*>(mapped);
        for (uint32_t slice = 0u; slice < num_subs; ++slice) {
            const uint64_t base_off = footprints[slice].Offset;
            const uint32_t pitch = footprints[slice].Footprint.RowPitch;
            for (uint32_t y = 0u; y < entry.map_tiles_y; ++y) {
                uint8_t* dst_row =
                    out_readback.min_mip_tiles.data() +
                    (static_cast<size_t>(slice) * entry.map_tiles_y + y) * entry.map_tiles_x;
                std::memcpy(dst_row, src_bytes + base_off + static_cast<uint64_t>(y) * pitch, entry.map_tiles_x);
            }
        }
        D3D12_RANGE write_none{0u, 0u};
        rb_buf->Unmap(0u, &write_none);
    } else {
        const uint64_t buf_bytes = static_cast<uint64_t>(entry.total_base_elements) * sizeof(uint32_t);
        LocalComPtr<ID3D12Resource> rb_buf;
        st = CreateSimpleBuffer(
            impl_->host_device.Get(), D3D12_HEAP_TYPE_READBACK, buf_bytes,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf);
        if (!st.ok()) {
            impl_->readback_cmd->Close();
            return st;
        }

        EmitTransition(impl_->readback_cmd.Get(), feedback_resource, resource_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        impl_->readback_cmd->CopyBufferRegion(rb_buf.Get(), 0ull, feedback_resource, 0ull, buf_bytes);
        EmitTransition(impl_->readback_cmd.Get(), feedback_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, resource_state);

        st = impl_->ExecuteAndWaitLocked(queue);
        if (!st.ok()) return st;

        void* mapped = nullptr;
        D3D12_RANGE read_range{0u, static_cast<SIZE_T>(buf_bytes)};
        if (FAILED(rb_buf->Map(0u, &read_range, &mapped)) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "Map(fallback min-mip readback) failed");
        }
        const auto* src_u32 = static_cast<const uint32_t*>(mapped);
        for (uint32_t i = 0u; i < entry.total_base_elements; ++i) {
            const uint32_t v = src_u32[i];
            out_readback.min_mip_tiles[i] = (v >= kSamplerFeedbackUnreferencedMinMip)
                                                ? kSamplerFeedbackUnreferencedMinMip
                                                : static_cast<uint8_t>(v & 0xFFu);
        }
        D3D12_RANGE write_none{0u, 0u};
        rb_buf->Unmap(0u, &write_none);
    }

    ++entry.resolve_count;
    return Status::Ok();
}

Status SamplerFeedbackManager::ResolveAndReadbackMipRegionUsed(
    ID3D12CommandQueue* queue,
    ID3D12Resource* feedback_resource,
    SamplerFeedbackMipRegionUsedReadback& out_readback,
    D3D12_RESOURCE_STATES resource_state) {
    out_readback = SamplerFeedbackMipRegionUsedReadback{};
    if (!queue || !feedback_resource) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveAndReadbackMipRegionUsed: null argument");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->maps.find(feedback_resource);
    if (it == impl_->maps.end()) {
        return Status::Error(StatusCode::NotFound, "ResolveAndReadbackMipRegionUsed: feedback resource not tracked");
    }
    TrackedFeedbackMap& entry = it->second;
    if (entry.desc.kind != SamplerFeedbackKind::MipRegionUsedOpaque) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "ResolveAndReadbackMipRegionUsed: resource is not MipRegionUsedOpaque");
    }

    Status st = impl_->EnsureSyncObjectsLocked();
    if (!st.ok()) return st;

    if (FAILED(impl_->readback_alloc->Reset()) ||
        FAILED(impl_->readback_cmd->Reset(impl_->readback_alloc.Get(), nullptr))) {
        return Status::Error(StatusCode::DeviceError, "Failed to reset internal readback command list");
    }

    ID3D12Resource* src_uav_buf = entry.using_native_hardware
                                      ? entry.aux_fallback_uav_buf.Get()
                                      : feedback_resource;
    const D3D12_RESOURCE_STATES src_state = entry.using_native_hardware
                                                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                : resource_state;
    if (!src_uav_buf) {
        impl_->readback_cmd->Close();
        return Status::Error(StatusCode::DeviceError, "MipRegionUsed backing UAV buffer unavailable");
    }

    const uint64_t buf_bytes = static_cast<uint64_t>(entry.total_base_elements) * sizeof(uint32_t);
    LocalComPtr<ID3D12Resource> rb_buf;
    st = CreateSimpleBuffer(
        impl_->host_device.Get(), D3D12_HEAP_TYPE_READBACK, buf_bytes,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf);
    if (!st.ok()) {
        impl_->readback_cmd->Close();
        return st;
    }

    EmitTransition(impl_->readback_cmd.Get(), src_uav_buf, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    impl_->readback_cmd->CopyBufferRegion(rb_buf.Get(), 0ull, src_uav_buf, 0ull, buf_bytes);
    EmitTransition(impl_->readback_cmd.Get(), src_uav_buf, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);

    st = impl_->ExecuteAndWaitLocked(queue);
    if (!st.ok()) return st;

    out_readback.base_tiles_x = entry.map_tiles_x;
    out_readback.base_tiles_y = entry.map_tiles_y;
    out_readback.array_size = entry.desc.target_array_size;
    out_readback.mip_levels = entry.desc.target_mip_levels;
    out_readback.used_native_resolve = false;
    out_readback.tile_mip_bitmasks.assign(entry.total_base_elements, 0u);

    void* mapped = nullptr;
    D3D12_RANGE read_range{0u, static_cast<SIZE_T>(buf_bytes)};
    if (FAILED(rb_buf->Map(0u, &read_range, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(mip-region-used readback) failed");
    }
    std::memcpy(out_readback.tile_mip_bitmasks.data(), mapped, static_cast<size_t>(buf_bytes));
    D3D12_RANGE write_none{0u, 0u};
    rb_buf->Unmap(0u, &write_none);

    // Decode per-(slice, mip) 2D used-flag planes from the GPU-accumulated bitmasks.
    out_readback.mip_planes.reserve(
        static_cast<size_t>(entry.desc.target_array_size) * entry.desc.target_mip_levels);
    for (uint32_t slice = 0u; slice < entry.desc.target_array_size; ++slice) {
        for (uint32_t mip = 0u; mip < entry.desc.target_mip_levels; ++mip) {
            const uint32_t mip_w = std::max<uint32_t>(1u, entry.desc.target_width >> mip);
            const uint32_t mip_h = std::max<uint32_t>(1u, entry.desc.target_height >> mip);
            const uint32_t mip_tx = std::max<uint32_t>(1u, DivRoundUpU32(mip_w, entry.desc.mip_region.width));
            const uint32_t mip_ty = std::max<uint32_t>(1u, DivRoundUpU32(mip_h, entry.desc.mip_region.height));

            SamplerFeedbackMipRegionUsedPlane plane{};
            plane.mip_level = mip;
            plane.array_slice = slice;
            plane.tiles_x = mip_tx;
            plane.tiles_y = mip_ty;
            plane.used_flags.assign(static_cast<size_t>(mip_tx) * mip_ty, kSamplerFeedbackRegionUnused);

            const uint32_t mip_bit = (mip < 32u) ? (1u << mip) : 0u;
            if (mip_bit != 0u) {
                for (uint32_t by = 0u; by < entry.map_tiles_y; ++by) {
                    const uint32_t my = std::min<uint32_t>(by >> mip, mip_ty - 1u);
                    for (uint32_t bx = 0u; bx < entry.map_tiles_x; ++bx) {
                        const uint32_t mask = out_readback.BitmaskAt(bx, by, slice);
                        if ((mask & mip_bit) != 0u) {
                            const uint32_t mx = std::min<uint32_t>(bx >> mip, mip_tx - 1u);
                            plane.used_flags[static_cast<size_t>(my) * mip_tx + mx] = kSamplerFeedbackRegionUsed;
                        }
                    }
                }
            }
            out_readback.mip_planes.push_back(std::move(plane));
        }
    }

    ++entry.resolve_count;
    return Status::Ok();
}

#endif  // _WIN32

}  // namespace openscarlett::d3d12_xs
