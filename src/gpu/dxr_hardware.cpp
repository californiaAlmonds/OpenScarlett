#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

#include "openscarlett/core/telemetry.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace openscarlett {

namespace {

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

Status RecordDxrResult(const char* symbol, const Status& st, int32_t hr = 0) {
    const CallOutcome outcome = st.ok() ? CallOutcome::Ok : CallOutcome::Error;
    const int32_t resolved_hr = st.ok() ? 0 : (hr != 0 ? hr : static_cast<int32_t>(0x80004005u));
    CallTelemetry::Global().Record("dxr_hardware", symbol, outcome, resolved_hr);
    return st;
}

bool IsZeroIdentifier(const std::array<uint8_t, kDxrShaderIdentifierSize>& id) noexcept {
    for (uint8_t b : id) {
        if (b != 0) return false;
    }
    return true;
}

// Deterministic 32-byte shader identifier derived from export name when no live
// ID3D12StateObjectProperties::GetShaderIdentifier blob is provided.
std::array<uint8_t, kDxrShaderIdentifierSize> DeriveShaderIdentifier(const std::string& name) {
    std::array<uint8_t, kDxrShaderIdentifierSize> id{};
    for (uint32_t lane = 0; lane < 4; ++lane) {
        uint64_t h = 14695981039346656037ull ^ (static_cast<uint64_t>(lane) * 0x9E3779B97F4A7C15ull);
        for (unsigned char c : name) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= static_cast<uint64_t>(name.size()) + (static_cast<uint64_t>(lane) << 32);
        h *= 1099511628211ull;
        std::memcpy(id.data() + static_cast<size_t>(lane) * 8u, &h, 8u);
    }
    if (IsZeroIdentifier(id)) {
        id[0] = 0x4F;  // 'O'
        id[1] = 0x53;  // 'S'
    }
    return id;
}

DxrPrebuildInfo ComputeAnalyticPrebuildInfo(const DxrAsInputs& inputs) noexcept {
    DxrPrebuildInfo info{};
    const uint64_t n = std::max<uint64_t>(1ull, inputs.primitive_count);
    const bool allow_update = (inputs.build_flags & kDxrBuildAllowUpdate) != 0;
    const bool minimize_mem = (inputs.build_flags & kDxrBuildMinimizeMemory) != 0;
    const bool fast_trace = (inputs.build_flags & kDxrBuildPreferFastTrace) != 0;

    if (inputs.type == DxrAsType::BottomLevel) {
        const uint64_t bytes_per_tri = minimize_mem ? 160ull : (fast_trace ? 224ull : 192ull);
        const uint64_t scratch_per_tri = fast_trace ? 160ull : 128ull;
        const uint64_t geom_overhead = static_cast<uint64_t>(std::max<uint32_t>(1u, inputs.geometry_count)) * 64ull;
        info.result_data_max_size_bytes = AlignUp(256ull + geom_overhead + n * bytes_per_tri, kDxrAsByteAlignment);
        info.scratch_data_size_bytes = AlignUp(256ull + geom_overhead + n * scratch_per_tri, kDxrAsByteAlignment);
        info.update_scratch_data_size_bytes =
            allow_update ? AlignUp(256ull + n * 64ull, kDxrAsByteAlignment) : 0ull;
    } else {
        info.result_data_max_size_bytes = AlignUp(256ull + n * 128ull, kDxrAsByteAlignment);
        info.scratch_data_size_bytes = AlignUp(256ull + n * 128ull, kDxrAsByteAlignment);
        info.update_scratch_data_size_bytes =
            allow_update ? AlignUp(256ull + n * 64ull, kDxrAsByteAlignment) : 0ull;
    }
    info.from_hardware_driver = false;
    return info;
}

#if defined(_WIN32)

// Verify ABI-exact alignment and layout parity with <d3d12.h> DXR structures.
static_assert(kDxrShaderIdentifierSize == D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
              "Shader identifier size mismatch");
static_assert(kDxrShaderRecordAlignment == D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT,
              "Shader record alignment mismatch");
static_assert(kDxrShaderTableAlignment == D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
              "Shader table alignment mismatch");
static_assert(kDxrAsByteAlignment == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT,
              "AS alignment mismatch");
static_assert(kDxrInstanceDescSize == sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
              "Instance desc size mismatch");

static_assert(sizeof(DxrGpuVirtualAddressRange) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE),
              "GPU VA range size mismatch");
static_assert(offsetof(DxrGpuVirtualAddressRange, StartAddress) ==
                  offsetof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE, StartAddress),
              "GPU VA range StartAddress offset mismatch");
static_assert(offsetof(DxrGpuVirtualAddressRange, SizeInBytes) ==
                  offsetof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE, SizeInBytes),
              "GPU VA range SizeInBytes offset mismatch");

static_assert(sizeof(DxrGpuVirtualAddressRangeAndStride) == sizeof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE),
              "GPU VA range+stride size mismatch");
static_assert(offsetof(DxrGpuVirtualAddressRangeAndStride, StartAddress) ==
                  offsetof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE, StartAddress),
              "GPU VA range+stride StartAddress offset mismatch");
static_assert(offsetof(DxrGpuVirtualAddressRangeAndStride, SizeInBytes) ==
                  offsetof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE, SizeInBytes),
              "GPU VA range+stride SizeInBytes offset mismatch");
static_assert(offsetof(DxrGpuVirtualAddressRangeAndStride, StrideInBytes) ==
                  offsetof(D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE, StrideInBytes),
              "GPU VA range+stride StrideInBytes offset mismatch");

static_assert(sizeof(DxrDispatchRaysDesc) == sizeof(D3D12_DISPATCH_RAYS_DESC),
              "D3D12_DISPATCH_RAYS_DESC size mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, RayGenerationShaderRecord) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, RayGenerationShaderRecord),
              "RayGen offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, MissShaderTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, MissShaderTable),
              "Miss offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, HitGroupTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, HitGroupTable),
              "HitGroup offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, CallableShaderTable) ==
                  offsetof(D3D12_DISPATCH_RAYS_DESC, CallableShaderTable),
              "Callable offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Width) == offsetof(D3D12_DISPATCH_RAYS_DESC, Width),
              "Width offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Height) == offsetof(D3D12_DISPATCH_RAYS_DESC, Height),
              "Height offset mismatch");
static_assert(offsetof(DxrDispatchRaysDesc, Depth) == offsetof(D3D12_DISPATCH_RAYS_DESC, Depth),
              "Depth offset mismatch");

template <typename T>
void SafeRelease(T*& ptr) noexcept {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || wide[0] == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool IsSoftwareOrWarpAdapter(const DXGI_ADAPTER_DESC1& desc) noexcept {
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return true;
    // Microsoft Basic Render Driver / WARP (VID 0x1414, DID 0x008C) can enumerate
    // without DXGI_ADAPTER_FLAG_SOFTWARE on headless/Hyper-V Windows Server hosts.
    if (desc.VendorId == 0x1414u && (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0)) {
        return true;
    }
    return false;
}

void PopulateAdapterCaps(IDXGIAdapter1* adapter, DxrCapabilities& caps) {
    if (!adapter) {
        return;
    }
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        caps.adapter_description = WideToUtf8(desc.Description);
        caps.adapter_is_warp = IsSoftwareOrWarpAdapter(desc);
        caps.vendor_id = desc.VendorId;
        caps.device_id = desc.DeviceId;
        caps.dedicated_video_memory = static_cast<uint64_t>(desc.DedicatedVideoMemory);
    }
}

Status CreateBufferResource(ID3D12Device* device,
                            D3D12_HEAP_TYPE heap_type,
                            uint64_t byte_size,
                            D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES initial_state,
                            ID3D12Resource** out_res) {
    if (!device || !out_res || byte_size == 0) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid buffer creation arguments");
    }
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = heap_type;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = byte_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    const HRESULT hr = device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr, IID_PPV_ARGS(out_res));
    if (FAILED(hr) || !*out_res) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource(buffer) failed");
    }
    return Status::Ok();
}

#endif  // _WIN32

}  // namespace

