#include "command_list_x.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>
#include <unordered_map>

#include "device_x.h"
#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/xg_gpu_untile.h"

namespace openscarlett {

// Placed tiled texture 64KB tile-pool helpers defined in src/gpu/xg_gpu_untile.cpp.
struct XgPlacedTiledSubresourceDesc {
    uint32_t subresource_index = 0u;
    uint32_t mip_level = 0u;
    uint32_t array_slice = 0u;
    uint32_t width_elements = 0u;
    uint32_t height_elements = 0u;
    uint32_t depth_elements = 1u;
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    uint32_t tiles_z = 1u;
    uint32_t tile_size_bytes = 65536u;
    uint32_t bytes_per_element = 4u;
    uint32_t linear_row_pitch_bytes = 256u;
    uint64_t subresource_offset_bytes = 0ull;
    uint64_t heap_subresource_offset_bytes = 0ull;
    uint64_t subresource_size_bytes = 0ull;
    uint32_t tile_pool_start_index_64kb = 0u;
    uint32_t tile_pool_count_64kb = 0u;
};

struct XgPlacedTilePoolEntry {
    uint32_t logical_tile_index = 0u;
    uint32_t heap_tile_index_64kb = 0u;
    uint64_t pool_byte_offset = 0ull;
    bool mapped = true;
};

bool ComputePlacedTiledSubresourceLayout2D(
    const XG_TEXTURE2D_DESC& desc,
    uint64_t placed_heap_offset_bytes,
    uint32_t subresource_index,
    XgPlacedTiledSubresourceDesc& out_sub) noexcept;

bool GatherPlacedTiledPool64KB(
    const void* src_pool_base,
    uint64_t src_pool_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    std::vector<uint8_t>& out_contiguous_tiled);

bool ScatterPlacedTiledPool64KB(
    const void* src_contiguous_tiled,
    uint64_t src_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    void* dst_pool_base,
    uint64_t dst_pool_size_bytes);

}  // namespace openscarlett

namespace openscarlett::d3d12_xs {

namespace {

constexpr const char* kModule = "d3d12_xs.dll";

constexpr uint64_t AlignUpPlaced64(uint64_t v, uint64_t a) noexcept {
    return (a <= 1ull) ? v : ((v + a - 1ull) & ~(a - 1ull));
}

constexpr uint32_t AlignUpPlaced32(uint32_t v, uint32_t a) noexcept {
    return (a <= 1u) ? v : ((v + a - 1u) & ~(a - 1u));
}

struct PlacedTiledTileKey {
    uint32_t subresource = 0u;
    uint32_t tx = 0u;
    uint32_t ty = 0u;
    uint32_t tz = 0u;

    bool operator==(const PlacedTiledTileKey& o) const noexcept {
        return subresource == o.subresource && tx == o.tx && ty == o.ty && tz == o.tz;
    }
};

struct PlacedTiledTileKeyHash {
    size_t operator()(const PlacedTiledTileKey& k) const noexcept {
        uint64_t h = static_cast<uint64_t>(k.subresource) * 1469598103934665603ull;
        h ^= static_cast<uint64_t>(k.tx) * 1099511628211ull;
        h ^= (static_cast<uint64_t>(k.ty) << 17u) ^ (static_cast<uint64_t>(k.tz) << 33u);
        return static_cast<size_t>(h);
    }
};

struct PlacedTiledSubresourceRecord {
    D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH pitch{};
    bool dirty_for_flush = false;
};

struct PlacedTiledResourceRecord {
    ID3D12Resource* host_resource = nullptr;
    ID3D12Heap* host_heap = nullptr;
    D3D12_RESOURCE_DESC res_desc{};
    XG_TEXTURE2D_DESC xg2d_desc{};
    XG_RESOURCE_LAYOUT xg_layout{};
    uint64_t heap_offset_bytes = 0ull;
    uint64_t tile_pool_base_offset_bytes = 0ull;
    XG_TILE_MODE tile_mode = XG_TILE_MODE_LINEAR;
    D3D12_RESOURCE_STATES all_sub_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t total_64kb_tiles = 0u;
    std::vector<PlacedTiledSubresourceRecord> subresources;
    std::unordered_map<PlacedTiledTileKey, D3D12XBOX_TILED_PLACED_TILE_MAPPING, PlacedTiledTileKeyHash> tile_mappings;
    bool has_custom_tile_mapping = false;
    uint64_t untile_copy_count = 0ull;
    uint64_t retile_copy_count = 0ull;
    uint64_t copy_resource_bridge_count = 0ull;
    uint64_t copy_tiles_bridge_count = 0ull;
    uint64_t barrier_transition_count = 0ull;
    uint64_t aliasing_barrier_count = 0ull;
    uint64_t coherency_flush_count = 0ull;
    bool used_gpu_untile_bridge = false;
};

struct PlacedTiledRegistry {
    std::mutex mutex;
    std::unordered_map<ID3D12Resource*, PlacedTiledResourceRecord> map;
};

PlacedTiledRegistry& GetPlacedTiledRegistry() {
    static PlacedTiledRegistry reg;
    return reg;
}

inline void RecordList(const char* method, int32_t slot, HRESULT hr = S_OK) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == E_NOTIMPL || hr == E_NOINTERFACE) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (FAILED(hr)) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(
        kModule,
        method,
        outcome,
        static_cast<int32_t>(hr),
        slot);
}

struct CommandListRegistry {
    std::shared_mutex mutex;
    std::unordered_map<const void*, ID3D12GraphicsCommandList*> map;
    std::unordered_map<const void*, CommandListX*> wrapper_map;
};

CommandListRegistry& GetCommandListRegistry() {
    static CommandListRegistry reg;
    return reg;
}

}  // namespace

#if defined(__ID3D12GraphicsCommandList10_INTERFACE_DEFINED__)
using CommandListExtendedBase = ID3D12GraphicsCommandList10;
#elif defined(__ID3D12GraphicsCommandList9_INTERFACE_DEFINED__)
using CommandListExtendedBase = ID3D12GraphicsCommandList9;
#elif defined(__ID3D12GraphicsCommandList8_INTERFACE_DEFINED__)
using CommandListExtendedBase = ID3D12GraphicsCommandList8;
#elif defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
using CommandListExtendedBase = ID3D12GraphicsCommandList7;
#endif

#if defined(__ID3D12GraphicsCommandList7_INTERFACE_DEFINED__)
class CommandListExtendedProxy final : public CommandListExtendedBase {
public:
    explicit CommandListExtendedProxy(CommandListX* parent) : parent_(parent) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override { return parent_->QueryInterface(riid, ppvObject); }
    ULONG STDMETHODCALLTYPE AddRef() override { return parent_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return parent_->Release(); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { return parent_->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { return parent_->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) override { return parent_->SetPrivateDataInterface(g, d); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return parent_->SetName(n); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** d) override { return parent_->GetDevice(r, d); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return parent_->GetType(); }
    HRESULT STDMETHODCALLTYPE Close() override { return parent_->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* s) override { return parent_->Reset(a, s); }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* s) override { parent_->ClearState(s); }
    void STDMETHODCALLTYPE DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si) override { parent_->DrawInstanced(vc, ic, sv, si); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT ix, UINT ic, UINT si, INT bv, UINT st) override { parent_->DrawIndexedInstanced(ix, ic, si, bv, st); }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override { parent_->Dispatch(x, y, z); }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT64 nb) override { parent_->CopyBufferRegion(d, dof, s, sof, nb); }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) override { parent_->CopyTextureRegion(d, x, y, z, s, b); }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* d, ID3D12Resource* s) override { parent_->CopyResource(d, s); }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f) override { parent_->CopyTiles(r, c, sz, b, off, f); }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) override { parent_->ResolveSubresource(d, ds, s, ss, f); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) override { parent_->IASetPrimitiveTopology(t); }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* v) override { parent_->RSSetViewports(n, v); }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* r) override { parent_->RSSetScissorRects(n, r); }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT b[4]) override { parent_->OMSetBlendFactor(b); }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT r) override { parent_->OMSetStencilRef(r); }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* p) override { parent_->SetPipelineState(p); }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) override { parent_->ResourceBarrier(n, b); }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* l) override { parent_->ExecuteBundle(l); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) override { parent_->SetDescriptorHeaps(n, h); }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* r) override { parent_->SetComputeRootSignature(r); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* r) override { parent_->SetGraphicsRootSignature(r); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override { parent_->SetComputeRootDescriptorTable(i, h); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override { parent_->SetGraphicsRootDescriptorTable(i, h); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) override { parent_->SetComputeRoot32BitConstant(i, v, o); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) override { parent_->SetGraphicsRoot32BitConstant(i, v, o); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) override { parent_->SetComputeRoot32BitConstants(i, n, d, o); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) override { parent_->SetGraphicsRoot32BitConstants(i, n, d, o); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetComputeRootConstantBufferView(i, a); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetGraphicsRootConstantBufferView(i, a); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetComputeRootShaderResourceView(i, a); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetGraphicsRootShaderResourceView(i, a); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetComputeRootUnorderedAccessView(i, a); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override { parent_->SetGraphicsRootUnorderedAccessView(i, a); }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) override { parent_->IASetIndexBuffer(v); }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) override { parent_->IASetVertexBuffers(s, n, v); }
    void STDMETHODCALLTYPE SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) override { parent_->SOSetTargets(s, n, v); }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* r, BOOL s, const D3D12_CPU_DESCRIPTOR_HANDLE* d) override { parent_->OMSetRenderTargets(n, r, s, d); }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* r) override { parent_->ClearDepthStencilView(h, f, d, s, n, r); }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT n, const D3D12_RECT* r) override { parent_->ClearRenderTargetView(h, c, n, r); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const UINT v[4], UINT n, const D3D12_RECT* r) override { parent_->ClearUnorderedAccessViewUint(g, c, res, v, n, r); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const FLOAT v[4], UINT n, const D3D12_RECT* r) override { parent_->ClearUnorderedAccessViewFloat(g, c, res, v, n, r); }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* res, const D3D12_DISCARD_REGION* r) override { parent_->DiscardResource(res, r); }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) override { parent_->BeginQuery(h, t, i); }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) override { parent_->EndQuery(h, t, i); }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource* d, UINT64 o) override { parent_->ResolveQueryData(h, t, s, n, d, o); }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource* b, UINT64 o, D3D12_PREDICATION_OP op) override { parent_->SetPredication(b, o, op); }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void* d, UINT s) override { parent_->SetMarker(m, d, s); }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void* d, UINT s) override { parent_->BeginEvent(m, d, s); }
    void STDMETHODCALLTYPE EndEvent() override { parent_->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* cs, UINT mc, ID3D12Resource* ab, UINT64 ao, ID3D12Resource* cb, UINT64 co) override { parent_->ExecuteIndirect(cs, mc, ab, ao, cb, co); }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT dep, ID3D12Resource* const* dr, const D3D12_SUBRESOURCE_RANGE_UINT64* sr) override { parent_->AtomicCopyBufferUINT(d, dof, s, sof, dep, dr, sr); }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT dep, ID3D12Resource* const* dr, const D3D12_SUBRESOURCE_RANGE_UINT64* sr) override { parent_->AtomicCopyBufferUINT64(d, dof, s, sof, dep, dr, sr); }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT mn, FLOAT mx) override { parent_->OMSetDepthBounds(mn, mx); }
    void STDMETHODCALLTYPE SetSamplePositions(UINT ns, UINT np, D3D12_SAMPLE_POSITION* sp) override { parent_->SetSamplePositions(ns, np, sp); }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource* d, UINT ds, UINT x, UINT y, ID3D12Resource* s, UINT ss, D3D12_RECT* sr, DXGI_FORMAT f, D3D12_RESOLVE_MODE rm) override { parent_->ResolveSubresourceRegion(d, ds, x, y, s, ss, sr, f, rm); }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT m) override { parent_->SetViewInstanceMask(m); }
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT c, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p, const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) override { parent_->WriteBufferImmediate(c, p, m); }
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession* ps) override { parent_->SetProtectedResourceSession(ps); }
    void STDMETHODCALLTYPE BeginRenderPass(UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* r, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* d, D3D12_RENDER_PASS_FLAGS f) override { parent_->BeginRenderPass(n, r, d, f); }
    void STDMETHODCALLTYPE EndRenderPass() override { parent_->EndRenderPass(); }
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand* mc, const void* d, SIZE_T s) override { parent_->InitializeMetaCommand(mc, d, s); }
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand* mc, const void* d, SIZE_T s) override { parent_->ExecuteMetaCommand(mc, d, s); }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) override { parent_->BuildRaytracingAccelerationStructure(d, n, p); }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* s) override { parent_->EmitRaytracingAccelerationStructurePostbuildInfo(d, n, s); }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) override { parent_->CopyRaytracingAccelerationStructure(d, s, m); }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* so) override { parent_->SetPipelineState1(so); }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) override { parent_->DispatchRays(d); }
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE r, const D3D12_SHADING_RATE_COMBINER* c) override { parent_->RSSetShadingRate(r, c); }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource* r) override { parent_->RSSetShadingRateImage(r); }
    void STDMETHODCALLTYPE DispatchMesh(UINT x, UINT y, UINT z) override { parent_->DispatchMesh(x, y, z); }

    void STDMETHODCALLTYPE Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP* pBarrierGroups) override {
        if (parent_->host_list7_) {
            parent_->host_list7_->Barrier(NumBarrierGroups, pBarrierGroups);
        }
        RecordList("ID3D12GraphicsCommandListX::Barrier", 80, parent_->host_list7_ ? S_OK : E_NOINTERFACE);
    }
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) override {
        if (parent_->host_list8_) {
            parent_->host_list8_->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);
        }
        RecordList("ID3D12GraphicsCommandListX::OMSetFrontAndBackStencilRef", 81, parent_->host_list8_ ? S_OK : E_NOINTERFACE);
    }
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) override {
        if (parent_->host_list9_) {
            parent_->host_list9_->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
        }
        RecordList("ID3D12GraphicsCommandListX::RSSetDepthBias", 82, parent_->host_list9_ ? S_OK : E_NOINTERFACE);
    }
    void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) override {
        if (parent_->host_list9_) {
            parent_->host_list9_->IASetIndexBufferStripCutValue(IBStripCutValue);
        }
        RecordList("ID3D12GraphicsCommandListX::IASetIndexBufferStripCutValue", 83, parent_->host_list9_ ? S_OK : E_NOINTERFACE);
    }
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC* pDesc) override {
        if (parent_->host_list10_) {
            parent_->host_list10_->SetProgram(pDesc);
        }
        RecordList("ID3D12GraphicsCommandListX::SetProgram", 84, parent_->host_list10_ ? S_OK : E_NOINTERFACE);
    }
    void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC* pDesc) override {
        if (parent_->host_list10_) {
            parent_->host_list10_->DispatchGraph(pDesc);
        }
        RecordList("ID3D12GraphicsCommandListX::DispatchGraph", 85, parent_->host_list10_ ? S_OK : E_NOINTERFACE);
    }
#endif

private:
    CommandListX* parent_ = nullptr;
};
#endif  // __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__

