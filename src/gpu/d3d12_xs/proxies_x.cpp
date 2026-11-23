#include "proxies_x.h"

#include <array>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "device_x.h"

namespace openscarlett::d3d12_xs {

namespace {

struct ProxyRegistryEntry {
    D3D12XProxyKind kind = D3D12XProxyKind::Unknown;
    IUnknown* host_obj = nullptr;
};

struct ProxyRegistryShard {
    std::shared_mutex mutex;
    std::unordered_map<const void*, ProxyRegistryEntry> map;
};

constexpr size_t kProxyShardCount = 64u;

size_t ShardIndex(const void* ptr) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
    return static_cast<size_t>((v >> 4u) ^ (v >> 11u)) & (kProxyShardCount - 1u);
}

std::array<ProxyRegistryShard, kProxyShardCount>& GetProxyShards() {
    static std::array<ProxyRegistryShard, kProxyShardCount> shards;
    return shards;
}

void RegisterProxy(const void* proxy_ptr, D3D12XProxyKind kind, IUnknown* host_obj) noexcept {
    if (!proxy_ptr) return;
    auto& shard = GetProxyShards()[ShardIndex(proxy_ptr)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.map[proxy_ptr] = ProxyRegistryEntry{kind, host_obj};
}

void UnregisterProxy(const void* proxy_ptr) noexcept {
    if (!proxy_ptr) return;
    auto& shard = GetProxyShards()[ShardIndex(proxy_ptr)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.map.erase(proxy_ptr);
}

bool LookupProxy(const void* maybe_wrapped, ProxyRegistryEntry& out_entry) noexcept {
    if (!maybe_wrapped) return false;
    auto& shard = GetProxyShards()[ShardIndex(maybe_wrapped)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.map.find(maybe_wrapped);
    if (it == shard.map.end()) return false;
    out_entry = it->second;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Thread-safe Unwrap helpers
// ---------------------------------------------------------------------------
ID3D12Resource* UnwrapResource(ID3D12Resource* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::Resource) {
        return static_cast<ID3D12Resource*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12Heap* UnwrapHeap(ID3D12Heap* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::Heap) {
        return static_cast<ID3D12Heap*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12DescriptorHeap* UnwrapDescriptorHeap(ID3D12DescriptorHeap* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::DescriptorHeap) {
        return static_cast<ID3D12DescriptorHeap*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12PipelineState* UnwrapPipelineState(ID3D12PipelineState* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::PipelineState) {
        return static_cast<ID3D12PipelineState*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12RootSignature* UnwrapRootSignature(ID3D12RootSignature* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::RootSignature) {
        return static_cast<ID3D12RootSignature*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12Fence* UnwrapFence(ID3D12Fence* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::Fence) {
        return static_cast<ID3D12Fence*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12CommandAllocator* UnwrapCommandAllocator(ID3D12CommandAllocator* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::CommandAllocator) {
        return static_cast<ID3D12CommandAllocator*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12QueryHeap* UnwrapQueryHeap(ID3D12QueryHeap* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::QueryHeap) {
        return static_cast<ID3D12QueryHeap*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12CommandSignature* UnwrapCommandSignature(ID3D12CommandSignature* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::CommandSignature) {
        return static_cast<ID3D12CommandSignature*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12StateObject* UnwrapStateObject(ID3D12StateObject* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.kind == D3D12XProxyKind::StateObject) {
        return static_cast<ID3D12StateObject*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12Pageable* UnwrapProxyPageable(ID3D12Pageable* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) &&
        entry.host_obj &&
        entry.kind != D3D12XProxyKind::RootSignature) {
        return static_cast<ID3D12Pageable*>(entry.host_obj);
    }
    return maybe_wrapped;
}

ID3D12DeviceChild* UnwrapProxyDeviceChild(ID3D12DeviceChild* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    ProxyRegistryEntry entry{};
    if (LookupProxy(static_cast<const void*>(maybe_wrapped), entry) && entry.host_obj) {
        return static_cast<ID3D12DeviceChild*>(entry.host_obj);
    }
    return maybe_wrapped;
}

bool IsKnownProxyPointer(const void* ptr) noexcept {
    ProxyRegistryEntry entry{};
    return LookupProxy(ptr, entry);
}

// ---------------------------------------------------------------------------
// 1. ResourceXProxy
// ---------------------------------------------------------------------------
ResourceXProxy::ResourceXProxy(
    DeviceX* parent_device,
    ID3D12Resource* host_resource,
    const D3D12_RESOURCE_DESC* desc_hint,
    const D3D12_HEAP_PROPERTIES* heap_props_hint,
    D3D12_HEAP_FLAGS heap_flags_hint,
    D3D12_GPU_VIRTUAL_ADDRESS xbox_gpu_va_hint)
    : device_x_(parent_device),
      host_res_(host_resource),
      cached_heap_flags_(heap_flags_hint),
      xbox_gpu_va_(xbox_gpu_va_hint) {
    if (device_x_) device_x_->AddRef();
    if (host_res_) {
        host_res_->AddRef();
#ifdef __ID3D12Resource1_INTERFACE_DEFINED__
        host_res_->QueryInterface(OS_D3D12_IID(ID3D12Resource1), reinterpret_cast<void**>(&host_res1_));
#endif
#ifdef __ID3D12Resource2_INTERFACE_DEFINED__
        host_res_->QueryInterface(OS_D3D12_IID(ID3D12Resource2), reinterpret_cast<void**>(&host_res2_));
#endif
        cached_desc_ = host_res_->GetDesc();
        if (cached_desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && xbox_gpu_va_ == 0ull) {
            xbox_gpu_va_ = host_res_->GetGPUVirtualAddress();
        }
        D3D12_HEAP_PROPERTIES hp{};
        D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
        if (SUCCEEDED(host_res_->GetHeapProperties(&hp, &hf))) {
            cached_heap_props_ = hp;
            cached_heap_flags_ = hf;
            has_cached_heap_props_ = true;
        }
    } else if (desc_hint) {
        cached_desc_ = *desc_hint;
    }
    if (!has_cached_heap_props_ && heap_props_hint) {
        cached_heap_props_ = *heap_props_hint;
        has_cached_heap_props_ = true;
    }
    RegisterProxy(static_cast<const ID3D12Resource*>(this), D3D12XProxyKind::Resource, host_res_);
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        static void* s_res_retail_vtbl[17]{};
        static std::once_flag s_res_once;
        static void* const* s_res_vtbl_ptr = nullptr;
        auto thunk_get_desc = +[](void* arg0, void* arg1) -> D3D12_RESOURCE_DESC* {
            ResourceXProxy* self = nullptr;
            D3D12_RESOURCE_DESC* out = nullptr;
            if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
                *reinterpret_cast<void* const* const*>(arg0) == s_res_vtbl_ptr) {
                self = static_cast<ResourceXProxy*>(arg0);
                out = static_cast<D3D12_RESOURCE_DESC*>(arg1);
            } else {
                out = static_cast<D3D12_RESOURCE_DESC*>(arg0);
                self = static_cast<ResourceXProxy*>(arg1);
            }
            if (out && self) {
                *out = self->HostResource() ? self->HostResource()->GetDesc() : D3D12_RESOURCE_DESC{};
            }
            return out;
        };
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        std::call_once(s_res_once, [raw_vtbl, thunk_get_desc]() {
            std::memcpy(s_res_retail_vtbl, raw_vtbl, sizeof(s_res_retail_vtbl));
            s_res_retail_vtbl[10] = reinterpret_cast<void*>(thunk_get_desc);
            s_res_vtbl_ptr = s_res_retail_vtbl;
        });
        *reinterpret_cast<void* const**>(this) = s_res_retail_vtbl;
    }
}

ResourceXProxy::~ResourceXProxy() {
    UnregisterProxy(static_cast<const ID3D12Resource*>(this));
    if (device_x_ && host_res_) {
        device_x_->NotifyResourceDestroyed(host_res_);
    }
#ifdef __ID3D12Resource2_INTERFACE_DEFINED__
    if (host_res2_) host_res2_->Release();
#endif
#ifdef __ID3D12Resource1_INTERFACE_DEFINED__
    if (host_res1_) host_res1_->Release();
#endif
    if (host_res_) host_res_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Resource)) ||
        IsEqualIID(riid, IID_ID3D12ResourceX)) {
        *ppvObject = static_cast<ID3D12Resource*>(this);
        AddRef();
        return S_OK;
    }
#ifdef __ID3D12Resource1_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Resource1))) {
        *ppvObject = static_cast<ID3D12Resource1*>(this);
        AddRef();
        return S_OK;
    }
#endif
#ifdef __ID3D12Resource2_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Resource2))) {
        *ppvObject = static_cast<ID3D12Resource2*>(this);
        AddRef();
        return S_OK;
    }
#endif
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    // Never fall through to raw host_res_->QueryInterface() so wrapped proxies cannot leak raw host pointers.
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ResourceXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE ResourceXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_res_ ? host_res_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_res_ ? host_res_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_res_ ? host_res_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::SetName(LPCWSTR Name) {
    return host_res_ ? host_res_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::Map(
    UINT Subresource, const D3D12_RANGE* pReadRange, void** ppData) {
    if (!host_res_) return E_FAIL;
    const bool skip_host_map =
        device_x_ && device_x_->IsRetailScarlettAbi() &&
        has_cached_heap_props_ && cached_heap_props_.Type == D3D12_HEAP_TYPE_DEFAULT;
    if (!skip_host_map) {
        const HRESULT hr = host_res_->Map(Subresource, pReadRange, ppData);
        if (SUCCEEDED(hr)) {
            return hr;
        }
    }
    // Xbox Series X UMA compatibility: titles routinely call Map() directly on
    // D3D12_HEAP_TYPE_DEFAULT resources (e.g. to zero-init textures/buffers).
    // On discrete PC GPUs where host Map() returns E_INVALIDARG, back the map
    // with a zero-initialized CPU shadow buffer sized to GetResourceAllocationInfo.
    std::lock_guard<std::mutex> lk(uma_map_mutex_);
    if (uma_cpu_shadow_.empty()) {
        uint64_t alloc_bytes = 0ull;
        if (device_x_) {
            const D3D12_RESOURCE_ALLOCATION_INFO info =
                device_x_->GetResourceAllocationInfo(0u, 1u, &cached_desc_);
            alloc_bytes = info.SizeInBytes;
        }
        if (alloc_bytes == 0ull || alloc_bytes == UINT64_MAX) {
            alloc_bytes = std::max<uint64_t>(cached_desc_.Width, 65536ull);
        }
        // Cap sanity bound at 512 MiB to prevent runaway allocations on malformed descs.
        alloc_bytes = std::min<uint64_t>(alloc_bytes, 512ull * 1024ull * 1024ull);
        // Add 256 KiB alignment headroom for Xbox XG tiled surface sizes that exceed PC GetResourceAllocationInfo.
        const uint64_t padded_bytes = ((alloc_bytes + 65535ull) & ~65535ull) + 262144ull;
        uma_cpu_shadow_.assign(static_cast<size_t>(padded_bytes), 0u);
    }
    uma_shadow_mapped_ = true;
    if (ppData) {
        *ppData = uma_cpu_shadow_.data();
    }
    return S_OK;
}

void STDMETHODCALLTYPE ResourceXProxy::Unmap(
    UINT Subresource, const D3D12_RANGE* pWrittenRange) {
    {
        std::lock_guard<std::mutex> lk(uma_map_mutex_);
        if (uma_shadow_mapped_) {
            uma_shadow_mapped_ = false;
            return;
        }
    }
    if (host_res_) host_res_->Unmap(Subresource, pWrittenRange);
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_DESC* STDMETHODCALLTYPE ResourceXProxy::GetDesc(D3D12_RESOURCE_DESC* pRet) {
    if (!pRet) return nullptr;
    if (host_res_) {
        host_res_->GetDesc(pRet);
    } else {
        *pRet = cached_desc_;
    }
    return pRet;
}
#else
D3D12_RESOURCE_DESC STDMETHODCALLTYPE ResourceXProxy::GetDesc() {
    return host_res_ ? host_res_->GetDesc() : cached_desc_;
}
#endif

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE ResourceXProxy::GetGPUVirtualAddress() {
    if (host_res_) {
        return host_res_->GetGPUVirtualAddress();
    }
    return xbox_gpu_va_;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::WriteToSubresource(
    UINT DstSubresource, const D3D12_BOX* pDstBox,
    const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) {
    return host_res_
               ? host_res_->WriteToSubresource(
                     DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch)
               : E_FAIL;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::ReadFromSubresource(
    void* pDstData, UINT DstRowPitch, UINT DstDepthPitch,
    UINT SrcSubresource, const D3D12_BOX* pSrcBox) {
    return host_res_
               ? host_res_->ReadFromSubresource(
                     pDstData, DstRowPitch, DstDepthPitch, SrcSubresource, pSrcBox)
               : E_FAIL;
}

HRESULT STDMETHODCALLTYPE ResourceXProxy::GetHeapProperties(
    D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS* pHeapFlags) {
    if (host_res_) {
        const HRESULT hr = host_res_->GetHeapProperties(pHeapProperties, pHeapFlags);
        if (SUCCEEDED(hr) && l0_spilled_from_default_ && pHeapProperties) {
            pHeapProperties->Type = D3D12_HEAP_TYPE_DEFAULT;
            pHeapProperties->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            pHeapProperties->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        }
        return hr;
    }
    if (!has_cached_heap_props_) return E_FAIL;
    if (pHeapProperties) *pHeapProperties = cached_heap_props_;
    if (pHeapFlags) *pHeapFlags = cached_heap_flags_;
    return S_OK;
}

#ifdef __ID3D12Resource1_INTERFACE_DEFINED__
HRESULT STDMETHODCALLTYPE ResourceXProxy::GetProtectedResourceSession(
    REFIID riid, void** ppProtectedSession) {
    if (!ppProtectedSession) return E_POINTER;
    *ppProtectedSession = nullptr;
    if (host_res1_) {
        return host_res1_->GetProtectedResourceSession(riid, ppProtectedSession);
    }
    return DXGI_ERROR_NOT_FOUND;
}
#endif

#ifdef __ID3D12Resource2_INTERFACE_DEFINED__
namespace {
D3D12_RESOURCE_DESC1 ConvertResourceDescToDesc1(const D3D12_RESOURCE_DESC& d) noexcept {
    D3D12_RESOURCE_DESC1 d1{};
    d1.Dimension = d.Dimension;
    d1.Alignment = d.Alignment;
    d1.Width = d.Width;
    d1.Height = d.Height;
    d1.DepthOrArraySize = d.DepthOrArraySize;
    d1.MipLevels = d.MipLevels;
    d1.Format = d.Format;
    d1.SampleDesc = d.SampleDesc;
    d1.Layout = d.Layout;
    d1.Flags = d.Flags;
    d1.SamplerFeedbackMipRegion = D3D12_MIP_REGION{0u, 0u, 0u};
    return d1;
}
}  // namespace

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_RESOURCE_DESC1* STDMETHODCALLTYPE ResourceXProxy::GetDesc1(D3D12_RESOURCE_DESC1* pRet) {
    if (!pRet) return nullptr;
    if (host_res2_) {
        host_res2_->GetDesc1(pRet);
    } else {
        D3D12_RESOURCE_DESC base{};
        if (host_res_) {
            host_res_->GetDesc(&base);
        } else {
            base = cached_desc_;
        }
        *pRet = ConvertResourceDescToDesc1(base);
    }
    return pRet;
}
#else
D3D12_RESOURCE_DESC1 STDMETHODCALLTYPE ResourceXProxy::GetDesc1() {
    if (host_res2_) {
        return host_res2_->GetDesc1();
    }
    const D3D12_RESOURCE_DESC base = host_res_ ? host_res_->GetDesc() : cached_desc_;
    return ConvertResourceDescToDesc1(base);
}
#endif
#endif

// ---------------------------------------------------------------------------
// 2. HeapXProxy
// ---------------------------------------------------------------------------
HeapXProxy::HeapXProxy(DeviceX* parent_device, ID3D12Heap* host_heap, uint64_t xbox_va_hint)
    : device_x_(parent_device), host_heap_(host_heap), xbox_va_(xbox_va_hint) {
    if (device_x_) device_x_->AddRef();
    if (host_heap_) {
        host_heap_->AddRef();
#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
        host_heap_->QueryInterface(OS_D3D12_IID(ID3D12Heap1), reinterpret_cast<void**>(&host_heap1_));
#endif
    }
    RegisterProxy(static_cast<const ID3D12Heap*>(this), D3D12XProxyKind::Heap, host_heap_);
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        static void* s_heap_retail_vtbl[10]{};
        static std::once_flag s_heap_once;
        static void* const* s_heap_vtbl_ptr = nullptr;
        auto thunk_get_desc = +[](void* arg0, void* arg1) -> D3D12_HEAP_DESC* {
            HeapXProxy* self = nullptr;
            D3D12_HEAP_DESC* out = nullptr;
            if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
                *reinterpret_cast<void* const* const*>(arg0) == s_heap_vtbl_ptr) {
                self = static_cast<HeapXProxy*>(arg0);
                out = static_cast<D3D12_HEAP_DESC*>(arg1);
            } else {
                out = static_cast<D3D12_HEAP_DESC*>(arg0);
                self = static_cast<HeapXProxy*>(arg1);
            }
            if (out && self) {
                D3D12_HEAP_DESC d = self->HostHeap() ? self->HostHeap()->GetDesc() : D3D12_HEAP_DESC{};
                if (self->IsL0SpilledFromDefault()) {
                    d.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                    d.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                    d.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                }
                *out = d;
            }
            return out;
        };
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        std::call_once(s_heap_once, [raw_vtbl, thunk_get_desc]() {
            std::memcpy(s_heap_retail_vtbl, raw_vtbl, sizeof(s_heap_retail_vtbl));
            s_heap_retail_vtbl[8] = reinterpret_cast<void*>(thunk_get_desc);
            s_heap_vtbl_ptr = s_heap_retail_vtbl;
        });
        *reinterpret_cast<void* const**>(this) = s_heap_retail_vtbl;
    }
}

HeapXProxy::~HeapXProxy() {
    UnregisterProxy(static_cast<const ID3D12Heap*>(this));
    if (device_x_ && host_heap_) {
        device_x_->NotifyHeapDestroyed(host_heap_);
    }
#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
    if (host_heap1_) host_heap1_->Release();
#endif
    if (host_heap_) host_heap_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE HeapXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Heap)) ||
        IsEqualIID(riid, IID_ID3D12HeapX)) {
        *ppvObject = static_cast<ID3D12Heap*>(this);
        AddRef();
        return S_OK;
    }
#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Heap1))) {
        *ppvObject = static_cast<ID3D12Heap1*>(this);
        AddRef();
        return S_OK;
    }
#endif
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    // Never fall through to raw host_heap_->QueryInterface() so wrapped proxies cannot leak raw host pointers.
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE HeapXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE HeapXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE HeapXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_heap_ ? host_heap_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE HeapXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_heap_ ? host_heap_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE HeapXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_heap_ ? host_heap_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE HeapXProxy::SetName(LPCWSTR Name) {
    return host_heap_ ? host_heap_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE HeapXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_HEAP_DESC* STDMETHODCALLTYPE HeapXProxy::GetDesc(D3D12_HEAP_DESC* pRet) {
    if (!pRet) return nullptr;
    if (host_heap_) {
        host_heap_->GetDesc(pRet);
        if (l0_spilled_from_default_) {
            pRet->Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            pRet->Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            pRet->Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        }
    } else {
        std::memset(pRet, 0, sizeof(*pRet));
    }
    return pRet;
}
#else
D3D12_HEAP_DESC STDMETHODCALLTYPE HeapXProxy::GetDesc() {
    D3D12_HEAP_DESC d = host_heap_ ? host_heap_->GetDesc() : D3D12_HEAP_DESC{};
    if (l0_spilled_from_default_) {
        d.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        d.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        d.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
    return d;
}
#endif

#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
HRESULT STDMETHODCALLTYPE HeapXProxy::GetProtectedResourceSession(
    REFIID riid, void** ppProtectedSession) {
    if (!ppProtectedSession) return E_POINTER;
    *ppProtectedSession = nullptr;
    if (host_heap1_) {
        return host_heap1_->GetProtectedResourceSession(riid, ppProtectedSession);
    }
    return DXGI_ERROR_NOT_FOUND;
}
#endif

// ---------------------------------------------------------------------------
// 3. DescriptorHeapXProxy
// ---------------------------------------------------------------------------
DescriptorHeapXProxy::DescriptorHeapXProxy(DeviceX* parent_device, ID3D12DescriptorHeap* host_heap)
    : device_x_(parent_device), host_heap_(host_heap) {
    if (device_x_) device_x_->AddRef();
    if (host_heap_) {
        host_heap_->AddRef();
        cached_desc_ = host_heap_->GetDesc();
        if ((cached_desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
            gpu_start_ = host_heap_->GetGPUDescriptorHandleForHeapStart();
        }
    }
    RegisterProxy(
        static_cast<const ID3D12DescriptorHeap*>(this),
        D3D12XProxyKind::DescriptorHeap,
        host_heap_);
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        static void* s_dh_retail_vtbl[11]{};
        static std::once_flag s_dh_once;
        static void* const* s_dh_vtbl_ptr = nullptr;
        auto thunk_get_desc = +[](void* arg0, void* arg1) -> D3D12_DESCRIPTOR_HEAP_DESC* {
            DescriptorHeapXProxy* self = nullptr;
            D3D12_DESCRIPTOR_HEAP_DESC* out = nullptr;
            if (arg0 && reinterpret_cast<uintptr_t>(arg0) >= 0x10000u &&
                *reinterpret_cast<void* const* const*>(arg0) == s_dh_vtbl_ptr) {
                self = static_cast<DescriptorHeapXProxy*>(arg0);
                out = static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(arg1);
            } else {
                out = static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(arg0);
                self = static_cast<DescriptorHeapXProxy*>(arg1);
            }
            if (out && self) {
                *out = self->HostDescriptorHeap() ? self->HostDescriptorHeap()->GetDesc()
                                                  : D3D12_DESCRIPTOR_HEAP_DESC{};
            }
            return out;
        };
        auto thunk_cpu_start = +[](DescriptorHeapXProxy* self) -> SIZE_T {
            if (!self || !self->HostDescriptorHeap()) return 0u;
            return self->HostDescriptorHeap()->GetCPUDescriptorHandleForHeapStart().ptr;
        };
        auto thunk_gpu_start = +[](DescriptorHeapXProxy* self) -> UINT64 {
            if (!self || !self->HostDescriptorHeap()) return 0ull;
            return self->HostDescriptorHeap()->GetGPUDescriptorHandleForHeapStart().ptr;
        };
        const void* const* raw_vtbl = *reinterpret_cast<const void* const* const*>(this);
        std::call_once(s_dh_once, [raw_vtbl, thunk_get_desc, thunk_cpu_start, thunk_gpu_start]() {
            std::memcpy(s_dh_retail_vtbl, raw_vtbl, sizeof(s_dh_retail_vtbl));
            s_dh_retail_vtbl[8] = reinterpret_cast<void*>(thunk_get_desc);
            s_dh_retail_vtbl[9] = reinterpret_cast<void*>(thunk_cpu_start);
            s_dh_retail_vtbl[10] = reinterpret_cast<void*>(thunk_gpu_start);
            s_dh_vtbl_ptr = s_dh_retail_vtbl;
        });
        *reinterpret_cast<void* const**>(this) = s_dh_retail_vtbl;
    }
}

DescriptorHeapXProxy::~DescriptorHeapXProxy() {
    UnregisterProxy(static_cast<const ID3D12DescriptorHeap*>(this));
    if (device_x_ && host_heap_) {
        device_x_->NotifyDescriptorHeapDestroyed(host_heap_);
    }
    if (host_heap_) host_heap_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DescriptorHeap)) ||
        IsEqualIID(riid, IID_ID3D12DescriptorHeapX)) {
        *ppvObject = static_cast<ID3D12DescriptorHeap*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_heap_ ? host_heap_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DescriptorHeapXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE DescriptorHeapXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_heap_ ? host_heap_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_heap_ ? host_heap_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_heap_ ? host_heap_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::SetName(LPCWSTR Name) {
    return host_heap_ ? host_heap_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE DescriptorHeapXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE DescriptorHeapXProxy::GetDesc(
    D3D12_DESCRIPTOR_HEAP_DESC* pRet) {
    if (!pRet) return nullptr;
    if (host_heap_) {
        host_heap_->GetDesc(pRet);
    } else {
        *pRet = cached_desc_;
    }
    return pRet;
}

D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE DescriptorHeapXProxy::GetCPUDescriptorHandleForHeapStart(
    D3D12_CPU_DESCRIPTOR_HANDLE* pRet) {
    if (!pRet) return nullptr;
    if (host_heap_) {
        host_heap_->GetCPUDescriptorHandleForHeapStart(pRet);
    } else {
        pRet->ptr = 0u;
    }
    return pRet;
}

D3D12_GPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE DescriptorHeapXProxy::GetGPUDescriptorHandleForHeapStart(
    D3D12_GPU_DESCRIPTOR_HANDLE* pRet) {
    if (!pRet) return nullptr;
    if (host_heap_) {
        host_heap_->GetGPUDescriptorHandleForHeapStart(pRet);
    } else {
        pRet->ptr = 0ull;
    }
    return pRet;
}
#else
D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE DescriptorHeapXProxy::GetDesc() {
    return host_heap_ ? host_heap_->GetDesc() : cached_desc_;
}

D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE DescriptorHeapXProxy::GetCPUDescriptorHandleForHeapStart() {
    D3D12_CPU_DESCRIPTOR_HANDLE h{0u};
    return host_heap_ ? host_heap_->GetCPUDescriptorHandleForHeapStart() : h;
}

D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE DescriptorHeapXProxy::GetGPUDescriptorHandleForHeapStart() {
    D3D12_GPU_DESCRIPTOR_HANDLE h{0ull};
    return host_heap_ ? host_heap_->GetGPUDescriptorHandleForHeapStart() : h;
}
#endif

// ---------------------------------------------------------------------------
// 4. PipelineStateXProxy
// ---------------------------------------------------------------------------
PipelineStateXProxy::PipelineStateXProxy(DeviceX* parent_device, ID3D12PipelineState* host_pso)
    : device_x_(parent_device), host_pso_(host_pso) {
    if (device_x_) device_x_->AddRef();
    if (host_pso_) host_pso_->AddRef();
    RegisterProxy(
        static_cast<const ID3D12PipelineState*>(this),
        D3D12XProxyKind::PipelineState,
        host_pso_);
}

PipelineStateXProxy::~PipelineStateXProxy() {
    UnregisterProxy(static_cast<const ID3D12PipelineState*>(this));
    if (device_x_) {
        device_x_->NotifyPipelineDestroyed(static_cast<ID3D12PipelineState*>(this));
    }
    if (host_pso_) host_pso_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12PipelineState)) ||
        IsEqualIID(riid, IID_ID3D12PipelineStateX)) {
        *ppvObject = static_cast<ID3D12PipelineState*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_pso_ ? host_pso_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE PipelineStateXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE PipelineStateXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_pso_ ? host_pso_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_pso_ ? host_pso_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_pso_ ? host_pso_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::SetName(LPCWSTR Name) {
    return host_pso_ ? host_pso_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE PipelineStateXProxy::GetCachedBlob(ID3DBlob** ppBlob) {
    if (host_pso_) return host_pso_->GetCachedBlob(ppBlob);
    if (ppBlob) *ppBlob = nullptr;
    return E_NOTIMPL;
}

// ---------------------------------------------------------------------------
// 5. RootSignatureXProxy
// ---------------------------------------------------------------------------
RootSignatureXProxy::RootSignatureXProxy(DeviceX* parent_device, ID3D12RootSignature* host_rs)
    : device_x_(parent_device), host_rs_(host_rs) {
    if (device_x_) device_x_->AddRef();
    if (host_rs_) host_rs_->AddRef();
    RegisterProxy(
        static_cast<const ID3D12RootSignature*>(this),
        D3D12XProxyKind::RootSignature,
        host_rs_);
}

RootSignatureXProxy::~RootSignatureXProxy() {
    UnregisterProxy(static_cast<const ID3D12RootSignature*>(this));
    if (host_rs_) host_rs_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12RootSignature)) ||
        IsEqualIID(riid, IID_ID3D12RootSignatureX)) {
        *ppvObject = static_cast<ID3D12RootSignature*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_rs_ ? host_rs_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE RootSignatureXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE RootSignatureXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_rs_ ? host_rs_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_rs_ ? host_rs_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_rs_ ? host_rs_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::SetName(LPCWSTR Name) {
    return host_rs_ ? host_rs_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE RootSignatureXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

// ---------------------------------------------------------------------------
// 6. FenceXProxy
// ---------------------------------------------------------------------------
FenceXProxy::FenceXProxy(
    DeviceX* parent_device,
    ID3D12Fence* host_fence,
    D3D12_FENCE_FLAGS creation_flags)
    : device_x_(parent_device), host_fence_(host_fence), creation_flags_(creation_flags) {
    if (device_x_) device_x_->AddRef();
    if (host_fence_) {
        host_fence_->AddRef();
        host_fence_->QueryInterface(OS_D3D12_IID(ID3D12Fence1), reinterpret_cast<void**>(&host_fence1_));
    }
    RegisterProxy(static_cast<const ID3D12Fence*>(this), D3D12XProxyKind::Fence, host_fence_);
}

FenceXProxy::~FenceXProxy() {
    UnregisterProxy(static_cast<const ID3D12Fence*>(this));
    if (host_fence1_) host_fence1_->Release();
    if (host_fence_) host_fence_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE FenceXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Fence)) ||
        IsEqualIID(riid, IID_ID3D12FenceX)) {
        *ppvObject = static_cast<ID3D12Fence*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12Fence1)) && host_fence1_) {
        *ppvObject = static_cast<ID3D12Fence1*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_fence_ ? host_fence_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FenceXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE FenceXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_fence_ ? host_fence_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_fence_ ? host_fence_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_fence_ ? host_fence_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::SetName(LPCWSTR Name) {
    return host_fence_ ? host_fence_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

UINT64 STDMETHODCALLTYPE FenceXProxy::GetCompletedValue() {
    return host_fence_ ? host_fence_->GetCompletedValue() : UINT64_MAX;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::SetEventOnCompletion(UINT64 Value, HANDLE hEvent) {
    return host_fence_ ? host_fence_->SetEventOnCompletion(Value, hEvent) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE FenceXProxy::Signal(UINT64 Value) {
    return host_fence_ ? host_fence_->Signal(Value) : E_FAIL;
}

D3D12_FENCE_FLAGS STDMETHODCALLTYPE FenceXProxy::GetCreationFlags() {
    return host_fence1_ ? host_fence1_->GetCreationFlags() : creation_flags_;
}

// ---------------------------------------------------------------------------
// 7. CommandAllocatorXProxy
// ---------------------------------------------------------------------------
CommandAllocatorXProxy::CommandAllocatorXProxy(
    DeviceX* parent_device,
    ID3D12CommandAllocator* host_alloc,
    D3D12_COMMAND_LIST_TYPE type)
    : device_x_(parent_device), host_alloc_(host_alloc), type_(type) {
    if (device_x_) device_x_->AddRef();
    if (host_alloc_) host_alloc_->AddRef();
    RegisterProxy(
        static_cast<const ID3D12CommandAllocator*>(this),
        D3D12XProxyKind::CommandAllocator,
        host_alloc_);
}

CommandAllocatorXProxy::~CommandAllocatorXProxy() {
    UnregisterProxy(static_cast<const ID3D12CommandAllocator*>(this));
    if (host_alloc_) host_alloc_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12CommandAllocator)) ||
        IsEqualIID(riid, IID_ID3D12CommandAllocatorX)) {
        *ppvObject = static_cast<ID3D12CommandAllocator*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_alloc_ ? host_alloc_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandAllocatorXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE CommandAllocatorXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_alloc_ ? host_alloc_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_alloc_ ? host_alloc_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_alloc_ ? host_alloc_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::SetName(LPCWSTR Name) {
    return host_alloc_ ? host_alloc_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE CommandAllocatorXProxy::Reset() {
    return host_alloc_ ? host_alloc_->Reset() : E_FAIL;
}

// ---------------------------------------------------------------------------
// 8. QueryHeapXProxy
// ---------------------------------------------------------------------------
QueryHeapXProxy::QueryHeapXProxy(
    DeviceX* parent_device,
    ID3D12QueryHeap* host_qheap,
    const D3D12_QUERY_HEAP_DESC* desc_hint)
    : device_x_(parent_device), host_qheap_(host_qheap) {
    if (device_x_) device_x_->AddRef();
    if (host_qheap_) host_qheap_->AddRef();
    if (desc_hint) cached_desc_ = *desc_hint;
    RegisterProxy(
        static_cast<const ID3D12QueryHeap*>(this),
        D3D12XProxyKind::QueryHeap,
        host_qheap_);
}

QueryHeapXProxy::~QueryHeapXProxy() {
    UnregisterProxy(static_cast<const ID3D12QueryHeap*>(this));
    if (host_qheap_) host_qheap_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12QueryHeap)) ||
        IsEqualIID(riid, IID_ID3D12QueryHeapX)) {
        *ppvObject = static_cast<ID3D12QueryHeap*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_qheap_ ? host_qheap_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE QueryHeapXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE QueryHeapXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_qheap_ ? host_qheap_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_qheap_ ? host_qheap_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_qheap_ ? host_qheap_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::SetName(LPCWSTR Name) {
    return host_qheap_ ? host_qheap_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE QueryHeapXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

// ---------------------------------------------------------------------------
// 9. CommandSignatureXProxy
// ---------------------------------------------------------------------------
CommandSignatureXProxy::CommandSignatureXProxy(
    DeviceX* parent_device,
    ID3D12CommandSignature* host_sig,
    UINT byte_stride,
    UINT num_arg_descs)
    : device_x_(parent_device),
      host_sig_(host_sig),
      byte_stride_(byte_stride),
      num_arg_descs_(num_arg_descs) {
    if (device_x_) device_x_->AddRef();
    if (host_sig_) host_sig_->AddRef();
    RegisterProxy(
        static_cast<const ID3D12CommandSignature*>(this),
        D3D12XProxyKind::CommandSignature,
        host_sig_);
}

CommandSignatureXProxy::~CommandSignatureXProxy() {
    UnregisterProxy(static_cast<const ID3D12CommandSignature*>(this));
    if (host_sig_) host_sig_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12CommandSignature)) ||
        IsEqualIID(riid, IID_ID3D12CommandSignatureX)) {
        *ppvObject = static_cast<ID3D12CommandSignature*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_sig_ ? host_sig_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandSignatureXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE CommandSignatureXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return host_sig_ ? host_sig_->GetPrivateData(guid, pDataSize, pData) : DXGI_ERROR_NOT_FOUND;
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return host_sig_ ? host_sig_->SetPrivateData(guid, DataSize, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_sig_ ? host_sig_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::SetName(LPCWSTR Name) {
    return host_sig_ ? host_sig_->SetName(Name) : S_OK;
}

HRESULT STDMETHODCALLTYPE CommandSignatureXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

// ---------------------------------------------------------------------------
// 10. StateObjectXProxy
// ---------------------------------------------------------------------------
StateObjectXProxy::StateObjectXProxy(
    DeviceX* parent_device,
    ID3D12StateObject* host_so,
    const D3D12_STATE_OBJECT_DESC* desc_hint,
    bool allow_dynamic_exports)
    : device_x_(parent_device), host_so_(host_so), sw_allow_dynamic_exports_(allow_dynamic_exports) {
    if (device_x_) device_x_->AddRef();
    if (host_so_) {
        host_so_->AddRef();
        host_so_->QueryInterface(
            OS_D3D12_IID(ID3D12StateObjectProperties),
            reinterpret_cast<void**>(&host_props_));
    } else if (desc_hint && desc_hint->pSubobjects && desc_hint->NumSubobjects > 0u) {
        auto register_export = [&](const wchar_t* name) {
            if (!name || !name[0]) return;
            std::wstring key(name);
            std::array<uint8_t, 32> id{};
            uint64_t h1 = 0xCBF29CE484222325ull;
            uint64_t h2 = 0x51A7D3D120000001ull;
            for (wchar_t ch : key) {
                h1 = (h1 ^ static_cast<uint64_t>(ch)) * 0x100000001B3ull;
                h2 = (h2 * 0x100000001B3ull) ^ static_cast<uint64_t>(ch + 0x9Eu);
            }
            std::memcpy(id.data() + 0u, &h1, 8u);
            std::memcpy(id.data() + 8u, &h2, 8u);
            const uint64_t tag = 0x4F53445852534254ull;  // "OSDXRSBT"
            std::memcpy(id.data() + 16u, &tag, 8u);
            const uint64_t len = static_cast<uint64_t>(key.size());
            std::memcpy(id.data() + 24u, &len, 8u);
            sw_shader_ids_[std::move(key)] = id;
        };
        for (UINT i = 0u; i < desc_hint->NumSubobjects; ++i) {
            const auto& sub = desc_hint->pSubobjects[i];
            if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && sub.pDesc) {
                const auto* lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(sub.pDesc);
                for (UINT e = 0u; e < lib->NumExports; ++e) {
                    register_export(lib->pExports[e].Name);
                }
            } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP && sub.pDesc) {
                const auto* hg = static_cast<const D3D12_HIT_GROUP_DESC*>(sub.pDesc);
                register_export(hg->HitGroupExport);
            }
        }
    }
    RegisterProxy(
        static_cast<const ID3D12StateObject*>(this),
        D3D12XProxyKind::StateObject,
        host_so_);
}

StateObjectXProxy::~StateObjectXProxy() {
    UnregisterProxy(static_cast<const ID3D12StateObject*>(this));
    if (host_props_) host_props_->Release();
    if (host_so_) host_so_->Release();
    if (device_x_) device_x_->Release();
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Pageable)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12StateObject)) ||
        IsEqualIID(riid, IID_ID3D12StateObjectX)) {
        *ppvObject = static_cast<ID3D12StateObject*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12StateObjectProperties)) &&
        (host_props_ != nullptr || host_so_ == nullptr)) {
        *ppvObject = static_cast<ID3D12StateObjectProperties*>(this);
        AddRef();
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettProxyInspect)) {
        *ppvObject = static_cast<IOpenScarlettProxyInspect*>(this);
        AddRef();
        return S_OK;
    }
    return host_so_ ? host_so_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE StateObjectXProxy::AddRef() {
    return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

ULONG STDMETHODCALLTYPE StateObjectXProxy::Release() {
    const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (rem == 0u) delete this;
    return rem;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    if (host_so_) return host_so_->GetPrivateData(guid, pDataSize, pData);
    if (!pDataSize) return E_POINTER;
    GuidKey key{};
    std::memcpy(key.bytes, &guid, 16u);
    std::lock_guard<std::mutex> lock(sw_mutex_);
    auto it = sw_private_data_.find(key);
    if (it == sw_private_data_.end()) return DXGI_ERROR_NOT_FOUND;
    const UINT stored = static_cast<UINT>(it->second.size());
    if (!pData) {
        *pDataSize = stored;
        return S_OK;
    }
    if (*pDataSize < stored) {
        *pDataSize = stored;
        return DXGI_ERROR_MORE_DATA;
    }
    if (stored > 0u) std::memcpy(pData, it->second.data(), stored);
    *pDataSize = stored;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    if (host_so_) return host_so_->SetPrivateData(guid, DataSize, pData);
    GuidKey key{};
    std::memcpy(key.bytes, &guid, 16u);
    std::lock_guard<std::mutex> lock(sw_mutex_);
    if (!pData || DataSize == 0u) {
        sw_private_data_.erase(key);
        return S_OK;
    }
    const auto* bytes = static_cast<const uint8_t*>(pData);
    sw_private_data_[key].assign(bytes, bytes + DataSize);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return host_so_ ? host_so_->SetPrivateDataInterface(guid, pData) : S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::SetName(LPCWSTR Name) {
    if (host_so_) return host_so_->SetName(Name);
    std::lock_guard<std::mutex> lock(sw_mutex_);
    sw_name_ = Name ? Name : L"";
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::GetDevice(REFIID riid, void** ppvDevice) {
    return device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
}

void* STDMETHODCALLTYPE StateObjectXProxy::GetShaderIdentifier(LPCWSTR pExportName) {
    if (host_props_) return host_props_->GetShaderIdentifier(pExportName);
    if (!pExportName || !pExportName[0]) return nullptr;
    std::lock_guard<std::mutex> lock(sw_mutex_);
    auto it = sw_shader_ids_.find(pExportName);
    if (it != sw_shader_ids_.end()) {
        return static_cast<void*>(it->second.data());
    }
    if (!sw_allow_dynamic_exports_) {
        return nullptr;
    }
    std::wstring key(pExportName);
    std::array<uint8_t, 32> id{};
    uint64_t h1 = 0xCBF29CE484222325ull;
    uint64_t h2 = 0x51A7D3D120000001ull;
    for (wchar_t ch : key) {
        h1 = (h1 ^ static_cast<uint64_t>(ch)) * 0x100000001B3ull;
        h2 = (h2 * 0x100000001B3ull) ^ static_cast<uint64_t>(ch + 0x9Eu);
    }
    std::memcpy(id.data() + 0u, &h1, 8u);
    std::memcpy(id.data() + 8u, &h2, 8u);
    const uint64_t tag = 0x4F53445852534254ull;  // "OSDXRSBT"
    std::memcpy(id.data() + 16u, &tag, 8u);
    const uint64_t len = static_cast<uint64_t>(key.size());
    std::memcpy(id.data() + 24u, &len, 8u);
    auto [ins_it, _] = sw_shader_ids_.emplace(std::move(key), id);
    return static_cast<void*>(ins_it->second.data());
}

UINT64 STDMETHODCALLTYPE StateObjectXProxy::GetShaderStackSize(LPCWSTR pExportName) {
    if (host_props_) return host_props_->GetShaderStackSize(pExportName);
    if (!pExportName || !pExportName[0]) return 0ull;
    std::lock_guard<std::mutex> lock(sw_mutex_);
    if (sw_shader_ids_.find(pExportName) != sw_shader_ids_.end() || sw_allow_dynamic_exports_) {
        return 256ull;
    }
    return 0ull;
}

UINT64 STDMETHODCALLTYPE StateObjectXProxy::GetPipelineStackSize() {
    if (host_props_) return host_props_->GetPipelineStackSize();
    return sw_pipeline_stack_size_.load(std::memory_order_relaxed);
}

void STDMETHODCALLTYPE StateObjectXProxy::SetPipelineStackSize(UINT64 PipelineStackSizeInBytes) {
    if (host_props_) {
        host_props_->SetPipelineStackSize(PipelineStackSizeInBytes);
    } else {
        sw_pipeline_stack_size_.store(PipelineStackSizeInBytes, std::memory_order_relaxed);
    }
}

namespace {

class SimpleZeroBlob final : public ID3DBlob {
public:
    explicit SimpleZeroBlob(bool owned_singleton) : owned_singleton_(owned_singleton) {
        std::memset(buf_, 0, sizeof(buf_));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) || IsEqualIID(riid, __uuidof(ID3DBlob))) {
            *ppvObject = static_cast<ID3DBlob*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        if (owned_singleton_) return 1u;
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        if (owned_singleton_) return 1u;
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return buf_; }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return sizeof(buf_); }

private:
    std::atomic<ULONG> ref_count_{1u};
    bool owned_singleton_ = false;
    uint8_t buf_[128]{};
};

}  // namespace

HRESULT STDMETHODCALLTYPE StateObjectXProxy::GetPipelineReflectionX(void** ppOutReflection) {
    if (!ppOutReflection) return E_POINTER;
    *ppOutReflection = static_cast<ID3D12StateObjectXbox*>(this);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::GetReflectionBlobX(
    UINT /*kind*/, UINT /*flags*/, ID3DBlob** ppOutBlob) {
    if (!ppOutBlob) return E_POINTER;
    auto* blob = new (std::nothrow) SimpleZeroBlob(false);
    if (!blob) {
        *ppOutBlob = nullptr;
        return E_OUTOFMEMORY;
    }
    *ppOutBlob = blob;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE StateObjectXProxy::GetPropertyX(UINT propertyId, void* pOut16Bytes) {
    if (!pOut16Bytes) return E_POINTER;
    std::memset(pOut16Bytes, 0, 16u);
    if (propertyId == 0u) {
        auto* u32 = static_cast<uint32_t*>(pOut16Bytes);
        u32[0] = 256u;
        u32[1] = 0u;
        return S_OK;
    }
    if (propertyId == 1u) {
        static SimpleZeroBlob s_owned_blob(true);
        ID3DBlob* blob_ptr = &s_owned_blob;
        std::memcpy(pOut16Bytes, &blob_ptr, sizeof(blob_ptr));
        return S_OK;
    }
    return S_OK;
}

}  // namespace openscarlett::d3d12_xs