struct DxrHardwareEngine::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    uint64_t next_as_id = 0xD4000001ull;
    uint64_t next_synthetic_va = 0x0000000100000000ull;
    uint64_t last_frame_hash = 0;
    uint64_t total_dispatches = 0;

    struct BlasEntry {
        uint64_t id = 0;
        std::vector<RtTriangle> triangles;
        BvhAccelerationStructure bvh;
        uint32_t build_flags = 0;
        uint64_t gpu_virtual_address = 0;
        uint64_t result_size_bytes = 0;
        uint64_t scratch_size_bytes = 0;
        uint64_t update_scratch_size_bytes = 0;
        bool hardware_built = false;
#if defined(_WIN32)
        ID3D12Resource* as_buffer = nullptr;
        ~BlasEntry() { SafeRelease(as_buffer); }
        BlasEntry() = default;
        BlasEntry(const BlasEntry&) = delete;
        BlasEntry& operator=(const BlasEntry&) = delete;
        BlasEntry(BlasEntry&& other) noexcept
            : id(other.id),
              triangles(std::move(other.triangles)),
              bvh(std::move(other.bvh)),
              build_flags(other.build_flags),
              gpu_virtual_address(other.gpu_virtual_address),
              result_size_bytes(other.result_size_bytes),
              scratch_size_bytes(other.scratch_size_bytes),
              update_scratch_size_bytes(other.update_scratch_size_bytes),
              hardware_built(other.hardware_built),
              as_buffer(other.as_buffer) {
            other.as_buffer = nullptr;
        }
        BlasEntry& operator=(BlasEntry&& other) noexcept {
            if (this != &other) {
                SafeRelease(as_buffer);
                id = other.id;
                triangles = std::move(other.triangles);
                bvh = std::move(other.bvh);
                build_flags = other.build_flags;
                gpu_virtual_address = other.gpu_virtual_address;
                result_size_bytes = other.result_size_bytes;
                scratch_size_bytes = other.scratch_size_bytes;
                update_scratch_size_bytes = other.update_scratch_size_bytes;
                hardware_built = other.hardware_built;
                as_buffer = other.as_buffer;
                other.as_buffer = nullptr;
            }
            return *this;
        }
#endif
    };

    struct TlasEntry {
        uint64_t id = 0;
        std::vector<DxrInstanceDesc> instances;
        BvhAccelerationStructure bvh;
        uint32_t build_flags = 0;
        uint64_t gpu_virtual_address = 0;
        uint64_t result_size_bytes = 0;
        uint64_t scratch_size_bytes = 0;
        bool hardware_built = false;
#if defined(_WIN32)
        ID3D12Resource* as_buffer = nullptr;
        ~TlasEntry() { SafeRelease(as_buffer); }
        TlasEntry() = default;
        TlasEntry(const TlasEntry&) = delete;
        TlasEntry& operator=(const TlasEntry&) = delete;
        TlasEntry(TlasEntry&& other) noexcept
            : id(other.id),
              instances(std::move(other.instances)),
              bvh(std::move(other.bvh)),
              build_flags(other.build_flags),
              gpu_virtual_address(other.gpu_virtual_address),
              result_size_bytes(other.result_size_bytes),
              scratch_size_bytes(other.scratch_size_bytes),
              hardware_built(other.hardware_built),
              as_buffer(other.as_buffer) {
            other.as_buffer = nullptr;
        }
        TlasEntry& operator=(TlasEntry&& other) noexcept {
            if (this != &other) {
                SafeRelease(as_buffer);
                id = other.id;
                instances = std::move(other.instances);
                bvh = std::move(other.bvh);
                build_flags = other.build_flags;
                gpu_virtual_address = other.gpu_virtual_address;
                result_size_bytes = other.result_size_bytes;
                scratch_size_bytes = other.scratch_size_bytes;
                hardware_built = other.hardware_built;
                as_buffer = other.as_buffer;
                other.as_buffer = nullptr;
            }
            return *this;
        }
#endif
    };

    std::unordered_map<uint64_t, BlasEntry> blas_map;
    std::unordered_map<uint64_t, TlasEntry> tlas_map;

#if defined(_WIN32)
    ID3D12Device* device = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList4* cmd_list4 = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;
    std::vector<ID3D12Resource*> owned_sbt_buffers;

    Status WaitForGpu() {
        if (!queue || !fence || !fence_event) {
            return Status::Error(StatusCode::DeviceError, "DXR command queue/fence unavailable");
        }
        const uint64_t target = ++fence_value;
        HRESULT hr = queue->Signal(fence, target);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "DXR fence Signal failed");
        }
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, fence_event);
            if (FAILED(hr)) {
                return Status::Error(StatusCode::DeviceError, "DXR fence SetEventOnCompletion failed");
            }
            const DWORD wait_res = WaitForSingleObject(fence_event, 15000);
            if (wait_res != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "DXR GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    void ProbeAndConfigureDevice(DxrCapabilities& caps, bool force_software_path) {
        if (!device) {
            return;
        }
        device->QueryInterface(IID_PPV_ARGS(&device5));
        caps.supports_device5 = (device5 != nullptr);

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        ID3D12Device* query_dev = device5 ? static_cast<ID3D12Device*>(device5) : device;
        if (SUCCEEDED(query_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
            caps.hardware_tier = static_cast<uint32_t>(opts5.RaytracingTier);
        }

        caps.supports_state_objects =
            caps.supports_device5 && caps.hardware_tier >= static_cast<uint32_t>(D3D12_RAYTRACING_TIER_1_0);
        caps.supports_inline_rt =
            caps.supports_device5 && caps.hardware_tier >= static_cast<uint32_t>(D3D12_RAYTRACING_TIER_1_1);

        const bool want_hw_dxr =
            !force_software_path && !caps.adapter_is_warp && caps.supports_state_objects && device5 != nullptr;

        if (want_hw_dxr) {
            D3D12_COMMAND_QUEUE_DESC qdesc{};
            qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            qdesc.NodeMask = 0;

            HRESULT hr = device5->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
            if (SUCCEEDED(hr) && queue) {
                hr = device5->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            }
            if (SUCCEEDED(hr) && allocator) {
                hr = device5->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&cmd_list4));
                if (SUCCEEDED(hr) && cmd_list4) {
                    cmd_list4->Close();
                }
            }
            if (SUCCEEDED(hr) && cmd_list4) {
                hr = device5->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            }
            if (SUCCEEDED(hr) && fence) {
                fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            }

            if (SUCCEEDED(hr) && queue && allocator && cmd_list4 && fence && fence_event) {
                caps.using_hardware_path = true;
            } else {
                if (fence_event) {
                    CloseHandle(fence_event);
                    fence_event = nullptr;
                }
                SafeRelease(fence);
                SafeRelease(cmd_list4);
                SafeRelease(allocator);
                SafeRelease(queue);
                caps.using_hardware_path = false;
            }
        }
    }
#endif

    void Cleanup() {
        blas_map.clear();
        tlas_map.clear();
#if defined(_WIN32)
        for (auto* res : owned_sbt_buffers) {
            if (res) res->Release();
        }
        owned_sbt_buffers.clear();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        SafeRelease(fence);
        SafeRelease(cmd_list4);
        SafeRelease(allocator);
        SafeRelease(queue);
        SafeRelease(device5);
        SafeRelease(device);
        fence_value = 0;
#endif
        initialized = false;
    }

    ~Impl() { Cleanup(); }
};

DxrHardwareEngine::DxrHardwareEngine() : impl_(std::make_unique<Impl>()) {}
DxrHardwareEngine::~DxrHardwareEngine() = default;

bool DxrHardwareEngine::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

uint64_t DxrHardwareEngine::LastFrameHash() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_frame_hash;
}

uint64_t DxrHardwareEngine::TotalDispatches() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_dispatches;
}

Status DxrHardwareEngine::Initialize(const DxrInitOptions& options) {
    Shutdown();

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        caps_ = DxrCapabilities{};

#if defined(_WIN32)
        if (options.enable_debug_layer) {
            ID3D12Debug* debug_ctrl = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl))) && debug_ctrl) {
                debug_ctrl->EnableDebugLayer();
                debug_ctrl->Release();
            }
        }

        IDXGIFactory6* factory = nullptr;
        const UINT factory_flags = options.enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0u;
        HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        }
        if (FAILED(hr) || !factory) {
            return RecordDxrResult("Initialize",
                                   Status::Error(StatusCode::DeviceError, "CreateDXGIFactory2(IDXGIFactory6) failed"),
                                   static_cast<int32_t>(hr));
        }

        IDXGIAdapter1* selected_adapter = nullptr;
        ID3D12Device* created_device = nullptr;

        if (!options.prefer_warp) {
            for (UINT idx = 0;; ++idx) {
                IDXGIAdapter1* candidate = nullptr;
                if (factory->EnumAdapterByGpuPreference(
                        idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) ==
                    DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (!candidate) {
                    continue;
                }
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                    !IsSoftwareOrWarpAdapter(desc)) {
                    HRESULT dev_hr = D3D12CreateDevice(
                        candidate, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created_device));
                    if (FAILED(dev_hr)) {
                        dev_hr = D3D12CreateDevice(
                            candidate, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&created_device));
                    }
                    if (SUCCEEDED(dev_hr) && created_device) {
                        selected_adapter = candidate;
                        break;
                    }
                }
                candidate->Release();
            }
        }

        // Explicit WARP request or fallback when no hardware D3D12 adapter is available.
        if (!created_device) {
            IDXGIAdapter1* warp_adapter = nullptr;
            hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter));
            if (SUCCEEDED(hr) && warp_adapter) {
                HRESULT dev_hr = D3D12CreateDevice(
                    warp_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created_device));
                if (FAILED(dev_hr)) {
                    dev_hr = D3D12CreateDevice(
                        warp_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&created_device));
                }
                if (SUCCEEDED(dev_hr) && created_device) {
                    selected_adapter = warp_adapter;
                } else {
                    warp_adapter->Release();
                }
            }
        }

        if (selected_adapter) {
            PopulateAdapterCaps(selected_adapter, caps_);
            selected_adapter->Release();
        }
        factory->Release();

        if (!created_device) {
            return RecordDxrResult("Initialize",
                                   Status::Error(StatusCode::DeviceError, "Failed to create D3D12 device for DXR"),
                                   static_cast<int32_t>(hr));
        }

        impl_->device = created_device;
        impl_->ProbeAndConfigureDevice(caps_, options.force_software_path);

        std::cerr << "[OpenScarlett][dxr] Initialized on \""
                  << (caps_.adapter_description.empty() ? "unknown" : caps_.adapter_description)
                  << "\" tier=" << caps_.hardware_tier
                  << " device5=" << (caps_.supports_device5 ? "yes" : "no")
                  << " state_objects=" << (caps_.supports_state_objects ? "yes" : "no")
                  << " inline_rt=" << (caps_.supports_inline_rt ? "yes" : "no")
                  << " path=" << (caps_.using_hardware_path ? "hardware-DXR" : "software-BVH") << "\n";