CommandListX::CommandListX(DeviceX* parent_device, ID3D12GraphicsCommandList* host_list)
    : device_x_(parent_device), host_list_(host_list) {
    if (device_x_) device_x_->AddRef();
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    ext_proxy_ = new (std::nothrow) CommandListExtendedProxy(this);
#endif
    if (host_list_) {
        host_list_->AddRef();
        list_type_ = host_list_->GetType();
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList1), reinterpret_cast<void**>(&host_list1_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&host_list2_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList3), reinterpret_cast<void**>(&host_list3_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&host_list4_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList5), reinterpret_cast<void**>(&host_list5_));
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList6), reinterpret_cast<void**>(&host_list6_));
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList7), reinterpret_cast<void**>(&host_list7_));
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList8), reinterpret_cast<void**>(&host_list8_));
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList9), reinterpret_cast<void**>(&host_list9_));
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
        host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList10), reinterpret_cast<void**>(&host_list10_));
#endif
    }
    auto& reg = GetCommandListRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map[static_cast<const ID3D12CommandList*>(this)] = host_list_;
    reg.wrapper_map[static_cast<const ID3D12CommandList*>(this)] = this;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    if (ext_proxy_) {
        reg.map[static_cast<const ID3D12CommandList*>(ext_proxy_)] = host_list_;
        reg.wrapper_map[static_cast<const ID3D12CommandList*>(ext_proxy_)] = this;
    }
#endif
}

CommandListX::~CommandListX() {
    ReleaseTiledTransientResources();
    {
        auto& reg = GetCommandListRegistry();
        std::unique_lock<std::shared_mutex> lock(reg.mutex);
        reg.map.erase(static_cast<const ID3D12CommandList*>(this));
        reg.wrapper_map.erase(static_cast<const ID3D12CommandList*>(this));
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
        if (ext_proxy_) {
            reg.map.erase(static_cast<const ID3D12CommandList*>(ext_proxy_));
            reg.wrapper_map.erase(static_cast<const ID3D12CommandList*>(ext_proxy_));
        }
#endif
    }
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    delete ext_proxy_;
    ext_proxy_ = nullptr;
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    if (host_list10_) host_list10_->Release();
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    if (host_list9_) host_list9_->Release();
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    if (host_list8_) host_list8_->Release();
#endif
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    if (host_list7_) host_list7_->Release();
#endif
    if (host_list6_) host_list6_->Release();
    if (host_list5_) host_list5_->Release();
    if (host_list4_) host_list4_->Release();
    if (host_list3_) host_list3_->Release();
    if (host_list2_) host_list2_->Release();
    if (host_list1_) host_list1_->Release();
    if (host_list_) host_list_->Release();
    if (device_x_) device_x_->Release();
}

CommandListX* CommandListX::TryFromCommandList(ID3D12CommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.wrapper_map.find(static_cast<const void*>(maybe_wrapped));
    return (it != reg.wrapper_map.end()) ? it->second : nullptr;
}

void CommandListX::CopyVulkanRecordedBatch(VulkanRecordedBatch& out_batch) const {
    std::lock_guard<std::mutex> lock(vk_rec_mutex_);
    out_batch = vk_batch_;
}

ID3D12CommandList* CommandListX::UnwrapHostList(ID3D12CommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(maybe_wrapped));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

ID3D12GraphicsCommandList* CommandListX::UnwrapHostGraphicsList(
    ID3D12GraphicsCommandList* maybe_wrapped) noexcept {
    if (!maybe_wrapped) return nullptr;
    auto& reg = GetCommandListRegistry();
    std::shared_lock<std::shared_mutex> lock(reg.mutex);
    auto it = reg.map.find(static_cast<const void*>(static_cast<ID3D12CommandList*>(maybe_wrapped)));
    if (it != reg.map.end()) {
        return it->second;
    }
    return maybe_wrapped;
}

// IUnknown (slots 0..2)
HRESULT STDMETHODCALLTYPE CommandListX::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) {
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, E_POINTER);
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (IsEqualIID(riid, OS_D3D12_IID(IUnknown)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12Object)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12DeviceChild)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12CommandList)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList1)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList2)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList3)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList4)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList5)) ||
        IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList6)) ||
        IsEqualIID(riid, IID_ID3D12GraphicsCommandListX)) {
        *ppvObject = static_cast<ID3D12GraphicsCommandListX*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettCommandListExtState)) {
        *ppvObject = static_cast<IOpenScarlettCommandListExtState*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
    if (IsEqualIID(riid, IID_IOpenScarlettTiledPlacedManager)) {
        *ppvObject = static_cast<IOpenScarlettTiledPlacedManager*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList7)) && host_list7_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList7*>(ext_proxy_);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList8)) && host_list8_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList8*>(ext_proxy_);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList9)) && host_list9_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList9*>(ext_proxy_);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList10)) && host_list10_ && ext_proxy_) {
        *ppvObject = static_cast<ID3D12GraphicsCommandList10*>(ext_proxy_);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#endif

    RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, E_NOINTERFACE);
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CommandListX::AddRef() {
    const ULONG refs = ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    RecordList("ID3D12GraphicsCommandListX::AddRef", 1, S_OK);
    return refs;
}

ULONG STDMETHODCALLTYPE CommandListX::Release() {
    RecordList("ID3D12GraphicsCommandListX::Release", 2, S_OK);
    const ULONG rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        delete this;
    }
    return rem;
}

