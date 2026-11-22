#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "openscarlett/gpu/d3d12_xs_abi.h"

#if defined(_WIN32)

// Portable COM IID helper: works identically on MSVC, Clang-CL, and MinGW-w64.
#if defined(_MSC_VER)
#define OS_D3D12_IID(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_D3D12_IID(T) __mingw_uuidof<T>()
#else
#define OS_D3D12_IID(T) IID_##T
#endif

namespace openscarlett::d3d12_xs {

class DeviceX;

enum class D3D12XProxyKind : uint32_t {
    Unknown = 0u,
    Resource = 1u,
    Heap = 2u,
    DescriptorHeap = 3u,
    PipelineState = 4u,
    RootSignature = 5u,
    Fence = 6u,
    CommandAllocator = 7u,
    QueryHeap = 8u,
    CommandSignature = 9u,
    StateObject = 10u,
};

// OpenScarlett COM IIDs for Xbox D3D12X child object proxies & diagnostics.
// {4F534431-3258-5052-5800-000000000010}
inline constexpr GUID IID_IOpenScarlettProxyInspect = {
    0x4F534431u, 0x3258u, 0x5052u, {0x58u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u}};

// {4F534431-3258-434C-4500-000000000011}
inline constexpr GUID IID_IOpenScarlettCommandListExtState = {
    0x4F534431u, 0x3258u, 0x434Cu, {0x45u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x11u}};

// {4F534431-3258-5245-5300-000000000021} .. 0x2A (10 Xbox D3D12X proxy object IIDs)
inline constexpr GUID IID_ID3D12ResourceX = {
    0x4F534431u, 0x3258u, 0x5245u, {0x53u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x21u}};
inline constexpr GUID IID_ID3D12HeapX = {
    0x4F534431u, 0x3258u, 0x4845u, {0x41u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x22u}};
inline constexpr GUID IID_ID3D12DescriptorHeapX = {
    0x4F534431u, 0x3258u, 0x4448u, {0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x23u}};
inline constexpr GUID IID_ID3D12PipelineStateX = {
    0x4F534431u, 0x3258u, 0x5053u, {0x4Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x24u}};
inline constexpr GUID IID_ID3D12RootSignatureX = {
    0x4F534431u, 0x3258u, 0x5253u, {0x47u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x25u}};
inline constexpr GUID IID_ID3D12FenceX = {
    0x4F534431u, 0x3258u, 0x464Eu, {0x43u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x26u}};
inline constexpr GUID IID_ID3D12CommandAllocatorX = {
    0x4F534431u, 0x3258u, 0x4341u, {0x4Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x27u}};
inline constexpr GUID IID_ID3D12QueryHeapX = {
    0x4F534431u, 0x3258u, 0x5148u, {0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x28u}};
inline constexpr GUID IID_ID3D12CommandSignatureX = {
    0x4F534431u, 0x3258u, 0x4353u, {0x47u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x29u}};
inline constexpr GUID IID_ID3D12StateObjectX = {
    0x4F534431u, 0x3258u, 0x5354u, {0x4Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x2Au}};

struct IOpenScarlettProxyInspect : public IUnknown {
    virtual D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept = 0;
    virtual IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept = 0;
};

struct D3D12XBOX_CMDLIST_EXT_STATE {
    float depth_bounds_min = 0.0f;
    float depth_bounds_max = 1.0f;
    uint32_t view_instance_mask = 0u;
    uint32_t sample_positions_per_pixel = 0u;
    uint32_t sample_positions_pixels = 0u;
    uint64_t write_buffer_immediate_calls = 0ull;
    uint32_t host_supports_depth_bounds = 0u;
    uint32_t host_supports_view_instancing = 0u;
    uint32_t host_supports_sample_positions = 0u;
};

struct IOpenScarlettCommandListExtState : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCommandListExtState(
        D3D12XBOX_CMDLIST_EXT_STATE* out_state) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// 1. ResourceXProxy (ID3D12Resource / ID3D12ResourceX)
// ---------------------------------------------------------------------------
class ResourceXProxy final
    : public ID3D12Resource,
      public IOpenScarlettProxyInspect {
public:
    ResourceXProxy(
        DeviceX* parent_device,
        ID3D12Resource* host_resource,
        const D3D12_RESOURCE_DESC* desc_hint = nullptr,
        const D3D12_HEAP_PROPERTIES* heap_props_hint = nullptr,
        D3D12_HEAP_FLAGS heap_flags_hint = D3D12_HEAP_FLAG_NONE,
        D3D12_GPU_VIRTUAL_ADDRESS xbox_gpu_va_hint = 0ull);
    ~ResourceXProxy();

    ResourceXProxy(const ResourceXProxy&) = delete;
    ResourceXProxy& operator=(const ResourceXProxy&) = delete;

    ID3D12Resource* HostResource() const noexcept { return host_res_; }
    D3D12_GPU_VIRTUAL_ADDRESS TrackedXboxGpuVa() const noexcept { return xbox_gpu_va_; }
    void SetTrackedXboxGpuVa(D3D12_GPU_VIRTUAL_ADDRESS va) noexcept { xbox_gpu_va_ = va; }

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12DeviceChild (slot 7)
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    // ID3D12Resource (slots 8..14)
    HRESULT STDMETHODCALLTYPE Map(
        UINT Subresource, const D3D12_RANGE* pReadRange, void** ppData) override;
    void STDMETHODCALLTYPE Unmap(
        UINT Subresource, const D3D12_RANGE* pWrittenRange) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Resource::GetDesc;
    D3D12_RESOURCE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC* pRet) override;
#else
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override;
#endif
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override;
    HRESULT STDMETHODCALLTYPE WriteToSubresource(
        UINT DstSubresource, const D3D12_BOX* pDstBox,
        const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override;
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(
        void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
        UINT SrcSubresource, const D3D12_BOX* pSrcBox) override;
    HRESULT STDMETHODCALLTYPE GetHeapProperties(
        D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS* pHeapFlags) override;

    // IOpenScarlettProxyInspect
    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::Resource;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_res_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return xbox_gpu_va_;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12Resource* host_res_ = nullptr;
    D3D12_RESOURCE_DESC cached_desc_{};
    D3D12_HEAP_PROPERTIES cached_heap_props_{};
    D3D12_HEAP_FLAGS cached_heap_flags_ = D3D12_HEAP_FLAG_NONE;
    bool has_cached_heap_props_ = false;
    D3D12_GPU_VIRTUAL_ADDRESS xbox_gpu_va_ = 0ull;
};

// ---------------------------------------------------------------------------
// 2. HeapXProxy (ID3D12Heap / ID3D12HeapX)
// ---------------------------------------------------------------------------
class HeapXProxy final
    : public ID3D12Heap,
      public IOpenScarlettProxyInspect {
public:
    HeapXProxy(
        DeviceX* parent_device,
        ID3D12Heap* host_heap,
        uint64_t xbox_va_hint = 0ull);
    ~HeapXProxy();

    HeapXProxy(const HeapXProxy&) = delete;
    HeapXProxy& operator=(const HeapXProxy&) = delete;

    ID3D12Heap* HostHeap() const noexcept { return host_heap_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Heap::GetDesc;
    D3D12_HEAP_DESC* STDMETHODCALLTYPE GetDesc(D3D12_HEAP_DESC* pRet) override;
#else
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override;
#endif

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::Heap;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_heap_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return xbox_va_;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12Heap* host_heap_ = nullptr;
    uint64_t xbox_va_ = 0ull;
};

// ---------------------------------------------------------------------------
// 3. DescriptorHeapXProxy (ID3D12DescriptorHeap / ID3D12DescriptorHeapX)
// ---------------------------------------------------------------------------
class DescriptorHeapXProxy final
    : public ID3D12DescriptorHeap,
      public IOpenScarlettProxyInspect {
public:
    DescriptorHeapXProxy(DeviceX* parent_device, ID3D12DescriptorHeap* host_heap);
    ~DescriptorHeapXProxy();

    DescriptorHeapXProxy(const DescriptorHeapXProxy&) = delete;
    DescriptorHeapXProxy& operator=(const DescriptorHeapXProxy&) = delete;

    ID3D12DescriptorHeap* HostDescriptorHeap() const noexcept { return host_heap_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12DescriptorHeap::GetDesc;
    D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE GetDesc(
        D3D12_DESCRIPTOR_HEAP_DESC* pRet) override;
    using ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart;
    D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart(
        D3D12_CPU_DESCRIPTOR_HANDLE* pRet) override;
    using ID3D12DescriptorHeap::GetGPUDescriptorHandleForHeapStart;
    D3D12_GPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart(
        D3D12_GPU_DESCRIPTOR_HANDLE* pRet) override;
#else
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override;
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override;
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override;
#endif

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::DescriptorHeap;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_heap_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return gpu_start_.ptr;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12DescriptorHeap* host_heap_ = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC cached_desc_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start_{0ull};
};

// ---------------------------------------------------------------------------
// 4. PipelineStateXProxy (ID3D12PipelineState / ID3D12PipelineStateX)
//    Supports both native host D3D12 PSOs and direct SPIR-V Vulkan PSOs (host_pso == nullptr).
// ---------------------------------------------------------------------------
class PipelineStateXProxy final
    : public ID3D12PipelineState,
      public IOpenScarlettProxyInspect {
public:
    PipelineStateXProxy(DeviceX* parent_device, ID3D12PipelineState* host_pso);
    ~PipelineStateXProxy();

    PipelineStateXProxy(const PipelineStateXProxy&) = delete;
    PipelineStateXProxy& operator=(const PipelineStateXProxy&) = delete;

    ID3D12PipelineState* HostPipelineState() const noexcept { return host_pso_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** ppBlob) override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::PipelineState;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_pso_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12PipelineState* host_pso_ = nullptr;
};

// ---------------------------------------------------------------------------
// 5. RootSignatureXProxy (ID3D12RootSignature / ID3D12RootSignatureX)
//    Note: ID3D12RootSignature inherits from ID3D12DeviceChild (not ID3D12Pageable).
// ---------------------------------------------------------------------------
class RootSignatureXProxy final
    : public ID3D12RootSignature,
      public IOpenScarlettProxyInspect {
public:
    RootSignatureXProxy(DeviceX* parent_device, ID3D12RootSignature* host_rs);
    ~RootSignatureXProxy();

    RootSignatureXProxy(const RootSignatureXProxy&) = delete;
    RootSignatureXProxy& operator=(const RootSignatureXProxy&) = delete;

    ID3D12RootSignature* HostRootSignature() const noexcept { return host_rs_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::RootSignature;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_rs_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12RootSignature* host_rs_ = nullptr;
};

// ---------------------------------------------------------------------------
// 6. FenceXProxy (ID3D12Fence / ID3D12Fence1 / ID3D12FenceX)
//    Lock-free thread-safe forwarding for concurrent multi-queue Signal/Wait.
// ---------------------------------------------------------------------------
class FenceXProxy final
    : public ID3D12Fence1,
      public IOpenScarlettProxyInspect {
public:
    FenceXProxy(
        DeviceX* parent_device,
        ID3D12Fence* host_fence,
        D3D12_FENCE_FLAGS creation_flags = D3D12_FENCE_FLAG_NONE);
    ~FenceXProxy();

    FenceXProxy(const FenceXProxy&) = delete;
    FenceXProxy& operator=(const FenceXProxy&) = delete;

    ID3D12Fence* HostFence() const noexcept { return host_fence_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value, HANDLE hEvent) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 Value) override;

    // ID3D12Fence1
    D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::Fence;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_fence_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12Fence* host_fence_ = nullptr;
    ID3D12Fence1* host_fence1_ = nullptr;
    D3D12_FENCE_FLAGS creation_flags_ = D3D12_FENCE_FLAG_NONE;
};

// ---------------------------------------------------------------------------
// 7. CommandAllocatorXProxy (ID3D12CommandAllocator / ID3D12CommandAllocatorX)
// ---------------------------------------------------------------------------
class CommandAllocatorXProxy final
    : public ID3D12CommandAllocator,
      public IOpenScarlettProxyInspect {
public:
    CommandAllocatorXProxy(
        DeviceX* parent_device,
        ID3D12CommandAllocator* host_alloc,
        D3D12_COMMAND_LIST_TYPE type);
    ~CommandAllocatorXProxy();

    CommandAllocatorXProxy(const CommandAllocatorXProxy&) = delete;
    CommandAllocatorXProxy& operator=(const CommandAllocatorXProxy&) = delete;

    ID3D12CommandAllocator* HostAllocator() const noexcept { return host_alloc_; }
    D3D12_COMMAND_LIST_TYPE Type() const noexcept { return type_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    HRESULT STDMETHODCALLTYPE Reset() override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::CommandAllocator;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_alloc_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12CommandAllocator* host_alloc_ = nullptr;
    D3D12_COMMAND_LIST_TYPE type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

// ---------------------------------------------------------------------------
// 8. QueryHeapXProxy (ID3D12QueryHeap / ID3D12QueryHeapX)
// ---------------------------------------------------------------------------
class QueryHeapXProxy final
    : public ID3D12QueryHeap,
      public IOpenScarlettProxyInspect {
public:
    QueryHeapXProxy(
        DeviceX* parent_device,
        ID3D12QueryHeap* host_qheap,
        const D3D12_QUERY_HEAP_DESC* desc_hint = nullptr);
    ~QueryHeapXProxy();

    QueryHeapXProxy(const QueryHeapXProxy&) = delete;
    QueryHeapXProxy& operator=(const QueryHeapXProxy&) = delete;

    ID3D12QueryHeap* HostQueryHeap() const noexcept { return host_qheap_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::QueryHeap;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_qheap_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12QueryHeap* host_qheap_ = nullptr;
    D3D12_QUERY_HEAP_DESC cached_desc_{};
};

// ---------------------------------------------------------------------------
// 9. CommandSignatureXProxy (ID3D12CommandSignature / ID3D12CommandSignatureX)
// ---------------------------------------------------------------------------
class CommandSignatureXProxy final
    : public ID3D12CommandSignature,
      public IOpenScarlettProxyInspect {
public:
    CommandSignatureXProxy(
        DeviceX* parent_device,
        ID3D12CommandSignature* host_sig,
        UINT byte_stride = 0u,
        UINT num_arg_descs = 0u);
    ~CommandSignatureXProxy();

    CommandSignatureXProxy(const CommandSignatureXProxy&) = delete;
    CommandSignatureXProxy& operator=(const CommandSignatureXProxy&) = delete;

    ID3D12CommandSignature* HostCommandSignature() const noexcept { return host_sig_; }
    UINT ByteStride() const noexcept { return byte_stride_; }
    UINT NumArgumentDescs() const noexcept { return num_arg_descs_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::CommandSignature;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_sig_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12CommandSignature* host_sig_ = nullptr;
    UINT byte_stride_ = 0u;
    UINT num_arg_descs_ = 0u;
};

// ---------------------------------------------------------------------------
// 10. StateObjectXProxy (ID3D12StateObject / ID3D12StateObjectX + ID3D12StateObjectProperties)
// ---------------------------------------------------------------------------
class StateObjectXProxy final
    : public ID3D12StateObject,
      public ID3D12StateObjectProperties,
      public IOpenScarlettProxyInspect {
public:
    StateObjectXProxy(
        DeviceX* parent_device,
        ID3D12StateObject* host_so,
        const D3D12_STATE_OBJECT_DESC* desc_hint = nullptr);
    ~StateObjectXProxy();

    StateObjectXProxy(const StateObjectXProxy&) = delete;
    StateObjectXProxy& operator=(const StateObjectXProxy&) = delete;

    ID3D12StateObject* HostStateObject() const noexcept { return host_so_; }

    // IUnknown (shared across ID3D12StateObject, ID3D12StateObjectProperties, IOpenScarlettProxyInspect)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    // ID3D12StateObjectProperties
    void* STDMETHODCALLTYPE GetShaderIdentifier(LPCWSTR pExportName) override;
    UINT64 STDMETHODCALLTYPE GetShaderStackSize(LPCWSTR pExportName) override;
    UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override;
    void STDMETHODCALLTYPE SetPipelineStackSize(UINT64 PipelineStackSizeInBytes) override;

    // IOpenScarlettProxyInspect
    D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept override {
        return D3D12XProxyKind::StateObject;
    }
    IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept override {
        return host_so_;
    }
    uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept override {
        return 0ull;
    }

private:
    struct GuidKey {
        uint8_t bytes[16]{};
        bool operator==(const GuidKey& o) const noexcept {
            return std::memcmp(bytes, o.bytes, 16u) == 0;
        }
    };
    struct GuidKeyHash {
        size_t operator()(const GuidKey& k) const noexcept {
            uint64_t a = 0ull, b = 0ull;
            std::memcpy(&a, k.bytes, 8u);
            std::memcpy(&b, k.bytes + 8u, 8u);
            return static_cast<size_t>(a ^ (b * 0x9E3779B97F4A7C15ull));
        }
    };

    std::atomic<ULONG> ref_count_{1u};
    DeviceX* device_x_ = nullptr;
    ID3D12StateObject* host_so_ = nullptr;
    ID3D12StateObjectProperties* host_props_ = nullptr;
    mutable std::mutex sw_mutex_;
    std::atomic<UINT64> sw_pipeline_stack_size_{4096ull};
    std::wstring sw_name_;
    std::unordered_map<GuidKey, std::vector<uint8_t>, GuidKeyHash> sw_private_data_;
    std::unordered_map<std::wstring, std::array<uint8_t, 32>> sw_shader_ids_;
};

// ---------------------------------------------------------------------------
// Thread-safe O(1) sharded proxy unwrap helpers.
// Safe for nullptr, wrapped proxies, AND raw host D3D12 pointers alike.
// ---------------------------------------------------------------------------
ID3D12Resource* UnwrapResource(ID3D12Resource* maybe_wrapped) noexcept;
ID3D12Heap* UnwrapHeap(ID3D12Heap* maybe_wrapped) noexcept;
ID3D12DescriptorHeap* UnwrapDescriptorHeap(ID3D12DescriptorHeap* maybe_wrapped) noexcept;
ID3D12PipelineState* UnwrapPipelineState(ID3D12PipelineState* maybe_wrapped) noexcept;
ID3D12RootSignature* UnwrapRootSignature(ID3D12RootSignature* maybe_wrapped) noexcept;
ID3D12Fence* UnwrapFence(ID3D12Fence* maybe_wrapped) noexcept;
ID3D12CommandAllocator* UnwrapCommandAllocator(ID3D12CommandAllocator* maybe_wrapped) noexcept;
ID3D12QueryHeap* UnwrapQueryHeap(ID3D12QueryHeap* maybe_wrapped) noexcept;
ID3D12CommandSignature* UnwrapCommandSignature(ID3D12CommandSignature* maybe_wrapped) noexcept;
ID3D12StateObject* UnwrapStateObject(ID3D12StateObject* maybe_wrapped) noexcept;
ID3D12Pageable* UnwrapProxyPageable(ID3D12Pageable* maybe_wrapped) noexcept;
ID3D12DeviceChild* UnwrapProxyDeviceChild(ID3D12DeviceChild* maybe_wrapped) noexcept;

bool IsKnownProxyPointer(const void* ptr) noexcept;

}  // namespace openscarlett::d3d12_xs

#endif  // _WIN32