#else
        (void)options;
        caps_.using_hardware_path = false;
#endif

        impl_->initialized = true;
    }

    return RecordDxrResult("Initialize", Status::Ok());
}

Status DxrHardwareEngine::Initialize(ID3D12Device* device, bool force_software_path) {
    Shutdown();

#if defined(_WIN32)
    if (!device) {
        return RecordDxrResult("Initialize",
                               Status::Error(StatusCode::InvalidArgument, "ID3D12Device pointer must not be null"),
                               static_cast<int32_t>(E_INVALIDARG));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        caps_ = DxrCapabilities{};

        impl_->device = device;
        impl_->device->AddRef();

        // Automatically query DXGI adapter metadata via the device's LUID.
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && factory) {
            const LUID luid = impl_->device->GetAdapterLuid();
            IDXGIAdapter1* adapter = nullptr;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter) {
                PopulateAdapterCaps(adapter, caps_);
                adapter->Release();
            }
            factory->Release();
        }

        impl_->ProbeAndConfigureDevice(caps_, force_software_path);

        std::cerr << "[OpenScarlett][dxr] Attached to \""
                  << (caps_.adapter_description.empty() ? "unknown" : caps_.adapter_description)
                  << "\" tier=" << caps_.hardware_tier
                  << " device5=" << (caps_.supports_device5 ? "yes" : "no")
                  << " state_objects=" << (caps_.supports_state_objects ? "yes" : "no")
                  << " inline_rt=" << (caps_.supports_inline_rt ? "yes" : "no")
                  << " path=" << (caps_.using_hardware_path ? "hardware-DXR" : "software-BVH") << "\n";

        impl_->initialized = true;
    }

    return RecordDxrResult("Initialize", Status::Ok());
#else
    (void)device;
    (void)force_software_path;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        caps_ = DxrCapabilities{};
        impl_->initialized = true;
    }
    return RecordDxrResult("Initialize", Status::Ok());
#endif
}

void DxrHardwareEngine::Shutdown() {
    if (!impl_) return;
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        was_initialized = impl_->initialized;
        impl_->Cleanup();
        caps_ = DxrCapabilities{};
    }
    if (was_initialized) {
        CallTelemetry::Global().Record("dxr_hardware", "Shutdown", CallOutcome::Ok, 0);
    }
}

Status DxrHardwareEngine::QueryPrebuildInfoLocked(const DxrAsInputs& inputs,
                                                  DxrPrebuildInfo& out_info) const {
    if (inputs.type == DxrAsType::BottomLevel) {
        if (inputs.primitive_count == 0 || inputs.geometry_count == 0) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "BLAS prebuild requires primitive_count > 0 and geometry_count > 0");
        }
        if (inputs.vertex_stride < 12) {
            return Status::Error(StatusCode::InvalidArgument,
                                 "BLAS vertex_stride must be >= 12 bytes (float3)");
        }
    } else if (inputs.primitive_count == 0) {
        return Status::Error(StatusCode::InvalidArgument, "TLAS prebuild requires primitive_count > 0");
    }

    out_info = DxrPrebuildInfo{};

#if defined(_WIN32)
    if (caps_.using_hardware_path && impl_->device5) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS d3d_inputs{};
        d3d_inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            inputs.build_flags & ~kDxrBuildPerformUpdate);
        d3d_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
        if (inputs.type == DxrAsType::BottomLevel) {
            const uint32_t gcount = std::min(inputs.geometry_count, inputs.primitive_count);
            geoms.resize(gcount);
            const uint32_t base_tris = inputs.primitive_count / gcount;
            const uint32_t rem_tris = inputs.primitive_count % gcount;
            for (uint32_t i = 0; i < gcount; ++i) {
                const uint32_t tris = base_tris + (i < rem_tris ? 1u : 0u);
                auto& g = geoms[i];
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = inputs.is_opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                                           : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                g.Triangles.Transform3x4 = 0;
                g.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.IndexCount = 0;
                g.Triangles.VertexCount = tris * 3u;
                g.Triangles.IndexBuffer = 0;
                g.Triangles.VertexBuffer.StartAddress = 0;
                g.Triangles.VertexBuffer.StrideInBytes = inputs.vertex_stride;
            }
            d3d_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            d3d_inputs.NumDescs = gcount;
            d3d_inputs.pGeometryDescs = geoms.data();
        } else {
            d3d_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            d3d_inputs.NumDescs = inputs.primitive_count;
            d3d_inputs.InstanceDescs = 0;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO d3d_prebuild{};
        impl_->device5->GetRaytracingAccelerationStructurePrebuildInfo(&d3d_inputs, &d3d_prebuild);

        if (d3d_prebuild.ResultDataMaxSizeInBytes > 0) {
            out_info.result_data_max_size_bytes =
                AlignUp(d3d_prebuild.ResultDataMaxSizeInBytes, kDxrAsByteAlignment);
            out_info.scratch_data_size_bytes =
                AlignUp(std::max<uint64_t>(kDxrAsByteAlignment, d3d_prebuild.ScratchDataSizeInBytes),
                        kDxrAsByteAlignment);
            out_info.update_scratch_data_size_bytes =
                d3d_prebuild.UpdateScratchDataSizeInBytes > 0
                    ? AlignUp(d3d_prebuild.UpdateScratchDataSizeInBytes, kDxrAsByteAlignment)
                    : 0ull;
            out_info.from_hardware_driver = true;
            return Status::Ok();
        }
    }
#endif

    out_info = ComputeAnalyticPrebuildInfo(inputs);
    return Status::Ok();
}