// ID3D12Object (slots 3..6)
HRESULT STDMETHODCALLTYPE CommandListX::GetPrivateData(
    REFGUID guid, UINT* pDataSize, void* pData) {
    const HRESULT hr = host_list_->GetPrivateData(guid, pDataSize, pData);
    RecordList("ID3D12GraphicsCommandListX::GetPrivateData", 3, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetPrivateData(
    REFGUID guid, UINT DataSize, const void* pData) {
    const HRESULT hr = host_list_->SetPrivateData(guid, DataSize, pData);
    RecordList("ID3D12GraphicsCommandListX::SetPrivateData", 4, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetPrivateDataInterface(
    REFGUID guid, const IUnknown* pData) {
    const HRESULT hr = host_list_->SetPrivateDataInterface(guid, pData);
    RecordList("ID3D12GraphicsCommandListX::SetPrivateDataInterface", 5, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::SetName(LPCWSTR Name) {
    const HRESULT hr = host_list_->SetName(Name);
    RecordList("ID3D12GraphicsCommandListX::SetName", 6, hr);
    return hr;
}

// ID3D12DeviceChild (slot 7)
HRESULT STDMETHODCALLTYPE CommandListX::GetDevice(REFIID riid, void** ppvDevice) {
    const HRESULT hr = device_x_ ? device_x_->QueryInterface(riid, ppvDevice) : E_FAIL;
    RecordList("ID3D12GraphicsCommandListX::GetDevice", 7, hr);
    return hr;
}

// ID3D12CommandList (slot 8)
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE CommandListX::GetType() {
    const D3D12_COMMAND_LIST_TYPE t = host_list_->GetType();
    RecordList("ID3D12GraphicsCommandListX::GetType", 8, S_OK);
    return t;
}

// ID3D12GraphicsCommandList (slots 9..59)
HRESULT STDMETHODCALLTYPE CommandListX::Close() {
    const HRESULT hr = host_list_->Close();
    RecordList("ID3D12GraphicsCommandListX::Close", 9, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CommandListX::Reset(
    ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) {
    ReleaseTiledTransientResources();
    depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bias_clamp_.store(0.0f, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_min_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_max_.store(1.0f, std::memory_order_relaxed);
    view_instance_mask_.store(0u, std::memory_order_relaxed);
    last_host_compute_rs_ = nullptr;
    last_host_pso_ = UnwrapPipelineState(pInitialState);
    bound_cbv_srv_uav_heap_ = nullptr;
    bound_sampler_heap_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        written_mirrored_vas_.clear();
        deferred_cpu_writes_.clear();
        deferred_cpu_waits_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = 0u;
        sample_positions_pixels_ = 0u;
        sample_positions_.clear();
    }
    ID3D12PipelineState* host_pso = UnwrapPipelineState(pInitialState);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pInitialState);
        if (is_spirv) host_pso = nullptr;
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_batch_ = VulkanRecordedBatch{};
        vk_cur_pso_ = pInitialState;
        vk_cur_pso_is_spirv_ = is_spirv;
        vk_cur_rtv_ = nullptr;
        vk_cur_dsv_ = nullptr;
        vk_has_clear_color_ = false;
        vk_clear_color_[0] = vk_clear_color_[1] = vk_clear_color_[2] = 0.0f;
        vk_clear_color_[3] = 1.0f;
        vk_clear_depth_ = 1.0f;
        vk_vp_width_ = 0u;
        vk_vp_height_ = 0u;
        vk_has_vbv_ = false;
        vk_has_ibv_ = false;
        vk_gfx_root_constants_.clear();
        vk_cmp_root_constants_.clear();
        vk_gfx_cbvs_.clear();
        vk_gfx_srvs_.clear();
        vk_gfx_uavs_.clear();
        vk_cmp_cbvs_.clear();
        vk_cmp_srvs_.clear();
        vk_cmp_uavs_.clear();
    }
    const HRESULT hr = host_list_->Reset(UnwrapCommandAllocator(pAllocator), host_pso);
    RecordList("ID3D12GraphicsCommandListX::Reset", 10, hr);
    return hr;
}

void STDMETHODCALLTYPE CommandListX::ClearState(ID3D12PipelineState* pPipelineState) {
    depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bias_clamp_.store(0.0f, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_min_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_max_.store(1.0f, std::memory_order_relaxed);
    view_instance_mask_.store(0u, std::memory_order_relaxed);
    last_host_compute_rs_ = nullptr;
    last_host_pso_ = UnwrapPipelineState(pPipelineState);
    bound_cbv_srv_uav_heap_ = nullptr;
    bound_sampler_heap_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = 0u;
        sample_positions_pixels_ = 0u;
        sample_positions_.clear();
    }
    ID3D12PipelineState* host_pso = UnwrapPipelineState(pPipelineState);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pPipelineState);
        if (is_spirv) host_pso = nullptr;
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_pso_ = pPipelineState;
        vk_cur_pso_is_spirv_ = is_spirv;
        vk_cur_rtv_ = nullptr;
        vk_cur_dsv_ = nullptr;
        vk_has_vbv_ = false;
        vk_has_ibv_ = false;
        vk_gfx_root_constants_.clear();
        vk_cmp_root_constants_.clear();
        vk_gfx_cbvs_.clear();
        vk_gfx_srvs_.clear();
        vk_gfx_uavs_.clear();
        vk_cmp_cbvs_.clear();
        vk_cmp_srvs_.clear();
        vk_cmp_uavs_.clear();
    }
    host_list_->ClearState(host_pso);
    RecordList("ID3D12GraphicsCommandListX::ClearState", 11, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DrawInstanced;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = VertexCountPerInstance;
        op.y = InstanceCount;
        op.start_location = StartVertexLocation;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.vbv = vk_cur_vbv_;
        op.has_vbv = vk_has_vbv_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->DrawInstanced(
            VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    RecordList("ID3D12GraphicsCommandListX::DrawInstanced", 12, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DrawIndexedInstanced;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = IndexCountPerInstance;
        op.y = InstanceCount;
        op.start_location = StartIndexLocation;
        op.base_vertex = BaseVertexLocation;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.vbv = vk_cur_vbv_;
        op.has_vbv = vk_has_vbv_;
        op.ibv = vk_cur_ibv_;
        op.has_ibv = vk_has_ibv_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->DrawIndexedInstanced(
            IndexCountPerInstance, InstanceCount, StartIndexLocation,
            BaseVertexLocation, StartInstanceLocation);
    }
    RecordList("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::Dispatch;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = ThreadGroupCountX;
        op.y = ThreadGroupCountY;
        op.z = ThreadGroupCountZ;
        op.root_constants = vk_cmp_root_constants_;
        op.root_cbvs = vk_cmp_cbvs_;
        op.root_srvs = vk_cmp_srvs_;
        op.root_uavs = vk_cmp_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    if (!skip_host) {
        host_list_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    RecordList("ID3D12GraphicsCommandListX::Dispatch", 14, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyBufferRegion(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) {
    ID3D12Resource* host_dst = UnwrapResource(pDstBuffer);
    ID3D12Resource* host_src = UnwrapResource(pSrcBuffer);
    RecordMirroredResourceGpuWrite(host_dst);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyBuffer;
        op.dst_resource = host_dst;
        op.src_resource = host_src;
        op.dst_offset = DstOffset;
        op.src_offset = SrcOffset;
        op.num_bytes = NumBytes;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->CopyBufferRegion(host_dst, DstOffset, host_src, SrcOffset, NumBytes);
    RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
    const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    const D3D12_TEXTURE_COPY_LOCATION* dst_ptr = pDst;
    const D3D12_TEXTURE_COPY_LOCATION* src_ptr = pSrc;
    if (pDst) {
        dst_loc = *pDst;
        dst_loc.pResource = UnwrapResource(pDst->pResource);
        dst_ptr = &dst_loc;
    }
    if (pSrc) {
        src_loc = *pSrc;
        src_loc.pResource = UnwrapResource(pSrc->pResource);
        src_ptr = &src_loc;
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyTexture;
        op.dst_resource = dst_ptr ? dst_ptr->pResource : nullptr;
        op.src_resource = src_ptr ? src_ptr->pResource : nullptr;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }

    // Sprint 13 placed tiled texture bridge (64KB tile-pool mapping, subresource pitch, untile/retile).
    if (dst_ptr && src_ptr &&
        TryBridgePlacedTiledCopyTextureRegion(*dst_ptr, DstX, DstY, DstZ, *src_ptr, pSrcBox)) {
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[PlacedTiled]", 16, S_OK);
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
        return;
    }

    // Live XgGpuUntiler intercept when source or destination uses a GFX10 swizzle layout.
    if (device_x_ && dst_ptr && src_ptr &&
        dst_ptr->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        const XG_TILE_MODE src_mode = device_x_->GetTrackedResourceTileMode(src_ptr->pResource);
        const XG_TILE_MODE dst_mode = device_x_->GetTrackedResourceTileMode(dst_ptr->pResource);
        const XG_TILE_MODE active_mode = (src_mode != XG_TILE_MODE_LINEAR) ? src_mode : dst_mode;
        if (active_mode != XG_TILE_MODE_LINEAR) {
            if (SUCCEEDED(device_x_->RecordUntileUploadOnCommandList(
                    host_list_, list_type_, *dst_ptr, DstX, DstY, DstZ, *src_ptr, pSrcBox, active_mode))) {
                if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
                if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);
                RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[XgGpuUntile]", 16, S_OK);
                RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
                return;
            }
        }
    }

    host_list_->CopyTextureRegion(dst_ptr, DstX, DstY, DstZ, src_ptr, pSrcBox);
    RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyResource(
    ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) {
    ID3D12Resource* host_dst = UnwrapResource(pDstResource);
    ID3D12Resource* host_src = UnwrapResource(pSrcResource);
    RecordMirroredResourceGpuWrite(host_dst);
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyResource;
        op.dst_resource = host_dst;
        op.src_resource = host_src;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    if (TryBridgePlacedTiledCopyResource(host_dst, host_src)) {
        RecordList("ID3D12GraphicsCommandListX::CopyResource[PlacedTiled]", 17, S_OK);
        RecordList("ID3D12GraphicsCommandListX::CopyResource", 17, S_OK);
        return;
    }
    host_list_->CopyResource(host_dst, host_src);
    RecordList("ID3D12GraphicsCommandListX::CopyResource", 17, S_OK);
}

void STDMETHODCALLTYPE CommandListX::CopyTiles(
    ID3D12Resource* pTiledResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pTileRegionSize,
    ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes,
    D3D12_TILE_COPY_FLAGS Flags) {
    ID3D12Resource* host_tiled = UnwrapResource(pTiledResource);
    ID3D12Resource* host_buf = UnwrapResource(pBuffer);
    if (TryBridgePlacedTiledCopyTiles(
            host_tiled, pTileRegionStartCoordinate, pTileRegionSize,
            host_buf, BufferStartOffsetInBytes, Flags)) {
        RecordList("ID3D12GraphicsCommandListX::CopyTiles[PlacedTiled]", 18, S_OK);
        RecordList("ID3D12GraphicsCommandListX::CopyTiles", 18, S_OK);
        return;
    }
    host_list_->CopyTiles(
        host_tiled, pTileRegionStartCoordinate, pTileRegionSize,
        host_buf, BufferStartOffsetInBytes, Flags);
    RecordList("ID3D12GraphicsCommandListX::CopyTiles", 18, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresource(
    ID3D12Resource* pDstResource, UINT DstSubresource,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) {
    host_list_->ResolveSubresource(
        UnwrapResource(pDstResource), DstSubresource,
        UnwrapResource(pSrcResource), SrcSubresource, Format);
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresource", 19, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) {
    host_list_->IASetPrimitiveTopology(PrimitiveTopology);
    RecordList("ID3D12GraphicsCommandListX::IASetPrimitiveTopology", 20, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetViewports(
    UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && NumViewports > 0u && pViewports) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_vp_width_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Width));
        vk_vp_height_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Height));
    }
    host_list_->RSSetViewports(NumViewports, pViewports);
    RecordList("ID3D12GraphicsCommandListX::RSSetViewports", 21, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetScissorRects(
    UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->RSSetScissorRects(NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::RSSetScissorRects", 22, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetBlendFactor(const FLOAT BlendFactor[4]) {
    host_list_->OMSetBlendFactor(BlendFactor);
    RecordList("ID3D12GraphicsCommandListX::OMSetBlendFactor", 23, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetStencilRef(UINT StencilRef) {
    host_list_->OMSetStencilRef(StencilRef);
    RecordList("ID3D12GraphicsCommandListX::OMSetStencilRef", 24, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPipelineState(ID3D12PipelineState* pPipelineState) {
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        const bool is_spirv = device_x_->IsSpirvDirectPipeline(pPipelineState);
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_pso_ = pPipelineState;
        vk_cur_pso_is_spirv_ = is_spirv;
        skip_host = is_spirv;
    }
    ID3D12PipelineState* host_pso = UnwrapPipelineState(pPipelineState);
    last_host_pso_ = host_pso;
    if (!skip_host) {
        host_list_->SetPipelineState(host_pso);
    }
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState", 25, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResourceBarrier(
    UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (!pBarriers || NumBarriers == 0u) {
        host_list_->ResourceBarrier(NumBarriers, pBarriers);
        RecordList("ID3D12GraphicsCommandListX::ResourceBarrier", 26, S_OK);
        return;
    }

    constexpr UINT kStackBarriers = 16u;
    D3D12_RESOURCE_BARRIER stack_buf[kStackBarriers];
    std::vector<D3D12_RESOURCE_BARRIER> heap_buf;
    D3D12_RESOURCE_BARRIER* unwrapped = stack_buf;
    if (NumBarriers > kStackBarriers) {
        heap_buf.resize(NumBarriers);
        unwrapped = heap_buf.data();
    }

    const bool track_unified =
        device_x_ && (device_x_->HasRegisteredUnifiedTextures() ||
                      device_x_->HasMirroredTitleVaRegions());
    auto remap_fallback_fb_state = [](D3D12_RESOURCE_STATES st) noexcept -> D3D12_RESOURCE_STATES {
        if (st == D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        if (st == D3D12_RESOURCE_STATE_RESOLVE_DEST) {
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        return st;
    };

    UINT out_count = 0u;
    for (UINT i = 0u; i < NumBarriers; ++i) {
        D3D12_RESOURCE_BARRIER b = pBarriers[i];
        bool emit = true;
        switch (b.Type) {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
                b.Transition.pResource = UnwrapResource(pBarriers[i].Transition.pResource);
                if (device_x_ && b.Transition.pResource != nullptr) {
                    SamplerFeedbackMapInfo sf_info{};
                    if (device_x_->GetSamplerFeedbackManager().LookupFeedbackMap(
                            b.Transition.pResource, sf_info) &&
                        !sf_info.using_native_hardware) {
                        b.Transition.StateBefore = remap_fallback_fb_state(b.Transition.StateBefore);
                        b.Transition.StateAfter = remap_fallback_fb_state(b.Transition.StateAfter);
                        if (b.Transition.StateBefore == b.Transition.StateAfter) {
                            if (b.Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                                ID3D12Resource* fb_res = b.Transition.pResource;
                                std::memset(&b, 0, sizeof(b));
                                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                                b.UAV.pResource = fb_res;
                            } else {
                                emit = false;
                            }
                        }
                    }
                }
                if (emit && b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                    track_unified && b.Transition.pResource != nullptr) {
                    device_x_->NotifyResourceStateTransition(
                        b.Transition.pResource,
                        b.Transition.StateAfter);
                    if ((b.Transition.StateAfter &
                         (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST)) != 0 ||
                        (b.Transition.StateBefore &
                         (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST)) != 0) {
                        RecordMirroredResourceGpuWrite(b.Transition.pResource);
                    }
                }
                break;
            }
            case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                b.Aliasing.pResourceBefore = UnwrapResource(pBarriers[i].Aliasing.pResourceBefore);
                b.Aliasing.pResourceAfter = UnwrapResource(pBarriers[i].Aliasing.pResourceAfter);
                break;
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                b.UAV.pResource = UnwrapResource(pBarriers[i].UAV.pResource);
                if (b.UAV.pResource != nullptr) {
                    RecordMirroredResourceGpuWrite(b.UAV.pResource);
                }
                break;
            default:
                break;
        }
        if (emit) {
            unwrapped[out_count++] = b;
        }
    }

    std::vector<D3D12_RESOURCE_BARRIER> extra_tiled_barriers;
    if (out_count > 0u) {
        TrackPlacedTiledResourceBarriers(out_count, unwrapped, extra_tiled_barriers);
    }

    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::Barrier;
        op.barrier_count = out_count + static_cast<uint32_t>(extra_tiled_barriers.size());
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    if (!extra_tiled_barriers.empty()) {
        host_list_->ResourceBarrier(
            static_cast<UINT>(extra_tiled_barriers.size()), extra_tiled_barriers.data());
    }
    if (out_count > 0u) {
        host_list_->ResourceBarrier(out_count, unwrapped);
    }
    RecordList("ID3D12GraphicsCommandListX::ResourceBarrier", 26, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    host_list_->ExecuteBundle(UnwrapHostGraphicsList(pCommandList));
    RecordList("ID3D12GraphicsCommandListX::ExecuteBundle", 27, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetDescriptorHeaps(
    UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) {
    bound_cbv_srv_uav_heap_ = nullptr;
    bound_sampler_heap_ = nullptr;
    if (!ppDescriptorHeaps || NumDescriptorHeaps == 0u) {
        host_list_->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
        RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
        return;
    }
    constexpr UINT kStackHeaps = 8u;
    ID3D12DescriptorHeap* stack_heaps[kStackHeaps];
    std::vector<ID3D12DescriptorHeap*> heap_vec;
    ID3D12DescriptorHeap** unwrapped = stack_heaps;
    if (NumDescriptorHeaps > kStackHeaps) {
        heap_vec.resize(NumDescriptorHeaps);
        unwrapped = heap_vec.data();
    }
    for (UINT i = 0u; i < NumDescriptorHeaps; ++i) {
        unwrapped[i] = UnwrapDescriptorHeap(ppDescriptorHeaps[i]);
        if (unwrapped[i]) {
            const D3D12_DESCRIPTOR_HEAP_DESC hd = unwrapped[i]->GetDesc();
            if (hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                bound_cbv_srv_uav_heap_ = unwrapped[i];
            } else if (hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
                bound_sampler_heap_ = unwrapped[i];
            }
        }
    }
    if (device_x_) {
        device_x_->RecordBridgeHeapBind();
    }
    host_list_->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped);
    RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootSignature(
    ID3D12RootSignature* pRootSignature) {
    ID3D12RootSignature* host_rs = UnwrapRootSignature(pRootSignature);
    last_host_compute_rs_ = host_rs;
    host_list_->SetComputeRootSignature(host_rs);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootSignature", 29, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootSignature(
    ID3D12RootSignature* pRootSignature) {
    host_list_->SetGraphicsRootSignature(UnwrapRootSignature(pRootSignature));
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootSignature", 30, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (device_x_) {
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, BaseDescriptor.ptr);
        device_x_->RecordBridgeTableBind();
    }
    host_list_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootDescriptorTable", 31, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    if (device_x_) {
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, BaseDescriptor.ptr);
        device_x_->RecordBridgeTableBind();
    }
    host_list_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootDescriptorTable", 32, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRoot32BitConstant(
    UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_) {
        (void)device_x_->GetDescriptorBridge().SetRootSgpr(
            DestOffsetIn32BitValues % openscarlett::DescriptorSgprBridge::kRootSgprCount,
            SrcData,
            openscarlett::RootSgprKind::InlineConstant32);
        device_x_->RecordBridgeRootConstUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_root_constants_.size() <= DestOffsetIn32BitValues) {
            vk_cmp_root_constants_.resize(static_cast<size_t>(DestOffsetIn32BitValues) + 1u, 0u);
        }
        vk_cmp_root_constants_[DestOffsetIn32BitValues] = SrcData;
    }
    host_list_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstant", 33, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstant(
    UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_) {
        (void)device_x_->GetDescriptorBridge().SetRootSgpr(
            DestOffsetIn32BitValues % openscarlett::DescriptorSgprBridge::kRootSgprCount,
            SrcData,
            openscarlett::RootSgprKind::InlineConstant32);
        device_x_->RecordBridgeRootConstUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_root_constants_.size() <= DestOffsetIn32BitValues) {
            vk_gfx_root_constants_.resize(static_cast<size_t>(DestOffsetIn32BitValues) + 1u, 0u);
        }
        vk_gfx_root_constants_[DestOffsetIn32BitValues] = SrcData;
    }
    host_list_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstant", 34, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && pSrcData && Num32BitValuesToSet > 0u) {
        const uint32_t start = DestOffsetIn32BitValues;
        if (start < openscarlett::DescriptorSgprBridge::kRootSgprCount) {
            const uint32_t avail = openscarlett::DescriptorSgprBridge::kRootSgprCount - start;
            const uint32_t count = std::min<uint32_t>(Num32BitValuesToSet, avail);
            (void)device_x_->GetDescriptorBridge().SetRootSgprRange(
                start, static_cast<const uint32_t*>(pSrcData), count,
                openscarlett::RootSgprKind::InlineConstant32);
        }
        device_x_->RecordBridgeRootConstUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive() && pSrcData && Num32BitValuesToSet > 0u) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        const size_t req = static_cast<size_t>(DestOffsetIn32BitValues) + Num32BitValuesToSet;
        if (vk_cmp_root_constants_.size() < req) vk_cmp_root_constants_.resize(req, 0u);
        std::memcpy(vk_cmp_root_constants_.data() + DestOffsetIn32BitValues,
                    pSrcData, static_cast<size_t>(Num32BitValuesToSet) * sizeof(uint32_t));
    }
    host_list_->SetComputeRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRoot32BitConstants", 35, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRoot32BitConstants(
    UINT RootParameterIndex, UINT Num32BitValuesToSet,
    const void* pSrcData, UINT DestOffsetIn32BitValues) {
    if (device_x_ && pSrcData && Num32BitValuesToSet > 0u) {
        const uint32_t start = DestOffsetIn32BitValues;
        if (start < openscarlett::DescriptorSgprBridge::kRootSgprCount) {
            const uint32_t avail = openscarlett::DescriptorSgprBridge::kRootSgprCount - start;
            const uint32_t count = std::min<uint32_t>(Num32BitValuesToSet, avail);
            (void)device_x_->GetDescriptorBridge().SetRootSgprRange(
                start, static_cast<const uint32_t*>(pSrcData), count,
                openscarlett::RootSgprKind::InlineConstant32);
        }
        device_x_->RecordBridgeRootConstUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive() && pSrcData && Num32BitValuesToSet > 0u) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        const size_t req = static_cast<size_t>(DestOffsetIn32BitValues) + Num32BitValuesToSet;
        if (vk_gfx_root_constants_.size() < req) vk_gfx_root_constants_.resize(req, 0u);
        std::memcpy(vk_gfx_root_constants_.data() + DestOffsetIn32BitValues,
                    pSrcData, static_cast<size_t>(Num32BitValuesToSet) * sizeof(uint32_t));
    }
    host_list_->SetGraphicsRoot32BitConstants(
        RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstants", 36, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_) {
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, translated);
        device_x_->RecordBridgeRootCbvUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_cbvs_.size() <= RootParameterIndex) vk_cmp_cbvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_cbvs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootConstantBufferView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootConstantBufferView", 37, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_) {
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, translated);
        device_x_->RecordBridgeRootCbvUpdate();
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_cbvs_.size() <= RootParameterIndex) vk_gfx_cbvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_cbvs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootConstantBufferView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_srvs_.size() <= RootParameterIndex) vk_cmp_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_srvs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootShaderResourceView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootShaderResourceView", 39, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_srvs_.size() <= RootParameterIndex) vk_gfx_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_srvs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootShaderResourceView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootShaderResourceView", 40, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_) {
        uint64_t off = 0ull;
        if (ID3D12Resource* res = device_x_->ResolveBufferResourceByGpuVa(BufferLocation, &off)) {
            RecordMirroredResourceGpuWrite(res);
        }
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_uavs_.size() <= RootParameterIndex) vk_cmp_uavs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_uavs_[RootParameterIndex] = translated;
    }
    host_list_->SetComputeRootUnorderedAccessView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootUnorderedAccessView", 41, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(BufferLocation) : BufferLocation;
    if (device_x_) {
        uint64_t off = 0ull;
        if (ID3D12Resource* res = device_x_->ResolveBufferResourceByGpuVa(BufferLocation, &off)) {
            RecordMirroredResourceGpuWrite(res);
        }
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_uavs_.size() <= RootParameterIndex) vk_gfx_uavs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_uavs_[RootParameterIndex] = translated;
    }
    host_list_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, translated);
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootUnorderedAccessView", 42, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (pView) {
            vk_cur_ibv_ = *pView;
            vk_has_ibv_ = true;
        } else {
            vk_has_ibv_ = false;
        }
    }
    host_list_->IASetIndexBuffer(pView);
    RecordList("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetVertexBuffers(
    UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    if (device_x_ && device_x_->IsVulkanRoutingActive() && StartSlot == 0u && NumViews > 0u && pViews) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_vbv_ = pViews[0];
        vk_has_vbv_ = true;
    }
    host_list_->IASetVertexBuffers(StartSlot, NumViews, pViews);
    RecordList("ID3D12GraphicsCommandListX::IASetVertexBuffers", 44, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SOSetTargets(
    UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) {
    host_list_->SOSetTargets(StartSlot, NumViews, pViews);
    RecordList("ID3D12GraphicsCommandListX::SOSetTargets", 45, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetRenderTargets(
    UINT NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_rtv_ = (NumRenderTargetDescriptors > 0u && pRenderTargetDescriptors)
                          ? device_x_->ResolveVulkanRtvResource(pRenderTargetDescriptors[0])
                          : nullptr;
        vk_cur_dsv_ = pDepthStencilDescriptor
                          ? device_x_->ResolveVulkanDsvResource(*pDepthStencilDescriptor)
                          : nullptr;
    }
    host_list_->OMSetRenderTargets(
        NumRenderTargetDescriptors, pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    RecordList("ID3D12GraphicsCommandListX::OMSetRenderTargets", 46, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearDepthStencilView(
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
    FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_clear_depth_ = Depth;
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::ClearDsv;
        op.dsv_resource = device_x_->ResolveVulkanDsvResource(DepthStencilView);
        op.clear_depth = Depth;
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->ClearDepthStencilView(
        DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearDepthStencilView", 47, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearRenderTargetView(
    D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
    UINT NumRects, const D3D12_RECT* pRects) {
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (ColorRGBA) {
            std::memcpy(vk_clear_color_, ColorRGBA, sizeof(vk_clear_color_));
            vk_has_clear_color_ = true;
        }
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::ClearRtv;
        op.rtv_resource = device_x_->ResolveVulkanRtvResource(RenderTargetView);
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }
    host_list_->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearRenderTargetView", 48, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewUint(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    if (device_x_ && host_res) {
        SamplerFeedbackMapInfo sf_info{};
        if (device_x_->GetSamplerFeedbackManager().LookupFeedbackMap(host_res, sf_info) &&
            !sf_info.using_native_hardware) {
            if (device_x_->GetSamplerFeedbackManager().ClearSamplerFeedbackMap(host_list_, host_res).ok()) {
                if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
                if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);
                RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint[SamplerFeedback]", 49, S_OK);
                RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint", 49, S_OK);
                return;
            }
        }
    }
    host_list_->ClearUnorderedAccessViewUint(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, host_res, Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint", 49, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewFloat(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    host_list_->ClearUnorderedAccessViewFloat(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, UnwrapResource(pResource), Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewFloat", 50, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DiscardResource(
    ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) {
    host_list_->DiscardResource(UnwrapResource(pResource), pRegion);
    RecordList("ID3D12GraphicsCommandListX::DiscardResource", 51, S_OK);
}

namespace {

bool TryMapD3D12QueryType(D3D12_QUERY_TYPE type, QueryKind& out_kind) noexcept {
    switch (type) {
        case D3D12_QUERY_TYPE_OCCLUSION:
            out_kind = QueryKind::Occlusion;
            return true;
        case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
            out_kind = QueryKind::BinaryOcclusion;
            return true;
        case D3D12_QUERY_TYPE_TIMESTAMP:
            out_kind = QueryKind::Timestamp;
            return true;
        case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
            out_kind = QueryKind::PipelineStatistics;
            return true;
        default:
            return false;
    }
}

}  // namespace

void STDMETHODCALLTYPE CommandListX::BeginQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    ID3D12QueryHeap* host_qh = UnwrapQueryHeap(pQueryHeap);
    QueryKind qkind = QueryKind::Occlusion;
    if (device_x_) {
        device_x_->RecordCommandListQueryBegin();
        if (TryMapD3D12QueryType(Type, qkind) &&
            device_x_->GetQueryAccumulator().IsInitialized() &&
            (host_qh == nullptr || host_qh == device_x_->GetQueryAccumulator().GetQueryHeap(qkind))) {
            (void)device_x_->GetQueryAccumulator().BeginQuery(host_list_, qkind, Index);
            RecordList("ID3D12GraphicsCommandListX::BeginQuery", 52, S_OK);
            return;
        }
    }
    host_list_->BeginQuery(host_qh, Type, Index);
    RecordList("ID3D12GraphicsCommandListX::BeginQuery", 52, S_OK);
}

void STDMETHODCALLTYPE CommandListX::EndQuery(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) {
    ID3D12QueryHeap* host_qh = UnwrapQueryHeap(pQueryHeap);
    QueryKind qkind = QueryKind::Occlusion;
    if (device_x_) {
        device_x_->RecordCommandListQueryEnd();
        if (TryMapD3D12QueryType(Type, qkind) &&
            device_x_->GetQueryAccumulator().IsInitialized() &&
            (host_qh == nullptr || host_qh == device_x_->GetQueryAccumulator().GetQueryHeap(qkind))) {
            (void)device_x_->GetQueryAccumulator().EndQuery(host_list_, qkind, Index);
            RecordList("ID3D12GraphicsCommandListX::EndQuery", 53, S_OK);
            return;
        }
    }
    host_list_->EndQuery(host_qh, Type, Index);
    RecordList("ID3D12GraphicsCommandListX::EndQuery", 53, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveQueryData(
    ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
    ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) {
    ID3D12QueryHeap* host_qh = UnwrapQueryHeap(pQueryHeap);
    ID3D12Resource* host_dst = UnwrapResource(pDestinationBuffer);
    RecordMirroredResourceGpuWrite(host_dst);
    QueryKind qkind = QueryKind::Occlusion;
    if (device_x_) {
        device_x_->RecordCommandListQueryResolve();
        if (TryMapD3D12QueryType(Type, qkind) &&
            device_x_->GetQueryAccumulator().IsInitialized() &&
            (host_qh == nullptr || host_qh == device_x_->GetQueryAccumulator().GetQueryHeap(qkind))) {
            (void)device_x_->GetQueryAccumulator().ResolveQueryData(
                host_list_, qkind, StartIndex, NumQueries, host_dst, AlignedDestinationBufferOffset);
            RecordList("ID3D12GraphicsCommandListX::ResolveQueryData", 54, S_OK);
            return;
        }
    }
    host_list_->ResolveQueryData(
        host_qh, Type, StartIndex, NumQueries, host_dst, AlignedDestinationBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ResolveQueryData", 54, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPredication(
    ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) {
    host_list_->SetPredication(UnwrapResource(pBuffer), AlignedBufferOffset, Operation);
    RecordList("ID3D12GraphicsCommandListX::SetPredication", 55, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetMarker(UINT Metadata, const void* pData, UINT Size) {
    host_list_->SetMarker(Metadata, pData, Size);
    RecordList("ID3D12GraphicsCommandListX::SetMarker", 56, S_OK);
}

void STDMETHODCALLTYPE CommandListX::BeginEvent(UINT Metadata, const void* pData, UINT Size) {
    host_list_->BeginEvent(Metadata, pData, Size);
    RecordList("ID3D12GraphicsCommandListX::BeginEvent", 57, S_OK);
}

void STDMETHODCALLTYPE CommandListX::EndEvent() {
    host_list_->EndEvent();
    RecordList("ID3D12GraphicsCommandListX::EndEvent", 58, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ExecuteIndirect(
    ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
    ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
    ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) {
    ID3D12Resource* host_arg = UnwrapResource(pArgumentBuffer);
    ID3D12Resource* host_cnt = UnwrapResource(pCountBuffer);

    // Ensure dirty CPU pages in TitleVAMirror-backed indirect argument / count buffers are flushed
    // directly into the GPU shadow buffer right before ExecuteIndirect reads them.
    if (device_x_ && device_x_->HasMirroredTitleVaRegions()) {
        auto flush_indirect_buf = [&](ID3D12Resource* host_buf) {
            if (!host_buf) return;
            uint64_t va = 0ull;
            if (!device_x_->LookupMirroredTitleVaForResource(host_buf, va) || va == 0ull) return;
            std::vector<openscarlett::DirtyPageRange> dirty_ranges;
            if (device_x_->GetTitleVaMirror().QueryDirtyPageRanges(va, false, dirty_ranges).ok() &&
                !dirty_ranges.empty()) {
                (void)device_x_->GetTitleVaMirror().NotifyGpuResourceState(
                    va, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                openscarlett::FlushStats fstats{};
                if (device_x_->GetTitleVaMirror().FlushDirtyPagesToGpu(
                        host_list_, va, &fstats, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT).ok() &&
                    fstats.regions_flushed > 0ull) {
                    device_x_->RecordIndirectMirrorFlush();
                }
            }
            RecordMirroredResourceGpuWrite(host_buf);
        };
        flush_indirect_buf(host_arg);
        flush_indirect_buf(host_cnt);
    }

    host_list_->ExecuteIndirect(
        UnwrapCommandSignature(pCommandSignature), MaxCommandCount,
        host_arg, ArgumentBufferOffset, host_cnt, CountBufferOffset);
    RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect", 59, S_OK);
}

// ID3D12GraphicsCommandList1 (slots 60..65)
void STDMETHODCALLTYPE CommandListX::AtomicCopyBufferUINT(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
    ID3D12Resource* const* ppDependentResources,
    const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) {
    if (host_list1_) {
        std::vector<ID3D12Resource*> deps;
        ID3D12Resource* const* dep_ptr = ppDependentResources;
        if (Dependencies > 0u && ppDependentResources) {
            deps.resize(Dependencies);
            for (UINT i = 0u; i < Dependencies; ++i) {
                deps[i] = UnwrapResource(ppDependentResources[i]);
            }
            dep_ptr = deps.data();
        }
        host_list1_->AtomicCopyBufferUINT(
            UnwrapResource(pDstBuffer), DstOffset, UnwrapResource(pSrcBuffer), SrcOffset,
            Dependencies, dep_ptr, pDependentSubresourceRanges);
    }
    RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT",
               60, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::AtomicCopyBufferUINT64(
    ID3D12Resource* pDstBuffer, UINT64 DstOffset,
    ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
    ID3D12Resource* const* ppDependentResources,
    const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) {
    if (host_list1_) {
        std::vector<ID3D12Resource*> deps;
        ID3D12Resource* const* dep_ptr = ppDependentResources;
        if (Dependencies > 0u && ppDependentResources) {
            deps.resize(Dependencies);
            for (UINT i = 0u; i < Dependencies; ++i) {
                deps[i] = UnwrapResource(ppDependentResources[i]);
            }
            dep_ptr = deps.data();
        }
        host_list1_->AtomicCopyBufferUINT64(
            UnwrapResource(pDstBuffer), DstOffset, UnwrapResource(pSrcBuffer), SrcOffset,
            Dependencies, dep_ptr, pDependentSubresourceRanges);
    }
    RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT64",
               61, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::OMSetDepthBounds(FLOAT Min, FLOAT Max) {
    depth_bounds_min_.store(Min, std::memory_order_relaxed);
    depth_bounds_max_.store(Max, std::memory_order_relaxed);
    if (host_list1_ && device_x_ && device_x_->SupportsDepthBoundsTest() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->OMSetDepthBounds(Min, Max);
    }
    RecordList("ID3D12GraphicsCommandListX::OMSetDepthBounds", 62, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetSamplePositions(
    UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions) {
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = NumSamplesPerPixel;
        sample_positions_pixels_ = NumPixels;
        const size_t total = static_cast<size_t>(NumSamplesPerPixel) * static_cast<size_t>(NumPixels);
        if (pSamplePositions && total > 0u && total <= 64u) {
            sample_positions_.assign(pSamplePositions, pSamplePositions + total);
        } else {
            sample_positions_.clear();
        }
    }
    if (host_list1_ && device_x_ && device_x_->SupportsProgrammableSamplePositions() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
    }
    RecordList("ID3D12GraphicsCommandListX::SetSamplePositions", 63, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResolveSubresourceRegion(
    ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
    ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
    DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) {
    ID3D12Resource* host_dst = UnwrapResource(pDstResource);
    ID3D12Resource* host_src = UnwrapResource(pSrcResource);
    const uint32_t mode_val = static_cast<uint32_t>(ResolveMode);
    constexpr uint32_t kResolveEncodeFeedback = 4u;
    constexpr uint32_t kResolveDecodeFeedback = 5u;

    if (device_x_ && mode_val == kResolveDecodeFeedback && host_src && host_dst) {
        SamplerFeedbackMapInfo sf_info{};
        if (device_x_->GetSamplerFeedbackManager().LookupFeedbackMap(host_src, sf_info)) {
            HRESULT hr = S_OK;
            if (sf_info.using_native_hardware) {
                const openscarlett::Status st =
                    device_x_->GetSamplerFeedbackManager().RecordResolveToR8Texture(
                        host_list_, host_src, host_dst,
                        D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                        D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                hr = st.ok() ? S_OK : E_FAIL;
                if (SUCCEEDED(hr)) device_x_->RecordFeedbackDecodeIntercept();
            } else {
                hr = device_x_->RecordFeedbackFallbackDecode(
                    host_list_, host_src, host_dst, DstSubresource, DstX, DstY);
                if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
                if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);
            }
            RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion[DecodeFeedback]", 64, hr);
            RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion", 64, hr);
            return;
        }
    }

    if (device_x_ && mode_val == kResolveEncodeFeedback && host_src && host_dst) {
        SamplerFeedbackMapInfo sf_info{};
        if (device_x_->GetSamplerFeedbackManager().LookupFeedbackMap(host_dst, sf_info)) {
            HRESULT hr = S_OK;
            if (sf_info.using_native_hardware && host_list1_) {
                const UINT eff_dst_sub =
                    (DstSubresource == 0u && sf_info.target_array_size == 1u) ? UINT_MAX : DstSubresource;
                const UINT eff_src_sub =
                    (SrcSubresource == 0u && sf_info.target_array_size == 1u) ? UINT_MAX : SrcSubresource;
                host_list1_->ResolveSubresourceRegion(
                    host_dst, eff_dst_sub, DstX, DstY,
                    host_src, eff_src_sub, pSrcRect, Format, ResolveMode);
                device_x_->RecordFeedbackEncodeIntercept();
            } else {
                hr = device_x_->RecordFeedbackFallbackEncode(
                    host_list_, host_src, SrcSubresource, host_dst);
                if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
                if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);
            }
            RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion[EncodeFeedback]", 64, hr);
            RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion", 64, hr);
            return;
        }
    }

    if (host_list1_) {
        host_list1_->ResolveSubresourceRegion(
            host_dst, DstSubresource, DstX, DstY,
            host_src, SrcSubresource, pSrcRect, Format, ResolveMode);
    }
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion",
               64, host_list1_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetViewInstanceMask(UINT Mask) {
    view_instance_mask_.store(Mask, std::memory_order_relaxed);
    if (host_list1_ && device_x_ && device_x_->SupportsViewInstancing() &&
        list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list1_->SetViewInstanceMask(Mask);
    }
    RecordList("ID3D12GraphicsCommandListX::SetViewInstanceMask", 65, S_OK);
}

// ID3D12GraphicsCommandList2 (slot 66)
void STDMETHODCALLTYPE CommandListX::WriteBufferImmediate(
    UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) {
    if (Count == 0u || !pParams) {
        RecordList("ID3D12GraphicsCommandListX::WriteBufferImmediate", 66, S_OK);
        return;
    }

    constexpr UINT kStackParams = 32u;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER stack_params[kStackParams];
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> heap_params;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* translated = stack_params;
    if (Count > kStackParams) {
        heap_params.resize(Count);
        translated = heap_params.data();
    }

    for (UINT i = 0u; i < Count; ++i) {
        translated[i] = pParams[i];
        if (device_x_) {
            translated[i].Dest = device_x_->TranslateGuestGpuVa(pParams[i].Dest);
            if (device_x_->IsVulkanRoutingActive()) {
                uint64_t buf_off = 0ull;
                ID3D12Resource* vk_res =
                    device_x_->ResolveVulkanBufferByGpuVa(translated[i].Dest, &buf_off);
                if (vk_res) {
                    const uint32_t v = translated[i].Value;
                    device_x_->WriteVulkanBufferData(vk_res, buf_off, &v, sizeof(v));
                }
            }
        }
    }

    if (host_list2_) {
        host_list2_->WriteBufferImmediate(Count, translated, pModes);
    } else {
        // Safe CPU fallback when host command list lacks ID3D12GraphicsCommandList2.
        for (UINT i = 0u; i < Count; ++i) {
            const uint64_t addr = translated[i].Dest;
            if (addr != 0ull) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)),
                                 &mbi, sizeof(mbi)) != 0 &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0u &&
                    (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0u) {
                    const uint64_t region_end =
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) +
                        static_cast<uint64_t>(mbi.RegionSize);
                    if (addr + sizeof(uint32_t) <= region_end) {
                        *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(addr)) =
                            translated[i].Value;
                    }
                }
            }
        }
    }
    write_buffer_immediate_calls_.fetch_add(1ull, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::WriteBufferImmediate", 66, S_OK);
}

// ID3D12GraphicsCommandList3 (slot 67)
void STDMETHODCALLTYPE CommandListX::SetProtectedResourceSession(
    ID3D12ProtectedResourceSession* pProtectedResourceSession) {
    if (host_list3_) host_list3_->SetProtectedResourceSession(pProtectedResourceSession);
    RecordList("ID3D12GraphicsCommandListX::SetProtectedResourceSession",
               67, host_list3_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList4 (slots 68..76)
void STDMETHODCALLTYPE CommandListX::BeginRenderPass(
    UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS Flags) {
    if (host_list4_) {
        std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rts;
        const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt_ptr = pRenderTargets;
        if (NumRenderTargets > 0u && pRenderTargets) {
            bool any_resolve = false;
            for (UINT i = 0u; i < NumRenderTargets; ++i) {
                if (pRenderTargets[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
                    any_resolve = true;
                    break;
                }
            }
            if (any_resolve) {
                rts.assign(pRenderTargets, pRenderTargets + NumRenderTargets);
                for (auto& rt : rts) {
                    if (rt.EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
                        rt.EndingAccess.Resolve.pSrcResource = UnwrapResource(rt.EndingAccess.Resolve.pSrcResource);
                        rt.EndingAccess.Resolve.pDstResource = UnwrapResource(rt.EndingAccess.Resolve.pDstResource);
                    }
                }
                rt_ptr = rts.data();
            }
        }
        host_list4_->BeginRenderPass(NumRenderTargets, rt_ptr, pDepthStencil, Flags);
    }
    RecordList("ID3D12GraphicsCommandListX::BeginRenderPass",
               68, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::EndRenderPass() {
    if (host_list4_) host_list4_->EndRenderPass();
    RecordList("ID3D12GraphicsCommandListX::EndRenderPass",
               69, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::InitializeMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
    SIZE_T InitializationParametersDataSizeInBytes) {
    if (host_list4_) {
        host_list4_->InitializeMetaCommand(
            pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
    }
    RecordList("ID3D12GraphicsCommandListX::InitializeMetaCommand",
               70, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::ExecuteMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
    SIZE_T ExecutionParametersDataSizeInBytes) {
    if (host_list4_) {
        host_list4_->ExecuteMetaCommand(
            pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
    }
    RecordList("ID3D12GraphicsCommandListX::ExecuteMetaCommand",
               71, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::BuildRaytracingAccelerationStructure(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
    UINT NumPostbuildInfoDescs,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) {
    if (host_list4_) {
        host_list4_->BuildRaytracingAccelerationStructure(
            pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
    }
    RecordList("ID3D12GraphicsCommandListX::BuildRaytracingAccelerationStructure",
               72, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::EmitRaytracingAccelerationStructurePostbuildInfo(
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
    UINT NumSourceAccelerationStructures,
    const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) {
    if (host_list4_) {
        host_list4_->EmitRaytracingAccelerationStructurePostbuildInfo(
            pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
    }
    RecordList("ID3D12GraphicsCommandListX::EmitRaytracingAccelerationStructurePostbuildInfo",
               73, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::CopyRaytracingAccelerationStructure(
    D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
    D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) {
    if (host_list4_) {
        host_list4_->CopyRaytracingAccelerationStructure(
            DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
    }
    RecordList("ID3D12GraphicsCommandListX::CopyRaytracingAccelerationStructure",
               74, host_list4_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::SetPipelineState1(ID3D12StateObject* pStateObject) {
    ID3D12StateObject* host_so = UnwrapStateObject(pStateObject);
    const bool is_sw_proxy = (pStateObject != nullptr && host_so == nullptr);
    if (host_list4_ && !is_sw_proxy) {
        host_list4_->SetPipelineState1(host_so);
    }
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState1",
               75, (host_list4_ || is_sw_proxy) ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) {
    if (host_list4_) host_list4_->DispatchRays(pDesc);
    RecordList("ID3D12GraphicsCommandListX::DispatchRays",
               76, host_list4_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList5 (slots 77..78)
void STDMETHODCALLTYPE CommandListX::RSSetShadingRate(
    D3D12_SHADING_RATE baseShadingRate,
    const D3D12_SHADING_RATE_COMBINER* combiners) {
    if (host_list5_) host_list5_->RSSetShadingRate(baseShadingRate, combiners);
    RecordList("ID3D12GraphicsCommandListX::RSSetShadingRate",
               77, host_list5_ ? S_OK : E_NOINTERFACE);
}

void STDMETHODCALLTYPE CommandListX::RSSetShadingRateImage(ID3D12Resource* shadingRateImage) {
    if (host_list5_) host_list5_->RSSetShadingRateImage(UnwrapResource(shadingRateImage));
    RecordList("ID3D12GraphicsCommandListX::RSSetShadingRateImage",
               78, host_list5_ ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandList6 (slot 79)
void STDMETHODCALLTYPE CommandListX::DispatchMesh(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (device_x_) {
        device_x_->RecordCmdListDispatchMesh(
            ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    if (ThreadGroupCountX == 0u || ThreadGroupCountY == 0u || ThreadGroupCountZ == 0u) {
        RecordList("ID3D12GraphicsCommandListX::DispatchMesh", 79, S_OK);
        return;
    }
    bool skip_host = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::DispatchMesh;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
        op.x = ThreadGroupCountX;
        op.y = ThreadGroupCountY;
        op.z = ThreadGroupCountZ;
        op.rtv_resource = vk_cur_rtv_;
        op.dsv_resource = vk_cur_dsv_;
        op.has_clear_color = vk_has_clear_color_;
        std::memcpy(op.clear_color, vk_clear_color_, sizeof(op.clear_color));
        op.clear_depth = vk_clear_depth_;
        op.viewport_width = vk_vp_width_;
        op.viewport_height = vk_vp_height_;
        op.root_constants = vk_gfx_root_constants_;
        op.root_cbvs = vk_gfx_cbvs_;
        op.root_srvs = vk_gfx_srvs_;
        op.root_uavs = vk_gfx_uavs_;
        vk_batch_.ops.push_back(std::move(op));
        skip_host = vk_cur_pso_is_spirv_;
        if (!skip_host) vk_batch_.has_host_commands = true;
    }
    const bool host_mesh_supported =
        !device_x_ || device_x_->AdapterInfo().mesh_shader_tier > 0u;
    if (!skip_host && host_list6_ && host_mesh_supported) {
        // Respect D3D12 per-axis limit (65535) while dispatching all X, Y, Z dimensions cleanly.
        constexpr UINT kMaxDim = 65535u;
        const UINT gx = std::min<UINT>(ThreadGroupCountX, kMaxDim);
        const UINT gy = std::min<UINT>(ThreadGroupCountY, kMaxDim);
        const UINT gz = std::min<UINT>(ThreadGroupCountZ, kMaxDim);
        host_list6_->DispatchMesh(gx, gy, gz);
    }
    RecordList("ID3D12GraphicsCommandListX::DispatchMesh",
               79, (skip_host || host_list6_) ? S_OK : E_NOINTERFACE);
}

// ID3D12GraphicsCommandListX (slots 80..88)
void STDMETHODCALLTYPE CommandListX::SetGpuVirtualAddressHintX(uint64_t Hint) {
    gpu_va_hint_.store(Hint, std::memory_order_relaxed);
    RecordList("ID3D12GraphicsCommandListX::SetGpuVirtualAddressHintX", 80, S_OK);
}

ID3D12GraphicsCommandList* STDMETHODCALLTYPE CommandListX::GetUnderlyingHostCommandList() {
    RecordList("ID3D12GraphicsCommandListX::GetUnderlyingHostCommandList", 81, S_OK);
    return host_list_;
}

void STDMETHODCALLTYPE CommandListX::KickoffX() {
    kickoff_count_.fetch_add(1ull, std::memory_order_relaxed);
    if (device_x_) device_x_->RecordCmdListKickoff();
    RecordList("ID3D12GraphicsCommandListX::KickoffX", 82, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Wait32BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint32_t Value,
    uint32_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    if (Address != 0ull) {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        deferred_cpu_waits_.push_back(DeferredCpuWait{
            Address, static_cast<uint64_t>(Value), static_cast<uint64_t>(Mask), Operation, false});
    }
    if (host_list_) {
        const D3D12_COMMAND_LIST_TYPE list_type = host_list_->GetType();
        if (list_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            D3D12_RESOURCE_BARRIER uav_barrier{};
            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_barrier.UAV.pResource = nullptr;
            host_list_->ResourceBarrier(1u, &uav_barrier);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::Wait32BitValueX", 83, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Wait64BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint64_t Value,
    uint64_t Mask,
    D3D12XBOX_WAIT_VALUE_OP Operation) {
    if (Address != 0ull) {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        deferred_cpu_waits_.push_back(DeferredCpuWait{Address, Value, Mask, Operation, true});
    }
    if (host_list_) {
        const D3D12_COMMAND_LIST_TYPE list_type = host_list_->GetType();
        if (list_type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
            D3D12_RESOURCE_BARRIER uav_barrier{};
            uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav_barrier.UAV.pResource = nullptr;
            host_list_->ResourceBarrier(1u, &uav_barrier);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::Wait64BitValueX", 84, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetDepthBiasX(
    FLOAT DepthBias,
    FLOAT DepthBiasClamp,
    FLOAT SlopeScaledDepthBias) {
    depth_bias_.store(DepthBias, std::memory_order_relaxed);
    depth_bias_clamp_.store(DepthBiasClamp, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(SlopeScaledDepthBias, std::memory_order_relaxed);
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    if (host_list9_ && list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        host_list9_->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
    }
#endif
    RecordList("ID3D12GraphicsCommandListX::RSSetDepthBiasX", 85, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Write32BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint32_t Value,
    uint32_t Flags) {
    if (device_x_) device_x_->RecordCmdListWrite32();
    if (Address == 0ull) {
        RecordList("ID3D12GraphicsCommandListX::Write32BitValueX", 86, E_INVALIDARG);
        return;
    }

    uint64_t buf_off = 0ull;
    ID3D12Resource* host_buf =
        device_x_ ? device_x_->ResolveBufferResourceByGpuVa(Address, &buf_off) : nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->TranslateGuestGpuVa(Address) : Address;

    if (host_buf != nullptr && host_list2_ != nullptr) {
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{};
        param.Dest = translated;
        param.Value = Value;
        D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
        if ((Flags & D3D12XBOX_WRITE_VALUE_FLAG_MARKER_IN) != 0u) {
            mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
        } else if ((Flags & D3D12XBOX_WRITE_VALUE_FLAG_MARKER_OUT) != 0u) {
            mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
        }
        host_list2_->WriteBufferImmediate(1u, &param, &mode);
        RecordMirroredResourceGpuWrite(host_buf);
        write_buffer_immediate_calls_.fetch_add(1ull, std::memory_order_relaxed);
    } else {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        deferred_cpu_writes_.push_back(DeferredCpuWrite32{Address, Value});
    }
    RecordList("ID3D12GraphicsCommandListX::Write32BitValueX", 86, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetPredicationX(
    D3D12_GPU_VIRTUAL_ADDRESS BufferAddress,
    D3D12_PREDICATION_OP Operation,
    uint32_t Flags) {
    if (device_x_) device_x_->RecordCmdListPredicationX();
    D3D12_PREDICATION_OP eff_op = Operation;
    if ((Flags & D3D12XBOX_PREDICATION_FLAG_INVERT) != 0u) {
        eff_op = (Operation == D3D12_PREDICATION_OP_EQUAL_ZERO)
                     ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO
                     : D3D12_PREDICATION_OP_EQUAL_ZERO;
    }
    if (BufferAddress == 0ull) {
        host_list_->SetPredication(nullptr, 0ull, eff_op);
        RecordList("ID3D12GraphicsCommandListX::SetPredicationX", 87, S_OK);
        return;
    }
    uint64_t offset = 0ull;
    ID3D12Resource* host_buf =
        device_x_ ? device_x_->ResolveBufferResourceByGpuVa(BufferAddress, &offset) : nullptr;
    if (host_buf) {
        offset &= ~7ull;  // D3D12 SetPredication requires 8-byte aligned offset
        host_list_->SetPredication(host_buf, offset, eff_op);
    }
    RecordList("ID3D12GraphicsCommandListX::SetPredicationX", 87, host_buf ? S_OK : E_INVALIDARG);
}

void STDMETHODCALLTYPE CommandListX::FlushGpuCacheRange(
    uint32_t Flags,
    D3D12_GPU_VIRTUAL_ADDRESS BaseAddress,
    SIZE_T SizeInBytes) {
    (void)SizeInBytes;
    if (device_x_) device_x_->RecordCmdListFlushCache();
    ID3D12Resource* host_buf = nullptr;
    if (device_x_ && BaseAddress != 0ull) {
        uint64_t off = 0ull;
        host_buf = device_x_->ResolveBufferResourceByGpuVa(BaseAddress, &off);
    }
    if (host_list_ &&
        (list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         list_type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE)) {
        ID3D12Resource* uav_res = nullptr;
        if (host_buf &&
            (host_buf->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            uav_res = host_buf;
        }
        D3D12_RESOURCE_BARRIER uav_barrier{};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = uav_res;
        host_list_->ResourceBarrier(1u, &uav_barrier);
    }
    if (host_buf && (Flags & D3D12XBOX_FLUSH_L2_WRITEBACK) != 0u) {
        RecordMirroredResourceGpuWrite(host_buf);
    }
    RecordList("ID3D12GraphicsCommandListX::FlushGpuCacheRange", 88, S_OK);
}

void CommandListX::GetDepthBiasXState(
    float* out_depth_bias,
    float* out_depth_bias_clamp,
    float* out_slope_scaled_depth_bias) const noexcept {
    if (out_depth_bias) *out_depth_bias = depth_bias_.load(std::memory_order_relaxed);
    if (out_depth_bias_clamp) *out_depth_bias_clamp = depth_bias_clamp_.load(std::memory_order_relaxed);
    if (out_slope_scaled_depth_bias) {
        *out_slope_scaled_depth_bias = slope_scaled_depth_bias_.load(std::memory_order_relaxed);
    }
}

HRESULT STDMETHODCALLTYPE CommandListX::GetCommandListExtState(
    D3D12XBOX_CMDLIST_EXT_STATE* out_state) const noexcept {
    if (!out_state) return E_POINTER;
    out_state->depth_bounds_min = depth_bounds_min_.load(std::memory_order_relaxed);
    out_state->depth_bounds_max = depth_bounds_max_.load(std::memory_order_relaxed);
    out_state->view_instance_mask = view_instance_mask_.load(std::memory_order_relaxed);
    out_state->write_buffer_immediate_calls =
        write_buffer_immediate_calls_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        out_state->sample_positions_per_pixel = sample_positions_per_pixel_;
        out_state->sample_positions_pixels = sample_positions_pixels_;
    }
    out_state->host_supports_depth_bounds =
        (device_x_ && device_x_->SupportsDepthBoundsTest()) ? 1u : 0u;
    out_state->host_supports_view_instancing =
        (device_x_ && device_x_->SupportsViewInstancing()) ? 1u : 0u;
    out_state->host_supports_sample_positions =
        (device_x_ && device_x_->SupportsProgrammableSamplePositions()) ? 1u : 0u;
    return S_OK;
}

void CommandListX::RecordMirroredResourceGpuWrite(ID3D12Resource* host_resource) {
    if (!device_x_ || !host_resource) return;
    uint64_t title_va = 0ull;
    if (!device_x_->LookupMirroredTitleVaForResource(host_resource, title_va) || title_va == 0ull) {
        return;
    }
    std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
    if (std::find(written_mirrored_vas_.begin(), written_mirrored_vas_.end(), title_va) ==
        written_mirrored_vas_.end()) {
        written_mirrored_vas_.push_back(title_va);
    }
}

void CommandListX::ExtractPendingMirrorWritebacks(std::vector<uint64_t>& out_title_vas) const {
    std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
    for (uint64_t va : written_mirrored_vas_) {
        if (std::find(out_title_vas.begin(), out_title_vas.end(), va) == out_title_vas.end()) {
            out_title_vas.push_back(va);
        }
    }
}

void CommandListX::ExtractDeferredCpuOps(
    std::vector<DeferredCpuWrite32>& out_writes,
    std::vector<DeferredCpuWait>& out_waits) const {
    std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
    out_writes.insert(out_writes.end(), deferred_cpu_writes_.begin(), deferred_cpu_writes_.end());
    out_waits.insert(out_waits.end(), deferred_cpu_waits_.begin(), deferred_cpu_waits_.end());
}

// ---------------------------------------------------------------------------
// Sprint 13 Placed Tiled Texture Management, Copy Bridging & Barrier Coherency
// (Leonida #1674)
// ---------------------------------------------------------------------------
void CommandListX::TrackTiledTransientResource(ID3D12Resource* res) {
    if (!res) return;
    std::lock_guard<std::mutex> lock(tiled_transient_mutex_);
    tiled_transient_resources_.push_back(res);
}

void CommandListX::ReleaseTiledTransientResources() {
    std::lock_guard<std::mutex> lock(tiled_transient_mutex_);
    for (ID3D12Resource* r : tiled_transient_resources_) {
        if (r) r->Release();
    }
    tiled_transient_resources_.clear();
}

HRESULT STDMETHODCALLTYPE CommandListX::RegisterPlacedTiledTexture(
    ID3D12Resource* resource,
    const D3D12XBOX_TILED_PLACED_DESC* desc) {
    if (!resource || !desc) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    if (!host_res) return E_INVALIDARG;

    const D3D12_RESOURCE_DESC rdesc = host_res->GetDesc();
    if (rdesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return E_INVALIDARG;
    }

    const XG_TILE_MODE tile_mode = static_cast<XG_TILE_MODE>(desc->xg_tile_mode);
    XG_TEXTURE2D_DESC xg2d{};
    xg2d.Width = static_cast<uint32_t>(rdesc.Width);
    xg2d.Height = rdesc.Height;
    xg2d.MipLevels = std::max<uint32_t>(1u, rdesc.MipLevels);
    xg2d.ArraySize = std::max<uint32_t>(1u, rdesc.DepthOrArraySize);
    xg2d.Format = static_cast<uint32_t>(rdesc.Format);
    xg2d.SampleCount = std::max<uint32_t>(1u, rdesc.SampleDesc.Count);
    xg2d.TileMode = tile_mode;
    xg2d.BindFlags = XG_BIND_SHADER_RESOURCE;
    if ((rdesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
        xg2d.BindFlags |= XG_BIND_RENDER_TARGET;
    }
    if ((rdesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
        xg2d.BindFlags |= XG_BIND_DEPTH_STENCIL;
    }
    if ((rdesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        xg2d.BindFlags |= XG_BIND_UNORDERED_ACCESS;
    }

    XG_RESOURCE_LAYOUT layout{};
    if (!openscarlett::xg::ComputeTexture2DLayout(xg2d, layout)) {
        return E_INVALIDARG;
    }

    PlacedTiledResourceRecord rec{};
    rec.host_resource = host_res;
    rec.host_heap = desc->placed_heap ? UnwrapHeap(desc->placed_heap) : nullptr;
    rec.res_desc = rdesc;
    rec.xg2d_desc = xg2d;
    rec.xg_layout = layout;
    rec.heap_offset_bytes = desc->heap_offset_bytes;
    rec.tile_pool_base_offset_bytes =
        (desc->tile_pool_base_offset_bytes != 0ull)
            ? desc->tile_pool_base_offset_bytes
            : desc->heap_offset_bytes;
    rec.tile_mode = tile_mode;
    rec.all_sub_state = static_cast<D3D12_RESOURCE_STATES>(desc->initial_state);

    const uint32_t sub_count = layout.MipLevels * layout.ArraySize;
    rec.subresources.resize(sub_count);
    uint32_t running_64kb_tiles = 0u;
    for (uint32_t s = 0u; s < sub_count; ++s) {
        openscarlett::XgPlacedTiledSubresourceDesc sd{};
        if (!openscarlett::ComputePlacedTiledSubresourceLayout2D(
                xg2d, rec.heap_offset_bytes, s, sd)) {
            return E_FAIL;
        }
        PlacedTiledSubresourceRecord& sr = rec.subresources[s];
        sr.pitch.subresource_index = sd.subresource_index;
        sr.pitch.mip_level = sd.mip_level;
        sr.pitch.array_slice = sd.array_slice;
        sr.pitch.width_elements = sd.width_elements;
        sr.pitch.height_elements = sd.height_elements;
        sr.pitch.depth_elements = sd.depth_elements;
        sr.pitch.tiles_x = sd.tiles_x;
        sr.pitch.tiles_y = sd.tiles_y;
        sr.pitch.tiles_z = sd.tiles_z;
        sr.pitch.tile_size_bytes = sd.tile_size_bytes;
        sr.pitch.bytes_per_element = sd.bytes_per_element;
        sr.pitch.linear_row_pitch_bytes = sd.linear_row_pitch_bytes;
        sr.pitch.subresource_offset_bytes = sd.subresource_offset_bytes;
        sr.pitch.heap_subresource_offset_bytes = sd.heap_subresource_offset_bytes;
        sr.pitch.subresource_size_bytes = sd.subresource_size_bytes;
        sr.pitch.tile_pool_start_index_64kb = sd.tile_pool_start_index_64kb;
        sr.pitch.tile_pool_count_64kb = sd.tile_pool_count_64kb;
        sr.pitch.current_state = desc->initial_state;
        sr.pitch.dirty_for_flush = 0u;
        sr.dirty_for_flush = false;

        running_64kb_tiles += sd.tile_pool_count_64kb;

        // Seed identity tile-pool mappings for every tile in this subresource.
        for (uint32_t ty = 0u; ty < sd.tiles_y; ++ty) {
            for (uint32_t tx = 0u; tx < sd.tiles_x; ++tx) {
                const uint32_t logical_idx = ty * sd.tiles_x + tx;
                D3D12XBOX_TILED_PLACED_TILE_MAPPING tm{};
                tm.subresource_index = s;
                tm.logical_tile_x = tx;
                tm.logical_tile_y = ty;
                tm.logical_tile_z = 0u;
                tm.heap_tile_index_64kb = logical_idx;
                tm.pool_byte_offset =
                    rec.tile_pool_base_offset_bytes +
                    sd.subresource_offset_bytes +
                    static_cast<uint64_t>(logical_idx) * static_cast<uint64_t>(sd.tile_size_bytes);
                tm.mapped = 1u;
                rec.tile_mappings[{s, tx, ty, 0u}] = tm;
            }
        }
    }
    rec.total_64kb_tiles = std::max<uint32_t>(
        running_64kb_tiles,
        static_cast<uint32_t>((layout.SizeBytes + 65535ull) / 65536ull));

    {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.map[host_res] = std::move(rec);
    }
    if (device_x_) {
        device_x_->SetTrackedResourceTileMode(host_res, tile_mode);
    }
    RecordList("IOpenScarlettTiledPlacedManager::RegisterPlacedTiledTexture", 90, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandListX::UnregisterPlacedTiledTexture(
    ID3D12Resource* resource) {
    if (!resource) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const size_t erased = reg.map.erase(host_res);
    return (erased > 0u) ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE CommandListX::UpdatePlacedTileMappings(
    ID3D12Resource* resource,
    uint32_t mapping_count,
    const D3D12XBOX_TILED_PLACED_TILE_MAPPING* mappings) {
    if (!resource || (mapping_count > 0u && !mappings)) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.map.find(host_res);
    if (it == reg.map.end()) return E_INVALIDARG;

    PlacedTiledResourceRecord& rec = it->second;
    for (uint32_t i = 0u; i < mapping_count; ++i) {
        D3D12XBOX_TILED_PLACED_TILE_MAPPING m = mappings[i];
        if (m.subresource_index >= rec.subresources.size()) return E_INVALIDARG;
        const auto& sp = rec.subresources[m.subresource_index].pitch;
        if (m.logical_tile_x >= sp.tiles_x || m.logical_tile_y >= sp.tiles_y) {
            return E_INVALIDARG;
        }
        const uint64_t tile_stride =
            (sp.tile_size_bytes >= 65536u) ? 65536ull : static_cast<uint64_t>(sp.tile_size_bytes);
        if (m.pool_byte_offset == 0ull && m.mapped != 0u) {
            m.pool_byte_offset =
                rec.tile_pool_base_offset_bytes +
                static_cast<uint64_t>(m.heap_tile_index_64kb) * tile_stride;
        }
        rec.tile_mappings[{m.subresource_index, m.logical_tile_x, m.logical_tile_y, m.logical_tile_z}] = m;
    }
    rec.has_custom_tile_mapping = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandListX::QueryPlacedTiledSubresourcePitch(
    ID3D12Resource* resource,
    uint32_t subresource_index,
    D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH* out_pitch) {
    if (!resource || !out_pitch) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.map.find(host_res);
    if (it == reg.map.end() || subresource_index >= it->second.subresources.size()) {
        return E_INVALIDARG;
    }
    *out_pitch = it->second.subresources[subresource_index].pitch;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandListX::QueryPlacedTiledTextureInfo(
    ID3D12Resource* resource,
    D3D12XBOX_TILED_PLACED_INFO* out_info) {
    if (!resource || !out_info) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.map.find(host_res);
    if (it == reg.map.end()) return E_INVALIDARG;

    const PlacedTiledResourceRecord& rec = it->second;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->heap_offset_bytes = rec.heap_offset_bytes;
    out_info->tile_pool_base_offset_bytes = rec.tile_pool_base_offset_bytes;
    out_info->total_surface_bytes = rec.xg_layout.SizeBytes;
    out_info->xg_tile_mode = static_cast<uint32_t>(rec.tile_mode);
    out_info->subresource_count = static_cast<uint32_t>(rec.subresources.size());
    out_info->total_64kb_tiles = rec.total_64kb_tiles;
    uint32_t mapped_cnt = 0u;
    for (const auto& kv : rec.tile_mappings) {
        if (kv.second.mapped != 0u) ++mapped_cnt;
    }
    out_info->mapped_tile_count = mapped_cnt;
    out_info->current_state = static_cast<uint32_t>(rec.all_sub_state);
    out_info->untile_copy_count = rec.untile_copy_count;
    out_info->retile_copy_count = rec.retile_copy_count;
    out_info->copy_resource_bridge_count = rec.copy_resource_bridge_count;
    out_info->copy_tiles_bridge_count = rec.copy_tiles_bridge_count;
    out_info->barrier_transition_count = rec.barrier_transition_count;
    out_info->aliasing_barrier_count = rec.aliasing_barrier_count;
    out_info->coherency_flush_count = rec.coherency_flush_count;
    out_info->used_gpu_untile_bridge = rec.used_gpu_untile_bridge ? 1u : 0u;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandListX::QueryPlacedTileMapping(
    ID3D12Resource* resource,
    uint32_t subresource_index,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t tile_z,
    D3D12XBOX_TILED_PLACED_TILE_MAPPING* out_mapping) {
    if (!resource || !out_mapping) return E_INVALIDARG;
    ID3D12Resource* host_res = UnwrapResource(resource);
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.map.find(host_res);
    if (it == reg.map.end()) return E_INVALIDARG;
    auto mit = it->second.tile_mappings.find({subresource_index, tile_x, tile_y, tile_z});
    if (mit == it->second.tile_mappings.end()) return E_INVALIDARG;
    *out_mapping = mit->second;
    return S_OK;
}

bool CommandListX::TryBridgePlacedTiledCopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION& dst_loc,
    UINT dst_x,
    UINT dst_y,
    UINT dst_z,
    const D3D12_TEXTURE_COPY_LOCATION& src_loc,
    const D3D12_BOX* src_box) {
    if (!device_x_ || !device_x_->HostDevice() || !host_list_) return false;
    ID3D12Device* host_dev = device_x_->HostDevice();

    PlacedTiledResourceRecord dst_snap{};
    PlacedTiledResourceRecord src_snap{};
    bool dst_is_placed = false;
    bool src_is_placed = false;
    {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (dst_loc.pResource) {
            auto dit = reg.map.find(dst_loc.pResource);
            if (dit != reg.map.end()) {
                dst_is_placed = true;
                dst_snap = dit->second;
            }
        }
        if (src_loc.pResource) {
            auto sit = reg.map.find(src_loc.pResource);
            if (sit != reg.map.end()) {
                src_is_placed = true;
                src_snap = sit->second;
            }
        }
    }
    if (!dst_is_placed && !src_is_placed) return false;

    auto create_buffer = [&](D3D12_HEAP_TYPE htype, uint64_t width_bytes,
                             D3D12_RESOURCE_FLAGS flags,
                             D3D12_RESOURCE_STATES init_state) -> ID3D12Resource* {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = htype;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = AlignUpPlaced64(std::max<uint64_t>(width_bytes, 256ull), 256ull);
        bd.Height = 1u;
        bd.DepthOrArraySize = 1u;
        bd.MipLevels = 1u;
        bd.SampleDesc.Count = 1u;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = flags;
        ID3D12Resource* buf = nullptr;
        if (FAILED(host_dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd, init_state, nullptr,
                OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&buf)))) {
            return nullptr;
        }
        return buf;
    };

    // Case 1: Tiled buffer -> Placed tiled texture (Untile + 64KB tile-pool gather)
    if (dst_is_placed &&
        dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
        dst_snap.tile_mode != XG_TILE_MODE_LINEAR) {
        const uint32_t sub = dst_loc.SubresourceIndex;
        if (sub >= dst_snap.subresources.size()) return false;
        const auto& sp = dst_snap.subresources[sub].pitch;

        const uint32_t row_pitch =
            (src_loc.PlacedFootprint.Footprint.RowPitch >= sp.width_elements * sp.bytes_per_element)
                ? src_loc.PlacedFootprint.Footprint.RowPitch
                : sp.linear_row_pitch_bytes;

        openscarlett::XgGpuSurface2DRequest req{};
        req.desc = dst_snap.xg2d_desc;
        req.mip_level = sp.mip_level;
        req.slice_index = sp.array_slice;
        req.direction = openscarlett::XgGpuDirection::Untile;
        req.linear_row_pitch_bytes = row_pitch;

        openscarlett::XgGpuUntileParams params{};
        uint64_t linear_bytes = 0ull;
        uint64_t tiled_bytes = 0ull;
        if (!openscarlett::BuildXgGpuUntileParams2D(req, params, &linear_bytes, &tiled_bytes).ok() ||
            linear_bytes == 0ull || tiled_bytes == 0ull) {
            return false;
        }

        const bool can_gpu =
            (list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
             list_type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE) &&
            device_x_->EnsureXgGpuUntilerReady() &&
            device_x_->GetXgGpuUntiler() != nullptr;

        if (can_gpu) {
            ID3D12Resource* effective_src_buf = src_loc.pResource;
            uint64_t effective_src_off = src_loc.PlacedFootprint.Offset;

            // Gather non-contiguous / permuted 64KB tile-pool entries into a contiguous
            // tiled staging buffer on GPU so XgGpuUntiler sees logical tile order.
            if (dst_snap.has_custom_tile_mapping) {
                const uint64_t src_buf_width = src_loc.pResource->GetDesc().Width;
                ID3D12Resource* gathered_buf = create_buffer(
                    D3D12_HEAP_TYPE_DEFAULT, tiled_bytes,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                if (!gathered_buf) return false;

                ID3D12Resource* zero_tile = create_buffer(
                    D3D12_HEAP_TYPE_UPLOAD, sp.tile_size_bytes,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
                if (zero_tile) {
                    void* zp = nullptr;
                    D3D12_RANGE rn{0u, 0u};
                    if (SUCCEEDED(zero_tile->Map(0u, &rn, &zp)) && zp) {
                        std::memset(zp, 0, sp.tile_size_bytes);
                        D3D12_RANGE wf{0u, sp.tile_size_bytes};
                        zero_tile->Unmap(0u, &wf);
                    }
                }

                const uint64_t tile_stride =
                    (sp.tile_size_bytes >= 65536u) ? 65536ull : static_cast<uint64_t>(sp.tile_size_bytes);
                for (uint32_t ty = 0u; ty < sp.tiles_y; ++ty) {
                    for (uint32_t tx = 0u; tx < sp.tiles_x; ++tx) {
                        const uint32_t logical_idx = ty * sp.tiles_x + tx;
                        const uint64_t dst_tile_off =
                            sp.subresource_offset_bytes +
                            static_cast<uint64_t>(logical_idx) * static_cast<uint64_t>(sp.tile_size_bytes);
                        if (dst_tile_off + sp.tile_size_bytes > tiled_bytes) continue;

                        auto mit = dst_snap.tile_mappings.find({sub, tx, ty, 0u});
                        const bool is_mapped = (mit == dst_snap.tile_mappings.end()) || (mit->second.mapped != 0u);
                        if (!is_mapped) {
                            if (zero_tile) {
                                host_list_->CopyBufferRegion(
                                    gathered_buf, dst_tile_off, zero_tile, 0ull, sp.tile_size_bytes);
                            }
                            continue;
                        }
                        const uint32_t pool_tile_idx =
                            (mit != dst_snap.tile_mappings.end()) ? mit->second.heap_tile_index_64kb : logical_idx;
                        const uint64_t src_tile_off =
                            src_loc.PlacedFootprint.Offset +
                            static_cast<uint64_t>(pool_tile_idx) * tile_stride;
                        if (src_tile_off + sp.tile_size_bytes <= src_buf_width) {
                            host_list_->CopyBufferRegion(
                                gathered_buf, dst_tile_off, src_loc.pResource, src_tile_off, sp.tile_size_bytes);
                        } else if (zero_tile) {
                            host_list_->CopyBufferRegion(
                                gathered_buf, dst_tile_off, zero_tile, 0ull, sp.tile_size_bytes);
                        }
                    }
                }

                D3D12_RESOURCE_BARRIER to_srv{};
                to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_srv.Transition.pResource = gathered_buf;
                to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                host_list_->ResourceBarrier(1u, &to_srv);

                TrackTiledTransientResource(gathered_buf);
                if (zero_tile) TrackTiledTransientResource(zero_tile);
                effective_src_buf = gathered_buf;
                effective_src_off = 0ull;
            }

            ID3D12Resource* cbv_buf = create_buffer(
                D3D12_HEAP_TYPE_UPLOAD, 256ull,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            ID3D12Resource* linear_uav = create_buffer(
                D3D12_HEAP_TYPE_DEFAULT, linear_bytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!cbv_buf || !linear_uav) {
                if (cbv_buf) cbv_buf->Release();
                if (linear_uav) linear_uav->Release();
                return false;
            }

            void* cbv_ptr = nullptr;
            D3D12_RANGE rn{0u, 0u};
            if (SUCCEEDED(cbv_buf->Map(0u, &rn, &cbv_ptr)) && cbv_ptr) {
                std::memset(cbv_ptr, 0, 256u);
                std::memcpy(cbv_ptr, &params, sizeof(params));
                D3D12_RANGE wf{0u, sizeof(params)};
                cbv_buf->Unmap(0u, &wf);
            }

            const openscarlett::Status st = device_x_->GetXgGpuUntiler()->RecordDispatchD3D12(
                host_list_, params,
                cbv_buf->GetGPUVirtualAddress(),
                effective_src_buf->GetGPUVirtualAddress() + effective_src_off,
                0ull,
                linear_uav->GetGPUVirtualAddress());
            if (!st.ok()) {
                cbv_buf->Release();
                linear_uav->Release();
                return false;
            }

            D3D12_RESOURCE_BARRIER barriers[2]{};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers[0].UAV.pResource = linear_uav;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = linear_uav;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            host_list_->ResourceBarrier(2u, barriers);

            D3D12_TEXTURE_COPY_LOCATION linear_src = src_loc;
            linear_src.pResource = linear_uav;
            linear_src.PlacedFootprint.Offset = 0ull;
            linear_src.PlacedFootprint.Footprint.RowPitch = row_pitch;
            host_list_->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &linear_src, src_box);

            if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
            if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);

            TrackTiledTransientResource(cbv_buf);
            TrackTiledTransientResource(linear_uav);

            auto& reg = GetPlacedTiledRegistry();
            std::lock_guard<std::mutex> lock(reg.mutex);
            auto it = reg.map.find(dst_loc.pResource);
            if (it != reg.map.end()) {
                ++it->second.untile_copy_count;
                it->second.used_gpu_untile_bridge = true;
                if (sub < it->second.subresources.size()) {
                    it->second.subresources[sub].dirty_for_flush = true;
                    it->second.subresources[sub].pitch.dirty_for_flush = 1u;
                }
            }
            return true;
        }

        // Safe CPU fallback (e.g. COPY command list or minimal host without DXC).
        D3D12_HEAP_PROPERTIES src_hp{};
        D3D12_HEAP_FLAGS src_hf = D3D12_HEAP_FLAG_NONE;
        if (FAILED(src_loc.pResource->GetHeapProperties(&src_hp, &src_hf)) ||
            src_hp.Type != D3D12_HEAP_TYPE_UPLOAD) {
            return false;
        }
        const uint64_t src_buf_width = src_loc.pResource->GetDesc().Width;
        void* src_mapped = nullptr;
        D3D12_RANGE srange{0u, static_cast<SIZE_T>(src_buf_width)};
        if (FAILED(src_loc.pResource->Map(0u, &srange, &src_mapped)) || !src_mapped) {
            return false;
        }

        std::vector<uint8_t> gathered_cpu;
        const uint8_t* tiled_ptr = static_cast<const uint8_t*>(src_mapped) + src_loc.PlacedFootprint.Offset;
        uint64_t tiled_avail = (src_buf_width > src_loc.PlacedFootprint.Offset)
                                   ? (src_buf_width - src_loc.PlacedFootprint.Offset)
                                   : 0ull;
        if (dst_snap.has_custom_tile_mapping) {
            std::vector<openscarlett::XgPlacedTilePoolEntry> entries;
            entries.reserve(static_cast<size_t>(sp.tiles_x) * sp.tiles_y);
            for (uint32_t ty = 0u; ty < sp.tiles_y; ++ty) {
                for (uint32_t tx = 0u; tx < sp.tiles_x; ++tx) {
                    openscarlett::XgPlacedTilePoolEntry e{};
                    e.logical_tile_index = ty * sp.tiles_x + tx;
                    auto mit = dst_snap.tile_mappings.find({sub, tx, ty, 0u});
                    e.heap_tile_index_64kb =
                        (mit != dst_snap.tile_mappings.end()) ? mit->second.heap_tile_index_64kb : e.logical_tile_index;
                    e.pool_byte_offset = 0ull;
                    e.mapped = (mit == dst_snap.tile_mappings.end()) || (mit->second.mapped != 0u);
                    entries.push_back(e);
                }
            }
            std::vector<uint8_t> sub_tiles;
            if (openscarlett::GatherPlacedTiledPool64KB(
                    tiled_ptr, tiled_avail, 0ull, entries, sp.tile_size_bytes, sub_tiles)) {
                gathered_cpu.assign(static_cast<size_t>(tiled_bytes), 0u);
                const size_t copy_n = std::min<size_t>(
                    sub_tiles.size(),
                    (tiled_bytes > sp.subresource_offset_bytes)
                        ? static_cast<size_t>(tiled_bytes - sp.subresource_offset_bytes)
                        : 0u);
                if (copy_n > 0u) {
                    std::memcpy(gathered_cpu.data() + sp.subresource_offset_bytes, sub_tiles.data(), copy_n);
                }
                tiled_ptr = gathered_cpu.data();
                tiled_avail = gathered_cpu.size();
            }
        }

        ID3D12Resource* lin_up = create_buffer(
            D3D12_HEAP_TYPE_UPLOAD, linear_bytes,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        bool ok = false;
        if (lin_up) {
            void* dst_mapped = nullptr;
            D3D12_RANGE rn{0u, 0u};
            if (SUCCEEDED(lin_up->Map(0u, &rn, &dst_mapped)) && dst_mapped) {
                ok = openscarlett::xg::UntileSurface2D(
                    dst_snap.xg2d_desc, sp.mip_level, sp.array_slice,
                    tiled_ptr, tiled_avail, dst_mapped, row_pitch);
                D3D12_RANGE wf{0u, static_cast<SIZE_T>(linear_bytes)};
                lin_up->Unmap(0u, &wf);
            }
        }
        D3D12_RANGE wn{0u, 0u};
        src_loc.pResource->Unmap(0u, &wn);

        if (!ok || !lin_up) {
            if (lin_up) lin_up->Release();
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION linear_src = src_loc;
        linear_src.pResource = lin_up;
        linear_src.PlacedFootprint.Offset = 0ull;
        linear_src.PlacedFootprint.Footprint.RowPitch = row_pitch;
        host_list_->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &linear_src, src_box);
        TrackTiledTransientResource(lin_up);

        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.map.find(dst_loc.pResource);
        if (it != reg.map.end()) {
            ++it->second.untile_copy_count;
            if (sub < it->second.subresources.size()) {
                it->second.subresources[sub].dirty_for_flush = true;
                it->second.subresources[sub].pitch.dirty_for_flush = 1u;
            }
        }
        return true;
    }

    // Case 2: Placed tiled texture -> Buffer (Retile if dst buffer is tiled; else linear readback)
    if (src_is_placed &&
        src_loc.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        const uint32_t sub = src_loc.SubresourceIndex;
        if (sub >= src_snap.subresources.size()) return false;
        const auto& sp = src_snap.subresources[sub].pitch;

        const XG_TILE_MODE dst_buf_mode = device_x_->GetTrackedResourceTileMode(dst_loc.pResource);
        if (dst_buf_mode != XG_TILE_MODE_LINEAR && src_snap.tile_mode != XG_TILE_MODE_LINEAR) {
            const uint32_t row_pitch =
                (dst_loc.PlacedFootprint.Footprint.RowPitch >= sp.width_elements * sp.bytes_per_element)
                    ? dst_loc.PlacedFootprint.Footprint.RowPitch
                    : sp.linear_row_pitch_bytes;

            openscarlett::XgGpuSurface2DRequest req{};
            req.desc = src_snap.xg2d_desc;
            req.desc.TileMode = dst_buf_mode;
            req.mip_level = sp.mip_level;
            req.slice_index = sp.array_slice;
            req.direction = openscarlett::XgGpuDirection::Retile;
            req.linear_row_pitch_bytes = row_pitch;

            openscarlett::XgGpuUntileParams params{};
            uint64_t linear_bytes = 0ull;
            uint64_t tiled_bytes = 0ull;
            if (openscarlett::BuildXgGpuUntileParams2D(req, params, &linear_bytes, &tiled_bytes).ok() &&
                linear_bytes > 0ull && tiled_bytes > 0ull &&
                (list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                 list_type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE) &&
                device_x_->EnsureXgGpuUntilerReady() &&
                device_x_->GetXgGpuUntiler() != nullptr) {
                ID3D12Resource* linear_stage = create_buffer(
                    D3D12_HEAP_TYPE_DEFAULT, linear_bytes,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
                ID3D12Resource* cbv_buf = create_buffer(
                    D3D12_HEAP_TYPE_UPLOAD, 256ull,
                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
                ID3D12Resource* tiled_uav = create_buffer(
                    D3D12_HEAP_TYPE_DEFAULT, tiled_bytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (linear_stage && cbv_buf && tiled_uav) {
                    D3D12_TEXTURE_COPY_LOCATION stage_dst = dst_loc;
                    stage_dst.pResource = linear_stage;
                    stage_dst.PlacedFootprint.Offset = 0ull;
                    stage_dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
                    host_list_->CopyTextureRegion(&stage_dst, 0u, 0u, 0u, &src_loc, src_box);

                    D3D12_RESOURCE_BARRIER to_srv{};
                    to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    to_srv.Transition.pResource = linear_stage;
                    to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    host_list_->ResourceBarrier(1u, &to_srv);

                    void* cbv_ptr = nullptr;
                    D3D12_RANGE rn{0u, 0u};
                    if (SUCCEEDED(cbv_buf->Map(0u, &rn, &cbv_ptr)) && cbv_ptr) {
                        std::memset(cbv_ptr, 0, 256u);
                        std::memcpy(cbv_ptr, &params, sizeof(params));
                        D3D12_RANGE wf{0u, sizeof(params)};
                        cbv_buf->Unmap(0u, &wf);
                    }

                    if (device_x_->GetXgGpuUntiler()->RecordDispatchD3D12(
                            host_list_, params,
                            cbv_buf->GetGPUVirtualAddress(),
                            linear_stage->GetGPUVirtualAddress(),
                            0ull,
                            tiled_uav->GetGPUVirtualAddress()).ok()) {
                        D3D12_RESOURCE_BARRIER post[2]{};
                        post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        post[0].UAV.pResource = tiled_uav;
                        post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        post[1].Transition.pResource = tiled_uav;
                        post[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        post[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        host_list_->ResourceBarrier(2u, post);

                        const uint64_t dst_buf_width = dst_loc.pResource->GetDesc().Width;
                        if (src_snap.has_custom_tile_mapping) {
                            const uint64_t tile_stride =
                                (sp.tile_size_bytes >= 65536u) ? 65536ull : static_cast<uint64_t>(sp.tile_size_bytes);
                            for (uint32_t ty = 0u; ty < sp.tiles_y; ++ty) {
                                for (uint32_t tx = 0u; tx < sp.tiles_x; ++tx) {
                                    const uint32_t logical_idx = ty * sp.tiles_x + tx;
                                    auto mit = src_snap.tile_mappings.find({sub, tx, ty, 0u});
                                    if (mit != src_snap.tile_mappings.end() && mit->second.mapped == 0u) continue;
                                    const uint32_t pool_tile_idx =
                                        (mit != src_snap.tile_mappings.end()) ? mit->second.heap_tile_index_64kb : logical_idx;
                                    const uint64_t src_tile_off =
                                        sp.subresource_offset_bytes +
                                        static_cast<uint64_t>(logical_idx) * static_cast<uint64_t>(sp.tile_size_bytes);
                                    const uint64_t dst_tile_off =
                                        dst_loc.PlacedFootprint.Offset +
                                        static_cast<uint64_t>(pool_tile_idx) * tile_stride;
                                    if (src_tile_off + sp.tile_size_bytes <= tiled_bytes &&
                                        dst_tile_off + sp.tile_size_bytes <= dst_buf_width) {
                                        host_list_->CopyBufferRegion(
                                            dst_loc.pResource, dst_tile_off, tiled_uav, src_tile_off, sp.tile_size_bytes);
                                    }
                                }
                            }
                        } else {
                            const uint64_t copy_bytes = std::min<uint64_t>(
                                tiled_bytes,
                                (dst_buf_width > dst_loc.PlacedFootprint.Offset)
                                    ? (dst_buf_width - dst_loc.PlacedFootprint.Offset)
                                    : 0ull);
                            if (copy_bytes > 0ull) {
                                host_list_->CopyBufferRegion(
                                    dst_loc.pResource, dst_loc.PlacedFootprint.Offset, tiled_uav, 0ull, copy_bytes);
                            }
                        }

                        if (last_host_compute_rs_) host_list_->SetComputeRootSignature(last_host_compute_rs_);
                        if (last_host_pso_) host_list_->SetPipelineState(last_host_pso_);
                        TrackTiledTransientResource(linear_stage);
                        TrackTiledTransientResource(cbv_buf);
                        TrackTiledTransientResource(tiled_uav);

                        auto& reg = GetPlacedTiledRegistry();
                        std::lock_guard<std::mutex> lock(reg.mutex);
                        auto it = reg.map.find(src_loc.pResource);
                        if (it != reg.map.end()) {
                            ++it->second.retile_copy_count;
                            it->second.used_gpu_untile_bridge = true;
                        }
                        return true;
                    }
                }
                if (linear_stage) linear_stage->Release();
                if (cbv_buf) cbv_buf->Release();
                if (tiled_uav) tiled_uav->Release();
            }
        }

        // Standard linear readback from placed tiled texture (already untiled on host GPU).
        host_list_->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &src_loc, src_box);
        return true;
    }

    // Case 3: Texture-to-texture copy involving placed tiled texture(s).
    host_list_->CopyTextureRegion(&dst_loc, dst_x, dst_y, dst_z, &src_loc, src_box);
    if (dst_is_placed && dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.map.find(dst_loc.pResource);
        if (it != reg.map.end() && dst_loc.SubresourceIndex < it->second.subresources.size()) {
            it->second.subresources[dst_loc.SubresourceIndex].dirty_for_flush = true;
            it->second.subresources[dst_loc.SubresourceIndex].pitch.dirty_for_flush = 1u;
        }
    }
    return true;
}

bool CommandListX::TryBridgePlacedTiledCopyResource(
    ID3D12Resource* host_dst,
    ID3D12Resource* host_src) {
    if (!host_dst || !host_src || !host_list_) return false;

    PlacedTiledResourceRecord dst_snap{};
    PlacedTiledResourceRecord src_snap{};
    bool dst_is_placed = false;
    bool src_is_placed = false;
    {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto dit = reg.map.find(host_dst);
        if (dit != reg.map.end()) {
            dst_is_placed = true;
            dst_snap = dit->second;
        }
        auto sit = reg.map.find(host_src);
        if (sit != reg.map.end()) {
            src_is_placed = true;
            src_snap = sit->second;
        }
    }
    if (!dst_is_placed && !src_is_placed) return false;

    const D3D12_RESOURCE_DESC ddesc = host_dst->GetDesc();
    const D3D12_RESOURCE_DESC sdesc = host_src->GetDesc();

    // Buffer -> Placed tiled texture: untile every subresource.
    if (dst_is_placed && sdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        for (size_t s = 0u; s < dst_snap.subresources.size(); ++s) {
            const auto& sp = dst_snap.subresources[s].pitch;
            D3D12_TEXTURE_COPY_LOCATION dloc{};
            dloc.pResource = host_dst;
            dloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dloc.SubresourceIndex = static_cast<UINT>(s);

            D3D12_TEXTURE_COPY_LOCATION sloc{};
            sloc.pResource = host_src;
            sloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sloc.PlacedFootprint.Offset = 0ull;
            sloc.PlacedFootprint.Footprint.Format = ddesc.Format;
            sloc.PlacedFootprint.Footprint.Width = std::max<UINT>(1u, static_cast<UINT>(ddesc.Width) >> sp.mip_level);
            sloc.PlacedFootprint.Footprint.Height = std::max<UINT>(1u, ddesc.Height >> sp.mip_level);
            sloc.PlacedFootprint.Footprint.Depth = 1u;
            sloc.PlacedFootprint.Footprint.RowPitch = sp.linear_row_pitch_bytes;
            if (!TryBridgePlacedTiledCopyTextureRegion(dloc, 0u, 0u, 0u, sloc, nullptr)) {
                return false;
            }
        }
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.map.find(host_dst);
        if (it != reg.map.end()) ++it->second.copy_resource_bridge_count;
        return true;
    }

    // Placed tiled texture -> Buffer: bridge each subresource.
    if (src_is_placed && ddesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        for (size_t s = 0u; s < src_snap.subresources.size(); ++s) {
            const auto& sp = src_snap.subresources[s].pitch;
            D3D12_TEXTURE_COPY_LOCATION dloc{};
            dloc.pResource = host_dst;
            dloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dloc.PlacedFootprint.Offset = 0ull;
            dloc.PlacedFootprint.Footprint.Format = sdesc.Format;
            dloc.PlacedFootprint.Footprint.Width = std::max<UINT>(1u, static_cast<UINT>(sdesc.Width) >> sp.mip_level);
            dloc.PlacedFootprint.Footprint.Height = std::max<UINT>(1u, sdesc.Height >> sp.mip_level);
            dloc.PlacedFootprint.Footprint.Depth = 1u;
            dloc.PlacedFootprint.Footprint.RowPitch = sp.linear_row_pitch_bytes;

            D3D12_TEXTURE_COPY_LOCATION sloc{};
            sloc.pResource = host_src;
            sloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sloc.SubresourceIndex = static_cast<UINT>(s);
            if (!TryBridgePlacedTiledCopyTextureRegion(dloc, 0u, 0u, 0u, sloc, nullptr)) {
                return false;
            }
        }
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.map.find(host_src);
        if (it != reg.map.end()) ++it->second.copy_resource_bridge_count;
        return true;
    }

    // Texture <-> Texture where at least one is a placed tiled texture.
    host_list_->CopyResource(host_dst, host_src);
    {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (dst_is_placed) {
            auto it = reg.map.find(host_dst);
            if (it != reg.map.end()) {
                ++it->second.copy_resource_bridge_count;
                for (auto& sr : it->second.subresources) {
                    sr.dirty_for_flush = true;
                    sr.pitch.dirty_for_flush = 1u;
                }
            }
        }
        if (src_is_placed) {
            auto it = reg.map.find(host_src);
            if (it != reg.map.end()) {
                ++it->second.copy_resource_bridge_count;
            }
        }
    }
    return true;
}

bool CommandListX::TryBridgePlacedTiledCopyTiles(
    ID3D12Resource* host_tiled_res,
    const D3D12_TILED_RESOURCE_COORDINATE* start_coord,
    const D3D12_TILE_REGION_SIZE* /*region_size*/,
    ID3D12Resource* host_buffer,
    UINT64 buffer_offset_bytes,
    D3D12_TILE_COPY_FLAGS flags) {
    if (!host_tiled_res || !host_buffer || !start_coord) return false;
    PlacedTiledResourceRecord snap{};
    {
        auto& reg = GetPlacedTiledRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.map.find(host_tiled_res);
        if (it == reg.map.end()) return false;
        snap = it->second;
    }
    const uint32_t sub = start_coord->Subresource;
    if (sub >= snap.subresources.size()) return false;
    const auto& sp = snap.subresources[sub].pitch;

    D3D12_TEXTURE_COPY_LOCATION tex_loc{};
    tex_loc.pResource = host_tiled_res;
    tex_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    tex_loc.SubresourceIndex = sub;

    D3D12_TEXTURE_COPY_LOCATION buf_loc{};
    buf_loc.pResource = host_buffer;
    buf_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf_loc.PlacedFootprint.Offset = buffer_offset_bytes;
    buf_loc.PlacedFootprint.Footprint.Format = snap.res_desc.Format;
    buf_loc.PlacedFootprint.Footprint.Width =
        std::max<UINT>(1u, static_cast<UINT>(snap.res_desc.Width) >> sp.mip_level);
    buf_loc.PlacedFootprint.Footprint.Height =
        std::max<UINT>(1u, snap.res_desc.Height >> sp.mip_level);
    buf_loc.PlacedFootprint.Footprint.Depth = 1u;
    buf_loc.PlacedFootprint.Footprint.RowPitch = sp.linear_row_pitch_bytes;

    const bool to_buffer =
        (flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER) != 0;
    const bool ok = to_buffer
        ? TryBridgePlacedTiledCopyTextureRegion(buf_loc, 0u, 0u, 0u, tex_loc, nullptr)
        : TryBridgePlacedTiledCopyTextureRegion(tex_loc, 0u, 0u, 0u, buf_loc, nullptr);
    if (!ok) return false;

    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.map.find(host_tiled_res);
    if (it != reg.map.end()) {
        ++it->second.copy_tiles_bridge_count;
    }
    return true;
}

void CommandListX::TrackPlacedTiledResourceBarriers(
    UINT num_barriers,
    const D3D12_RESOURCE_BARRIER* unwrapped_barriers,
    std::vector<D3D12_RESOURCE_BARRIER>& extra_coherency_barriers) {
    if (!unwrapped_barriers || num_barriers == 0u) return;
    auto& reg = GetPlacedTiledRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    if (reg.map.empty()) return;

    auto flush_subresource = [&](PlacedTiledResourceRecord& rec, uint32_t sub_idx,
                                 D3D12_RESOURCE_STATES new_state) {
        if (sub_idx >= rec.subresources.size()) return;
        auto& sr = rec.subresources[sub_idx];
        sr.pitch.current_state = static_cast<uint32_t>(new_state);
        if (sr.dirty_for_flush) {
            sr.dirty_for_flush = false;
            sr.pitch.dirty_for_flush = 0u;
            ++rec.coherency_flush_count;
        }
    };

    for (UINT i = 0u; i < num_barriers; ++i) {
        const D3D12_RESOURCE_BARRIER& b = unwrapped_barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            auto it = reg.map.find(b.Transition.pResource);
            if (it == reg.map.end()) continue;
            PlacedTiledResourceRecord& rec = it->second;
            ++rec.barrier_transition_count;
            if (b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                rec.all_sub_state = b.Transition.StateAfter;
                for (uint32_t s = 0u; s < static_cast<uint32_t>(rec.subresources.size()); ++s) {
                    flush_subresource(rec, s, b.Transition.StateAfter);
                }
            } else if (b.Transition.Subresource < rec.subresources.size()) {
                flush_subresource(rec, b.Transition.Subresource, b.Transition.StateAfter);
            }
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            auto touch_alias = [&](ID3D12Resource* res) {
                if (!res) return;
                auto it = reg.map.find(res);
                if (it == reg.map.end()) return;
                PlacedTiledResourceRecord& rec = it->second;
                ++rec.aliasing_barrier_count;
                for (auto& sr : rec.subresources) {
                    if (sr.dirty_for_flush) {
                        sr.dirty_for_flush = false;
                        sr.pitch.dirty_for_flush = 0u;
                        ++rec.coherency_flush_count;
                    }
                }
            };
            touch_alias(b.Aliasing.pResourceBefore);
            touch_alias(b.Aliasing.pResourceAfter);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && b.UAV.pResource) {
            auto it = reg.map.find(b.UAV.pResource);
            if (it == reg.map.end()) continue;
            PlacedTiledResourceRecord& rec = it->second;
            for (auto& sr : rec.subresources) {
                if (sr.dirty_for_flush) {
                    sr.dirty_for_flush = false;
                    sr.pitch.dirty_for_flush = 0u;
                    ++rec.coherency_flush_count;
                }
            }
        }
    }
    (void)extra_coherency_barriers;
}

}  // namespace openscarlett::d3d12_xs