Status DxrHardwareEngine::QueryPrebuildInfo(const DxrAsInputs& inputs, DxrPrebuildInfo& out_info) const {
    if (!IsInitialized()) {
        return RecordDxrResult("QueryPrebuildInfo",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    Status st;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        st = QueryPrebuildInfoLocked(inputs, out_info);
    }
    return RecordDxrResult("QueryPrebuildInfo", st);
}

Status DxrHardwareEngine::BuildBlas(const std::vector<RtTriangle>& triangles,
                                    uint32_t build_flags,
                                    uint64_t& out_blas_id) {
    if (!IsInitialized()) {
        return RecordDxrResult("BuildBlas",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    if (triangles.empty()) {
        return RecordDxrResult("BuildBlas",
                               Status::Error(StatusCode::RaytracingError, "Cannot build BLAS from 0 triangles"));
    }

    DxrAsInputs prebuild_inputs{};
    prebuild_inputs.type = DxrAsType::BottomLevel;
    prebuild_inputs.build_flags = build_flags & ~kDxrBuildPerformUpdate;
    prebuild_inputs.primitive_count = static_cast<uint32_t>(triangles.size());
    prebuild_inputs.geometry_count = 1;
    prebuild_inputs.vertex_stride = 12;
    prebuild_inputs.is_opaque = true;

    Impl::BlasEntry entry{};
    entry.build_flags = prebuild_inputs.build_flags;
    entry.triangles = triangles;

    // Build deterministic SAH software BVH mirror for traversal / fallback.
    Status bvh_st = entry.bvh.Build(entry.triangles);
    if (!bvh_st.ok()) {
        return RecordDxrResult("BuildBlas", bvh_st);
    }

    Status final_st = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        DxrPrebuildInfo prebuild{};
        final_st = QueryPrebuildInfoLocked(prebuild_inputs, prebuild);
        if (final_st.ok()) {
            entry.result_size_bytes = prebuild.result_data_max_size_bytes;
            entry.scratch_size_bytes = prebuild.scratch_data_size_bytes;
            entry.update_scratch_size_bytes = prebuild.update_scratch_data_size_bytes;

#if defined(_WIN32)
            D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
            geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geom_desc.Triangles.Transform3x4 = 0;
            geom_desc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
            geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geom_desc.Triangles.IndexCount = 0;
            geom_desc.Triangles.VertexCount = static_cast<UINT>(triangles.size() * 3u);
            geom_desc.Triangles.IndexBuffer = 0;
            geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3u;

            if (caps_.using_hardware_path && impl_->device5 && impl_->cmd_list4) {
                const uint64_t vertex_bytes =
                    static_cast<uint64_t>(triangles.size()) * 3ull * 3ull * sizeof(float);
                ID3D12Resource* vertex_upload = nullptr;
                ID3D12Resource* scratch_buf = nullptr;
                ID3D12Resource* result_buf = nullptr;

                auto cleanup_temps = [&]() {
                    SafeRelease(scratch_buf);
                    SafeRelease(vertex_upload);
                };

                final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, vertex_bytes,
                                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                &vertex_upload);
                if (final_st.ok()) {
                    void* mapped = nullptr;
                    D3D12_RANGE read_none{0, 0};
                    const HRESULT map_hr = vertex_upload->Map(0, &read_none, &mapped);
                    if (FAILED(map_hr) || !mapped) {
                        final_st = Status::Error(StatusCode::DeviceError, "BLAS vertex upload Map failed");
                    } else {
                        auto* dst_floats = static_cast<float*>(mapped);
                        for (const auto& tri : triangles) {
                            *dst_floats++ = tri.v0.x; *dst_floats++ = tri.v0.y; *dst_floats++ = tri.v0.z;
                            *dst_floats++ = tri.v1.x; *dst_floats++ = tri.v1.y; *dst_floats++ = tri.v1.z;
                            *dst_floats++ = tri.v2.x; *dst_floats++ = tri.v2.y; *dst_floats++ = tri.v2.z;
                        }
                        vertex_upload->Unmap(0, nullptr);
                    }
                }

                if (final_st.ok()) {
                    final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT,
                                                    prebuild.scratch_data_size_bytes,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
                }

                if (final_st.ok()) {
                    final_st = CreateBufferResource(
                        impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.result_data_max_size_bytes,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &result_buf);
                }

                if (final_st.ok()) {
                    geom_desc.Triangles.VertexBuffer.StartAddress = vertex_upload->GetGPUVirtualAddress();

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
                    build_desc.DestAccelerationStructureData = result_buf->GetGPUVirtualAddress();
                    build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
                    build_desc.SourceAccelerationStructureData = 0;
                    build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                    build_desc.Inputs.Flags =
                        static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(entry.build_flags);
                    build_desc.Inputs.NumDescs = 1;
                    build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    build_desc.Inputs.pGeometryDescs = &geom_desc;

                    if ((build_desc.DestAccelerationStructureData % kDxrAsByteAlignment) != 0 ||
                        (build_desc.ScratchAccelerationStructureData % kDxrAsByteAlignment) != 0) {
                        final_st = Status::Error(StatusCode::RaytracingError,
                                                 "DXR BLAS buffer address is not 256-byte aligned");
                    } else {
                        HRESULT hr = impl_->allocator->Reset();
                        if (SUCCEEDED(hr)) {
                            hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
                        }
                        if (FAILED(hr)) {
                            final_st = Status::Error(StatusCode::DeviceError,
                                                     "DXR BLAS command list Reset failed");
                        } else {
                            impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

                            D3D12_RESOURCE_BARRIER uav_barrier{};
                            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            uav_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                            uav_barrier.UAV.pResource = result_buf;
                            impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

                            hr = impl_->cmd_list4->Close();
                            if (FAILED(hr)) {
                                final_st = Status::Error(StatusCode::DeviceError,
                                                         "DXR BLAS command list Close failed");
                            } else {
                                ID3D12CommandList* lists[] = {impl_->cmd_list4};
                                impl_->queue->ExecuteCommandLists(1, lists);
                                final_st = impl_->WaitForGpu();
                            }
                        }
                    }
                }

                cleanup_temps();
                if (final_st.ok()) {
                    entry.as_buffer = result_buf;
                    entry.gpu_virtual_address = result_buf->GetGPUVirtualAddress();
                    entry.hardware_built = true;
                } else {
                    SafeRelease(result_buf);
                }
            } else {
                (void)geom_desc;
                entry.gpu_virtual_address = impl_->next_synthetic_va;
                impl_->next_synthetic_va = AlignUp(
                    impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                    kDxrAsByteAlignment);
                entry.hardware_built = false;
            }
#else
            entry.gpu_virtual_address = impl_->next_synthetic_va;
            impl_->next_synthetic_va = AlignUp(
                impl_->next_synthetic_va + std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                kDxrAsByteAlignment);
            entry.hardware_built = false;
#endif

            if (final_st.ok()) {
                const uint64_t id = impl_->next_as_id++;
                entry.id = id;
                impl_->blas_map.emplace(id, std::move(entry));
                out_blas_id = id;
            }
        }
    }

    return RecordDxrResult("BuildBlas", final_st);
}

Status DxrHardwareEngine::UpdateBlas(uint64_t blas_id, const std::vector<RtTriangle>& triangles) {
    if (!IsInitialized()) {
        return RecordDxrResult("UpdateBlas",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    if (triangles.empty()) {
        return RecordDxrResult("UpdateBlas",
                               Status::Error(StatusCode::RaytracingError, "Cannot update BLAS with 0 triangles"));
    }

    BvhAccelerationStructure rebuilt_bvh;
    Status bvh_st = rebuilt_bvh.Build(triangles);
    if (!bvh_st.ok()) {
        return RecordDxrResult("UpdateBlas", bvh_st);
    }

    Status final_st = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->blas_map.find(blas_id);
        if (it == impl_->blas_map.end()) {
            final_st = Status::Error(StatusCode::NotFound, "BLAS handle not found");
        } else {
            auto& entry = it->second;
            if ((entry.build_flags & kDxrBuildAllowUpdate) == 0) {
                final_st = Status::Error(StatusCode::InvalidArgument,
                                         "BLAS was not created with kDxrBuildAllowUpdate");
            } else if (entry.triangles.size() != triangles.size()) {
                final_st = Status::Error(StatusCode::InvalidArgument,
                                         "BLAS in-place update requires identical triangle count");
            } else {
#if defined(_WIN32)
                if (entry.hardware_built && entry.as_buffer && impl_->device5 && impl_->cmd_list4) {
                    const uint64_t vertex_bytes =
                        static_cast<uint64_t>(triangles.size()) * 3ull * 3ull * sizeof(float);
                    const uint64_t scratch_bytes = std::max<uint64_t>(
                        kDxrAsByteAlignment,
                        entry.update_scratch_size_bytes > 0 ? entry.update_scratch_size_bytes
                                                            : entry.scratch_size_bytes);
                    ID3D12Resource* vertex_upload = nullptr;
                    ID3D12Resource* scratch_buf = nullptr;

                    auto cleanup = [&]() {
                        SafeRelease(scratch_buf);
                        SafeRelease(vertex_upload);
                    };

                    final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, vertex_bytes,
                                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    &vertex_upload);
                    if (final_st.ok()) {
                        void* mapped = nullptr;
                        D3D12_RANGE read_none{0, 0};
                        if (FAILED(vertex_upload->Map(0, &read_none, &mapped)) || !mapped) {
                            final_st = Status::Error(StatusCode::DeviceError, "BLAS update vertex Map failed");
                        } else {
                            auto* dst_floats = static_cast<float*>(mapped);
                            for (const auto& tri : triangles) {
                                *dst_floats++ = tri.v0.x; *dst_floats++ = tri.v0.y; *dst_floats++ = tri.v0.z;
                                *dst_floats++ = tri.v1.x; *dst_floats++ = tri.v1.y; *dst_floats++ = tri.v1.z;
                                *dst_floats++ = tri.v2.x; *dst_floats++ = tri.v2.y; *dst_floats++ = tri.v2.z;
                            }
                            vertex_upload->Unmap(0, nullptr);
                        }
                    }

                    if (final_st.ok()) {
                        final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT, scratch_bytes,
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
                    }

                    if (final_st.ok()) {
                        D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
                        geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                        geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                        geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                        geom_desc.Triangles.VertexCount = static_cast<UINT>(triangles.size() * 3u);
                        geom_desc.Triangles.VertexBuffer.StartAddress = vertex_upload->GetGPUVirtualAddress();
                        geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3u;

                        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
                        build_desc.DestAccelerationStructureData = entry.as_buffer->GetGPUVirtualAddress();
                        build_desc.SourceAccelerationStructureData = entry.as_buffer->GetGPUVirtualAddress();
                        build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
                        build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                        build_desc.Inputs.Flags =
                            static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
                                entry.build_flags | kDxrBuildPerformUpdate);
                        build_desc.Inputs.NumDescs = 1;
                        build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                        build_desc.Inputs.pGeometryDescs = &geom_desc;

                        HRESULT hr = impl_->allocator->Reset();
                        if (SUCCEEDED(hr)) {
                            hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
                        }
                        if (FAILED(hr)) {
                            final_st = Status::Error(StatusCode::DeviceError,
                                                     "DXR BLAS update command list Reset failed");
                        } else {
                            impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);
                            D3D12_RESOURCE_BARRIER uav_barrier{};
                            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            uav_barrier.UAV.pResource = entry.as_buffer;
                            impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

                            if (FAILED(impl_->cmd_list4->Close())) {
                                final_st = Status::Error(StatusCode::DeviceError,
                                                         "DXR BLAS update command list Close failed");
                            } else {
                                ID3D12CommandList* lists[] = {impl_->cmd_list4};
                                impl_->queue->ExecuteCommandLists(1, lists);
                                final_st = impl_->WaitForGpu();
                            }
                        }
                    }

                    cleanup();
                }
#endif
                if (final_st.ok()) {
                    entry.triangles = triangles;
                    entry.bvh = std::move(rebuilt_bvh);
                }
            }
        }
    }

    return RecordDxrResult("UpdateBlas", final_st);
}

Status DxrHardwareEngine::BuildTlas(const std::vector<DxrInstanceDesc>& instances,
                                    uint32_t build_flags,
                                    uint64_t& out_tlas_id) {
    if (!IsInitialized()) {
        return RecordDxrResult("BuildTlas",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    if (instances.empty()) {
        return RecordDxrResult("BuildTlas",
                               Status::Error(StatusCode::RaytracingError, "Cannot build TLAS from 0 instances"));
    }

    DxrAsInputs prebuild_inputs{};
    prebuild_inputs.type = DxrAsType::TopLevel;
    prebuild_inputs.build_flags = build_flags & ~kDxrBuildPerformUpdate;
    prebuild_inputs.primitive_count = static_cast<uint32_t>(instances.size());

    Status final_st = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        DxrPrebuildInfo prebuild{};
        final_st = QueryPrebuildInfoLocked(prebuild_inputs, prebuild);

        std::vector<RtTriangle> world_triangles;
        bool all_blas_hw = true;
#if defined(_WIN32)
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> d3d_instances;
        d3d_instances.reserve(instances.size());
#endif

        if (final_st.ok()) {
            for (const auto& inst : instances) {
                auto bit = impl_->blas_map.find(inst.blas_id);
                if (bit == impl_->blas_map.end()) {
                    final_st = Status::Error(StatusCode::NotFound, "TLAS references unknown BLAS handle");
                    break;
                }
                const auto& blas = bit->second;
                if (!blas.hardware_built) {
                    all_blas_hw = false;
                }
                if ((blas.gpu_virtual_address % kDxrAsByteAlignment) != 0) {
                    final_st = Status::Error(StatusCode::RaytracingError,
                                             "Referenced BLAS GPU VA is not 256-byte aligned");
                    break;
                }

                if (inst.instance_mask != 0) {
                    for (const auto& src_tri : blas.triangles) {
                        world_triangles.push_back(TransformTriangle3x4(inst.transform, src_tri));
                    }
                }

#if defined(_WIN32)
                D3D12_RAYTRACING_INSTANCE_DESC d{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        d.Transform[r][c] = inst.transform[r][c];
                    }
                }
                d.InstanceID = inst.instance_id & 0x00FFFFFFu;
                d.InstanceMask = inst.instance_mask & 0xFFu;
                d.InstanceContributionToHitGroupIndex =
                    inst.instance_contribution_to_hit_group_index & 0x00FFFFFFu;
                d.Flags = inst.flags & 0xFFu;
                d.AccelerationStructure = blas.gpu_virtual_address;
                d3d_instances.push_back(d);
#endif
            }
        }

        if (final_st.ok() && world_triangles.empty()) {
            final_st = Status::Error(StatusCode::RaytracingError, "TLAS contains no visible triangles");
        }

        if (final_st.ok()) {
            Impl::TlasEntry entry{};
            entry.instances = instances;
            entry.build_flags = prebuild_inputs.build_flags;
            entry.result_size_bytes = prebuild.result_data_max_size_bytes;
            entry.scratch_size_bytes = prebuild.scratch_data_size_bytes;

            final_st = entry.bvh.Build(world_triangles);

            if (final_st.ok()) {
#if defined(_WIN32)
                if (caps_.using_hardware_path && all_blas_hw && impl_->device5 && impl_->cmd_list4) {
                    const uint64_t inst_bytes =
                        static_cast<uint64_t>(d3d_instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
                    ID3D12Resource* inst_upload = nullptr;
                    ID3D12Resource* scratch_buf = nullptr;
                    ID3D12Resource* result_buf = nullptr;

                    auto cleanup_temps = [&]() {
                        SafeRelease(scratch_buf);
                        SafeRelease(inst_upload);
                    };

                    final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_UPLOAD, inst_bytes,
                                                    D3D12_RESOURCE_FLAG_NONE,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, &inst_upload);
                    if (final_st.ok()) {
                        void* mapped = nullptr;
                        D3D12_RANGE read_none{0, 0};
                        if (FAILED(inst_upload->Map(0, &read_none, &mapped)) || !mapped) {
                            final_st = Status::Error(StatusCode::DeviceError,
                                                     "TLAS instance upload Map failed");
                        } else {
                            std::memcpy(mapped, d3d_instances.data(), static_cast<size_t>(inst_bytes));
                            inst_upload->Unmap(0, nullptr);
                        }
                    }

                    if (final_st.ok()) {
                        final_st = CreateBufferResource(impl_->device5, D3D12_HEAP_TYPE_DEFAULT,
                                                        prebuild.scratch_data_size_bytes,
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &scratch_buf);
                    }

                    if (final_st.ok()) {
                        final_st = CreateBufferResource(
                            impl_->device5, D3D12_HEAP_TYPE_DEFAULT, prebuild.result_data_max_size_bytes,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &result_buf);
                    }

                    if (final_st.ok()) {
                        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build_desc{};
                        build_desc.DestAccelerationStructureData = result_buf->GetGPUVirtualAddress();
                        build_desc.ScratchAccelerationStructureData = scratch_buf->GetGPUVirtualAddress();
                        build_desc.SourceAccelerationStructureData = 0;
                        build_desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                        build_desc.Inputs.Flags =
                            static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(entry.build_flags);
                        build_desc.Inputs.NumDescs = static_cast<UINT>(d3d_instances.size());
                        build_desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                        build_desc.Inputs.InstanceDescs = inst_upload->GetGPUVirtualAddress();

                        HRESULT hr = impl_->allocator->Reset();
                        if (SUCCEEDED(hr)) {
                            hr = impl_->cmd_list4->Reset(impl_->allocator, nullptr);
                        }
                        if (FAILED(hr)) {
                            final_st = Status::Error(StatusCode::DeviceError,
                                                     "DXR TLAS command list Reset failed");
                        } else {
                            impl_->cmd_list4->BuildRaytracingAccelerationStructure(&build_desc, 0, nullptr);

                            D3D12_RESOURCE_BARRIER uav_barrier{};
                            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            uav_barrier.UAV.pResource = result_buf;
                            impl_->cmd_list4->ResourceBarrier(1, &uav_barrier);

                            if (FAILED(impl_->cmd_list4->Close())) {
                                final_st = Status::Error(StatusCode::DeviceError,
                                                         "DXR TLAS command list Close failed");
                            } else {
                                ID3D12CommandList* lists[] = {impl_->cmd_list4};
                                impl_->queue->ExecuteCommandLists(1, lists);
                                final_st = impl_->WaitForGpu();
                            }
                        }
                    }

                    cleanup_temps();
                    if (final_st.ok()) {
                        entry.as_buffer = result_buf;
                        entry.gpu_virtual_address = result_buf->GetGPUVirtualAddress();
                        entry.hardware_built = true;
                    } else {
                        SafeRelease(result_buf);
                    }
                } else {
                    entry.gpu_virtual_address = impl_->next_synthetic_va;
                    impl_->next_synthetic_va = AlignUp(
                        impl_->next_synthetic_va +
                            std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                        kDxrAsByteAlignment);
                    entry.hardware_built = false;
                }
#else
                (void)all_blas_hw;
                entry.gpu_virtual_address = impl_->next_synthetic_va;
                impl_->next_synthetic_va = AlignUp(
                    impl_->next_synthetic_va +
                        std::max<uint64_t>(prebuild.result_data_max_size_bytes, 65536ull),
                    kDxrAsByteAlignment);
                entry.hardware_built = false;
#endif
            }

            if (final_st.ok()) {
                const uint64_t id = impl_->next_as_id++;
                entry.id = id;
                impl_->tlas_map.emplace(id, std::move(entry));
                out_tlas_id = id;
            }
        }
    }

    return RecordDxrResult("BuildTlas", final_st);
}

Status DxrHardwareEngine::GetAsInfo(uint64_t as_id, DxrAsRecordInfo& out_info) const {
    if (!IsInitialized()) {
        return RecordDxrResult("GetAsInfo",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }

    Status final_st = Status::Error(StatusCode::NotFound, "Acceleration structure handle not found");
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (auto it = impl_->blas_map.find(as_id); it != impl_->blas_map.end()) {
            const auto& e = it->second;
            out_info = DxrAsRecordInfo{};
            out_info.id = e.id;
            out_info.type = DxrAsType::BottomLevel;
            out_info.gpu_virtual_address = e.gpu_virtual_address;
            out_info.result_size_bytes = e.result_size_bytes;
            out_info.scratch_size_bytes = e.scratch_size_bytes;
            out_info.build_flags = e.build_flags;
            out_info.primitive_count = static_cast<uint32_t>(e.triangles.size());
            out_info.bvh_node_count = e.bvh.NodeCount();
            out_info.hardware_built = e.hardware_built;
            final_st = Status::Ok();
        } else if (auto tit = impl_->tlas_map.find(as_id); tit != impl_->tlas_map.end()) {
            const auto& e = tit->second;
            out_info = DxrAsRecordInfo{};
            out_info.id = e.id;
            out_info.type = DxrAsType::TopLevel;
            out_info.gpu_virtual_address = e.gpu_virtual_address;
            out_info.result_size_bytes = e.result_size_bytes;
            out_info.scratch_size_bytes = e.scratch_size_bytes;
            out_info.build_flags = e.build_flags;
            out_info.primitive_count = static_cast<uint32_t>(e.instances.size());
            out_info.bvh_node_count = e.bvh.NodeCount();
            out_info.hardware_built = e.hardware_built;
            final_st = Status::Ok();
        }
    }

    return RecordDxrResult("GetAsInfo", final_st);
}

Status DxrHardwareEngine::DestroyAs(uint64_t as_id) {
    if (!IsInitialized()) {
        return RecordDxrResult("DestroyAs",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }

    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        erased = (impl_->tlas_map.erase(as_id) > 0) || (impl_->blas_map.erase(as_id) > 0);
    }

    return RecordDxrResult(
        "DestroyAs",
        erased ? Status::Ok()
               : Status::Error(StatusCode::NotFound, "Acceleration structure handle not found"));
}

Status DxrHardwareEngine::BuildShaderBindingTable(const DxrShaderTableDesc& desc,
                                                  DxrShaderBindingTable& out_sbt) {
    if (!IsInitialized()) {
        return RecordDxrResult("BuildShaderBindingTable",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    if (desc.raygen.export_name.empty() && IsZeroIdentifier(desc.raygen.shader_identifier)) {
        return RecordDxrResult("BuildShaderBindingTable",
                               Status::Error(StatusCode::InvalidArgument,
                                             "SBT requires a RayGen export name or non-zero 32-byte shader identifier"));
    }
    if (desc.base_gpu_va != 0 && (desc.base_gpu_va % kDxrShaderTableAlignment) != 0) {
        return RecordDxrResult("BuildShaderBindingTable",
                               Status::Error(StatusCode::InvalidArgument,
                                             "SBT base_gpu_va must be aligned to 64 bytes"));
    }

    auto compute_record_size = [](const DxrShaderRecordDesc& rec) -> uint64_t {
        return AlignUp(static_cast<uint64_t>(kDxrShaderIdentifierSize) + rec.local_root_arguments.size(),
                       kDxrShaderRecordAlignment);
    };
    auto compute_table_stride = [&](const std::vector<DxrShaderRecordDesc>& records) -> uint64_t {
        uint64_t stride = 0;
        for (const auto& rec : records) {
            stride = std::max(stride, compute_record_size(rec));
        }
        return stride;
    };

    const uint64_t raygen_size = compute_record_size(desc.raygen);
    const uint64_t miss_stride = compute_table_stride(desc.miss_records);
    const uint64_t miss_size = miss_stride * desc.miss_records.size();
    const uint64_t hit_stride = compute_table_stride(desc.hit_group_records);
    const uint64_t hit_size = hit_stride * desc.hit_group_records.size();
    const uint64_t callable_stride = compute_table_stride(desc.callable_records);
    const uint64_t callable_size = callable_stride * desc.callable_records.size();

    const uint64_t raygen_offset = 0;
    const uint64_t miss_offset = AlignUp(raygen_offset + raygen_size, kDxrShaderTableAlignment);
    const uint64_t hit_offset = AlignUp(miss_offset + miss_size, kDxrShaderTableAlignment);
    const uint64_t callable_offset = AlignUp(hit_offset + hit_size, kDxrShaderTableAlignment);
    const uint64_t total_size = AlignUp(callable_offset + callable_size, kDxrShaderTableAlignment);

    std::vector<uint8_t> sbt_bytes(static_cast<size_t>(total_size), 0u);

    auto write_record = [&](uint64_t offset, const DxrShaderRecordDesc& rec) {
        const auto id = IsZeroIdentifier(rec.shader_identifier)
                            ? DeriveShaderIdentifier(rec.export_name)
                            : rec.shader_identifier;
        std::memcpy(sbt_bytes.data() + offset, id.data(), kDxrShaderIdentifierSize);
        if (!rec.local_root_arguments.empty()) {
            std::memcpy(sbt_bytes.data() + offset + kDxrShaderIdentifierSize,
                        rec.local_root_arguments.data(),
                        rec.local_root_arguments.size());
        }
    };

    write_record(raygen_offset, desc.raygen);
    for (size_t i = 0; i < desc.miss_records.size(); ++i) {
        write_record(miss_offset + i * miss_stride, desc.miss_records[i]);
    }
    for (size_t i = 0; i < desc.hit_group_records.size(); ++i) {
        write_record(hit_offset + i * hit_stride, desc.hit_group_records[i]);
    }
    for (size_t i = 0; i < desc.callable_records.size(); ++i) {
        write_record(callable_offset + i * callable_stride, desc.callable_records[i]);
    }

    uint64_t resolved_base_va = desc.base_gpu_va;
    bool uploaded_to_gpu = false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
#if defined(_WIN32)
        if (resolved_base_va == 0 && impl_->device != nullptr) {
            ID3D12Resource* sbt_buf = nullptr;
            Status up_st = CreateBufferResource(impl_->device, D3D12_HEAP_TYPE_UPLOAD, total_size,
                                                D3D12_RESOURCE_FLAG_NONE,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, &sbt_buf);
            if (up_st.ok() && sbt_buf) {
                void* mapped = nullptr;
                D3D12_RANGE read_none{0, 0};
                if (SUCCEEDED(sbt_buf->Map(0, &read_none, &mapped)) && mapped) {
                    std::memcpy(mapped, sbt_bytes.data(), sbt_bytes.size());
                    sbt_buf->Unmap(0, nullptr);
                    const uint64_t gpu_va = sbt_buf->GetGPUVirtualAddress();
                    if ((gpu_va % kDxrShaderTableAlignment) == 0) {
                        resolved_base_va = gpu_va;
                        impl_->owned_sbt_buffers.push_back(sbt_buf);
                        uploaded_to_gpu = true;
                    } else {
                        SafeRelease(sbt_buf);
                    }
                } else {
                    SafeRelease(sbt_buf);
                }
            }
        }
#endif
        if (resolved_base_va == 0) {
            resolved_base_va = AlignUp(impl_->next_synthetic_va, kDxrShaderTableAlignment);
            impl_->next_synthetic_va = AlignUp(resolved_base_va + total_size, kDxrAsByteAlignment);
        }
    }

    out_sbt = DxrShaderBindingTable{};
    out_sbt.buffer = std::move(sbt_bytes);
    out_sbt.base_gpu_va = resolved_base_va;
    out_sbt.raygen_offset = raygen_offset;
    out_sbt.miss_offset = miss_offset;
    out_sbt.hit_group_offset = hit_offset;
    out_sbt.callable_offset = callable_offset;
    out_sbt.raygen_record_size = raygen_size;
    out_sbt.miss_record_stride = miss_stride;
    out_sbt.hit_group_record_stride = hit_stride;
    out_sbt.callable_record_stride = callable_stride;
    out_sbt.uploaded_to_gpu = uploaded_to_gpu;

    auto& d = out_sbt.dispatch_rays_desc;
    d.RayGenerationShaderRecord.StartAddress = resolved_base_va + raygen_offset;
    d.RayGenerationShaderRecord.SizeInBytes = raygen_size;

    if (!desc.miss_records.empty()) {
        d.MissShaderTable.StartAddress = resolved_base_va + miss_offset;
        d.MissShaderTable.SizeInBytes = miss_size;
        d.MissShaderTable.StrideInBytes = miss_stride;
    }
    if (!desc.hit_group_records.empty()) {
        d.HitGroupTable.StartAddress = resolved_base_va + hit_offset;
        d.HitGroupTable.SizeInBytes = hit_size;
        d.HitGroupTable.StrideInBytes = hit_stride;
    }
    if (!desc.callable_records.empty()) {
        d.CallableShaderTable.StartAddress = resolved_base_va + callable_offset;
        d.CallableShaderTable.SizeInBytes = callable_size;
        d.CallableShaderTable.StrideInBytes = callable_stride;
    }
    d.Width = std::max<uint32_t>(1u, desc.dispatch_width);
    d.Height = std::max<uint32_t>(1u, desc.dispatch_height);
    d.Depth = std::max<uint32_t>(1u, desc.dispatch_depth);

    return RecordDxrResult("BuildShaderBindingTable", Status::Ok());
}

Status DxrHardwareEngine::DispatchRays(uint64_t tlas_id,
                                       const RtDispatchDesc& desc,
                                       FrameBufferRgba8& out_frame,
                                       RtStats* out_stats,
                                       uint64_t* out_frame_hash) {
    if (!IsInitialized()) {
        return RecordDxrResult("DispatchRays",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0) {
        return RecordDxrResult("DispatchRays",
                               Status::Error(StatusCode::InvalidArgument, "DispatchRays dimensions must be > 0"));
    }

    // Validate caller-provided SBT alignment & D3D12_DISPATCH_RAYS_DESC contract.
    if (desc.sbt) {
        const auto& d = desc.sbt->dispatch_rays_desc;
        if (d.RayGenerationShaderRecord.StartAddress == 0 ||
            d.RayGenerationShaderRecord.SizeInBytes < kDxrShaderIdentifierSize ||
            (d.RayGenerationShaderRecord.StartAddress % kDxrShaderTableAlignment) != 0) {
            return RecordDxrResult("DispatchRays",
                                   Status::Error(StatusCode::RaytracingError,
                                                 "Invalid RayGenerationShaderRecord in DxrShaderBindingTable"));
        }
        auto validate_table = [](const DxrGpuVirtualAddressRangeAndStride& tbl) -> bool {
            if (tbl.SizeInBytes == 0) return true;
            if (tbl.StartAddress == 0) return false;
            if ((tbl.StartAddress % kDxrShaderTableAlignment) != 0) return false;
            if (tbl.StrideInBytes < kDxrShaderIdentifierSize) return false;
            if ((tbl.StrideInBytes % kDxrShaderRecordAlignment) != 0) return false;
            return true;
        };
        if (!validate_table(d.MissShaderTable) ||
            !validate_table(d.HitGroupTable) ||
            !validate_table(d.CallableShaderTable)) {
            return RecordDxrResult("DispatchRays",
                                   Status::Error(StatusCode::RaytracingError,
                                                 "Shader binding table alignment or stride violation"));
        }
    }

    Status final_st = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->tlas_map.find(tlas_id);
        if (it == impl_->tlas_map.end()) {
            final_st = Status::Error(StatusCode::NotFound, "TLAS handle not found for DispatchRays");
        } else {
            const auto& tlas = it->second;
#if defined(_WIN32)
            if (tlas.hardware_built) {
                if (!tlas.as_buffer || tlas.as_buffer->GetGPUVirtualAddress() != tlas.gpu_virtual_address) {
                    final_st = Status::Error(StatusCode::DeviceError, "Hardware TLAS GPU resource mismatch");
                }
            }
#endif
            if (final_st.ok()) {
                RtDispatchParams params{};
                params.width = desc.width;
                params.height = desc.height;
                params.camera_pos = desc.camera_pos;
                params.camera_look_at = desc.camera_look_at;
                params.light_pos = desc.light_pos;
                params.max_bounces = desc.max_bounces;

                RtStats stats{};
                final_st = tlas.bvh.RenderScene(params, out_frame, &stats);
                if (final_st.ok()) {
                    const uint64_t hash = out_frame.ComputeFnv1aHash();
                    impl_->last_frame_hash = hash;
                    ++impl_->total_dispatches;

                    if (out_stats) {
                        *out_stats = stats;
                    }
                    if (out_frame_hash) {
                        *out_frame_hash = hash;
                    }
                }
            }
        }
    }

    return RecordDxrResult("DispatchRays", final_st);
}

// ---------------------------------------------------------------------------
// Automatic DXR Tier 1.1 Hardware vs Compute BVH4 Backend Selection
// ---------------------------------------------------------------------------
Rdna2RayQueryBackend SelectRayQueryBackend(
    const DxrCapabilities& caps,
    bool force_compute_fallback) noexcept {
    if (!force_compute_fallback &&
        caps.using_hardware_path &&
        caps.supports_inline_rt &&
        caps.hardware_tier >= static_cast<uint32_t>(DxrTier::Tier1_1)) {
        return Rdna2RayQueryBackend::HardwareDxr11;
    }
    return Rdna2RayQueryBackend::ComputeBvh4Fallback;
}

Rdna2RayQueryBackend SelectRayQueryBackend(
    const DxrHardwareEngine& engine,
    bool force_compute_fallback) noexcept {
    if (!engine.IsInitialized()) {
        return Rdna2RayQueryBackend::CpuReference;
    }
    return SelectRayQueryBackend(engine.Capabilities(), force_compute_fallback);
}

// ---------------------------------------------------------------------------
// Live CreateStateObject / DispatchRays SBT Record Remapper
// Maps Xbox 32B/64B shader identifiers + local root args -> Host PC DXR SBT
// and software raygen/miss/closest-hit/callable dispatch records.
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> DeriveXboxShaderIdentifier(const std::string& name, uint32_t id_size) {
    std::vector<uint8_t> id(id_size, 0u);
    const uint32_t lanes = id_size / 8u;
    for (uint32_t lane = 0; lane < lanes; ++lane) {
        uint64_t h = 0x58424F5852543131ull ^ (static_cast<uint64_t>(lane) * 0xBF58476D1CE4E5B9ull);
        for (unsigned char c : name) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= (static_cast<uint64_t>(id_size) << 40u) | static_cast<uint64_t>(name.size());
        h *= 1099511628211ull;
        std::memcpy(id.data() + static_cast<size_t>(lane) * 8u, &h, 8u);
    }
    bool all_zero = true;
    for (uint8_t b : id) {
        if (b != 0u) {
            all_zero = false;
            break;
        }
    }
    if (all_zero && id.size() >= 2u) {
        id[0] = 0x58u;  // 'X'
        id[1] = 0x42u;  // 'B'
    }
    return id;
}

std::string MakeByteKey(const uint8_t* data, size_t len) {
    return std::string(reinterpret_cast<const char*>(data), len);
}

}  // namespace

struct DxrStateObjectSbtRemapper::Impl {
    mutable std::mutex mutex;
    uint64_t next_so_id = 0x50120001ull;

    struct ExportEntry {
        std::string export_name;
        DxrStateObjectExportDesc::Kind kind = DxrStateObjectExportDesc::Kind::RayGen;
        std::vector<uint8_t> xbox_id;
        std::array<uint8_t, kDxrShaderIdentifierSize> host_id{};
        uint32_t software_shader_index = 0u;
    };

    struct StateObjectEntry {
        uint64_t id = 0ull;
        uint32_t xbox_identifier_size_bytes = 32u;
        uint32_t max_payload_size_bytes = 32u;
        uint32_t max_attribute_size_bytes = 8u;
        uint32_t max_recursion_depth = 2u;
        std::unordered_map<std::string, ExportEntry> by_name;
        std::unordered_map<std::string, std::string> xbox_id_to_name;
    };

    std::unordered_map<uint64_t, StateObjectEntry> state_objects;
};

DxrStateObjectSbtRemapper::DxrStateObjectSbtRemapper() : impl_(std::make_unique<Impl>()) {}
DxrStateObjectSbtRemapper::~DxrStateObjectSbtRemapper() = default;

Status DxrStateObjectSbtRemapper::CreateStateObject(
    const DxrStateObjectDesc& desc,
    uint64_t& out_state_object_id) {
    out_state_object_id = 0ull;
    if (desc.exports.empty()) {
        return RecordDxrResult("CreateStateObject",
                               Status::Error(StatusCode::InvalidArgument,
                                             "State object requires at least one shader export"));
    }
    if (desc.xbox_identifier_size_bytes != 32u && desc.xbox_identifier_size_bytes != 64u) {
        return RecordDxrResult("CreateStateObject",
                               Status::Error(StatusCode::InvalidArgument,
                                             "xbox_identifier_size_bytes must be 32 or 64"));
    }

    Impl::StateObjectEntry entry{};
    entry.xbox_identifier_size_bytes = desc.xbox_identifier_size_bytes;
    entry.max_payload_size_bytes = desc.max_payload_size_bytes;
    entry.max_attribute_size_bytes = desc.max_attribute_size_bytes;
    entry.max_recursion_depth = desc.max_recursion_depth;

    uint32_t kind_counters[4] = {0u, 0u, 0u, 0u};

    for (const auto& exp : desc.exports) {
        if (exp.export_name.empty()) {
            return RecordDxrResult("CreateStateObject",
                                   Status::Error(StatusCode::InvalidArgument, "Export name must not be empty"));
        }
        if (entry.by_name.find(exp.export_name) != entry.by_name.end()) {
            return RecordDxrResult("CreateStateObject",
                                   Status::Error(StatusCode::InvalidArgument,
                                                 "Duplicate export name in DXR state object: " + exp.export_name));
        }

        std::vector<uint8_t> xbox_id = exp.xbox_shader_identifier;
        if (xbox_id.empty()) {
            xbox_id = DeriveXboxShaderIdentifier(exp.export_name, desc.xbox_identifier_size_bytes);
        } else if (xbox_id.size() != desc.xbox_identifier_size_bytes) {
            return RecordDxrResult("CreateStateObject",
                                   Status::Error(StatusCode::InvalidArgument,
                                                 "Xbox shader identifier size mismatch for export: " + exp.export_name));
        }

        const std::string key = MakeByteKey(xbox_id.data(), xbox_id.size());
        if (entry.xbox_id_to_name.find(key) != entry.xbox_id_to_name.end()) {
            return RecordDxrResult("CreateStateObject",
                                   Status::Error(StatusCode::InvalidArgument,
                                                 "Duplicate Xbox shader identifier in DXR state object"));
        }

        Impl::ExportEntry rec{};
        rec.export_name = exp.export_name;
        rec.kind = exp.kind;
        rec.xbox_id = std::move(xbox_id);
        rec.host_id = DeriveShaderIdentifier(exp.export_name);
        const uint32_t kind_idx = static_cast<uint32_t>(exp.kind) & 3u;
        rec.software_shader_index = kind_counters[kind_idx]++;

        entry.xbox_id_to_name.emplace(key, exp.export_name);
        entry.by_name.emplace(exp.export_name, std::move(rec));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const uint64_t id = impl_->next_so_id++;
        entry.id = id;
        impl_->state_objects.emplace(id, std::move(entry));
        out_state_object_id = id;
    }

    return RecordDxrResult("CreateStateObject", Status::Ok());
}

Status DxrStateObjectSbtRemapper::GetXboxShaderIdentifier(
    uint64_t state_object_id,
    const std::string& export_name,
    std::vector<uint8_t>& out_xbox_id) const {
    out_xbox_id.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->state_objects.find(state_object_id);
    if (it == impl_->state_objects.end()) {
        return Status::Error(StatusCode::NotFound, "State object ID not found");
    }
    auto eit = it->second.by_name.find(export_name);
    if (eit == it->second.by_name.end()) {
        return Status::Error(StatusCode::NotFound, "Export name not found in state object");
    }
    out_xbox_id = eit->second.xbox_id;
    return Status::Ok();
}

Status DxrStateObjectSbtRemapper::GetHostShaderIdentifier(
    uint64_t state_object_id,
    const std::string& export_name,
    std::array<uint8_t, kDxrShaderIdentifierSize>& out_host_id) const {
    out_host_id.fill(0u);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->state_objects.find(state_object_id);
    if (it == impl_->state_objects.end()) {
        return Status::Error(StatusCode::NotFound, "State object ID not found");
    }
    auto eit = it->second.by_name.find(export_name);
    if (eit == it->second.by_name.end()) {
        return Status::Error(StatusCode::NotFound, "Export name not found in state object");
    }
    out_host_id = eit->second.host_id;
    return Status::Ok();
}

Status DxrStateObjectSbtRemapper::RemapDispatchRaysSbt(
    uint64_t state_object_id,
    DxrHardwareEngine& engine,
    const XboxDispatchRaysInput& xbox_input,
    DxrRemappedSbtResult& out_remapped) const {
    out_remapped = DxrRemappedSbtResult{};

    if (!engine.IsInitialized()) {
        return RecordDxrResult("RemapDispatchRaysSbt",
                               Status::Error(StatusCode::DeviceError, "DxrHardwareEngine is not initialized"));
    }

    DxrShaderTableDesc host_table{};
    host_table.base_gpu_va = xbox_input.optional_host_base_va;
    host_table.dispatch_width = std::max<uint32_t>(1u, xbox_input.width);
    host_table.dispatch_height = std::max<uint32_t>(1u, xbox_input.height);
    host_table.dispatch_depth = std::max<uint32_t>(1u, xbox_input.depth);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->state_objects.find(state_object_id);
        if (it == impl_->state_objects.end()) {
            return RecordDxrResult("RemapDispatchRaysSbt",
                                   Status::Error(StatusCode::NotFound, "State object ID not found"));
        }
        const auto& so = it->second;
        const uint32_t id_sz = so.xbox_identifier_size_bytes;
        out_remapped.xbox_identifier_size_bytes = id_sz;

        auto remap_one = [&](const XboxSbtRecordInput& src,
                             DxrStateObjectExportDesc::Kind expected_kind,
                             DxrShaderRecordDesc& dst_host,
                             SoftwareDispatchRecord& dst_sw) -> Status {
            if (src.raw_record_bytes.size() < id_sz) {
                return Status::Error(StatusCode::InvalidArgument,
                                     "Xbox SBT record smaller than shader identifier size");
            }
            const std::string key = MakeByteKey(src.raw_record_bytes.data(), id_sz);
            auto nit = so.xbox_id_to_name.find(key);
            if (nit == so.xbox_id_to_name.end()) {
                return Status::Error(StatusCode::NotFound,
                                     "Unknown Xbox shader identifier in SBT record");
            }
            const auto& exp = so.by_name.at(nit->second);
            if (exp.kind != expected_kind) {
                return Status::Error(StatusCode::InvalidArgument,
                                     "Xbox SBT record shader kind mismatch for export: " + exp.export_name);
            }

            dst_host = DxrShaderRecordDesc{};
            dst_host.export_name = exp.export_name;
            dst_host.shader_identifier = exp.host_id;
            if (src.raw_record_bytes.size() > id_sz) {
                dst_host.local_root_arguments.assign(
                    src.raw_record_bytes.begin() + static_cast<std::ptrdiff_t>(id_sz),
                    src.raw_record_bytes.end());
            }

            dst_sw = SoftwareDispatchRecord{};
            dst_sw.export_name = exp.export_name;
            dst_sw.kind = exp.kind;
            dst_sw.shader_index = exp.software_shader_index;
            dst_sw.local_root_arguments = dst_host.local_root_arguments;
            ++out_remapped.remapped_record_count;
            return Status::Ok();
        };

        Status st = remap_one(xbox_input.raygen_record,
                              DxrStateObjectExportDesc::Kind::RayGen,
                              host_table.raygen,
                              out_remapped.sw_raygen);
        if (!st.ok()) {
            return RecordDxrResult("RemapDispatchRaysSbt", st);
        }

        host_table.miss_records.resize(xbox_input.miss_records.size());
        out_remapped.sw_miss.resize(xbox_input.miss_records.size());
        for (size_t i = 0; i < xbox_input.miss_records.size(); ++i) {
            st = remap_one(xbox_input.miss_records[i],
                           DxrStateObjectExportDesc::Kind::Miss,
                           host_table.miss_records[i],
                           out_remapped.sw_miss[i]);
            if (!st.ok()) {
                return RecordDxrResult("RemapDispatchRaysSbt", st);
            }
        }

        host_table.hit_group_records.resize(xbox_input.hit_group_records.size());
        out_remapped.sw_hit_groups.resize(xbox_input.hit_group_records.size());
        for (size_t i = 0; i < xbox_input.hit_group_records.size(); ++i) {
            st = remap_one(xbox_input.hit_group_records[i],
                           DxrStateObjectExportDesc::Kind::HitGroup,
                           host_table.hit_group_records[i],
                           out_remapped.sw_hit_groups[i]);
            if (!st.ok()) {
                return RecordDxrResult("RemapDispatchRaysSbt", st);
            }
        }

        host_table.callable_records.resize(xbox_input.callable_records.size());
        out_remapped.sw_callables.resize(xbox_input.callable_records.size());
        for (size_t i = 0; i < xbox_input.callable_records.size(); ++i) {
            st = remap_one(xbox_input.callable_records[i],
                           DxrStateObjectExportDesc::Kind::Callable,
                           host_table.callable_records[i],
                           out_remapped.sw_callables[i]);
            if (!st.ok()) {
                return RecordDxrResult("RemapDispatchRaysSbt", st);
            }
        }
    }

    Status sbt_st = engine.BuildShaderBindingTable(host_table, out_remapped.host_sbt);
    return RecordDxrResult("RemapDispatchRaysSbt", sbt_st);
}

Status DxrStateObjectSbtRemapper::DestroyStateObject(uint64_t state_object_id) {
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        erased = (impl_->state_objects.erase(state_object_id) > 0u);
    }
    return RecordDxrResult(
        "DestroyStateObject",
        erased ? Status::Ok() : Status::Error(StatusCode::NotFound, "State object ID not found"));
}

}  // namespace openscarlett
