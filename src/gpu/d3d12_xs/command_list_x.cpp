#include "command_list_x.h"

#include <algorithm>
#include <cstdio>
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
    std::atomic<uint32_t> count{0u};
};

PlacedTiledRegistry& GetPlacedTiledRegistry() {
    static PlacedTiledRegistry reg;
    return reg;
}

std::atomic<bool> g_retail_fast_cmdlist{false};

inline void RecordList(const char* method, int32_t slot, HRESULT hr = S_OK) {
    if (hr == S_OK &&
        g_retail_fast_cmdlist.load(std::memory_order_relaxed) &&
        !openscarlett::CallTelemetry::IsHookTelemetryActive()) {
        return;
    }
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

std::atomic<void*> g_bink_host_pso{nullptr};

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
        parent_->ExecuteEnhancedBarriers(NumBarrierGroups, pBarrierGroups);
    }
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) override {
        if (parent_->host_list8_) {
            parent_->host_list8_->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);
            RecordList("ID3D12GraphicsCommandListX::OMSetFrontAndBackStencilRef", 81, S_OK);
        } else {
            parent_->RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::OMSetFrontAndBackStencilRef", 81);
        }
    }
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) override {
        if (parent_->host_list9_) {
            parent_->host_list9_->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
            RecordList("ID3D12GraphicsCommandListX::RSSetDepthBias", 82, S_OK);
        } else {
            parent_->RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::RSSetDepthBias", 82);
        }
    }
    void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) override {
        if (parent_->host_list9_) {
            parent_->host_list9_->IASetIndexBufferStripCutValue(IBStripCutValue);
            RecordList("ID3D12GraphicsCommandListX::IASetIndexBufferStripCutValue", 83, S_OK);
        } else {
            parent_->RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::IASetIndexBufferStripCutValue", 83);
        }
    }
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC* pDesc) override {
        if (parent_->host_list10_) {
            parent_->host_list10_->SetProgram(pDesc);
            RecordList("ID3D12GraphicsCommandListX::SetProgram", 84, S_OK);
        } else {
            parent_->RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::SetProgram", 84);
        }
    }
    void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC* pDesc) override {
        if (parent_->host_list10_) {
            parent_->host_list10_->DispatchGraph(pDesc);
            RecordList("ID3D12GraphicsCommandListX::DispatchGraph", 85, S_OK);
        } else {
            parent_->RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::DispatchGraph", 85);
        }
    }
#endif

private:
    CommandListX* parent_ = nullptr;
};
#endif  // __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__

CommandListX::CommandListX(DeviceX* parent_device, ID3D12GraphicsCommandList* host_list)
    : device_x_(parent_device), host_list_(host_list) {
    if (device_x_) {
        device_x_->AddRef();
        if (device_x_->IsRetailScarlettAbi()) {
            g_retail_fast_cmdlist.store(true, std::memory_order_relaxed);
        }
    }
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
    for (uint32_t i = 0u; i < kRetailPrivateAllocRingSize; ++i) {
        if (retail_private_allocs_[i]) {
            retail_private_allocs_[i]->Release();
            retail_private_allocs_[i] = nullptr;
        }
    }
    ReleaseAlignedStagingRing();
    ReleaseTiledTransientResources();
    ReleaseAllAlignTransientResources();
    RetireUploadArenaPages(/*immediate_release=*/false);
    if (ei_sync_dummy_buf_) {
        ei_sync_dummy_buf_->Release();
        ei_sync_dummy_buf_ = nullptr;
    }
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

void CommandListX::RegisterExternalAlias(const void* alias_ptr, CommandListX* wrapper) noexcept {
    if (!alias_ptr || !wrapper) return;
    auto& reg = GetCommandListRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map[alias_ptr] = wrapper->HostList();
    reg.wrapper_map[alias_ptr] = wrapper;
}

void CommandListX::UnregisterExternalAlias(const void* alias_ptr) noexcept {
    if (!alias_ptr) return;
    auto& reg = GetCommandListRegistry();
    std::unique_lock<std::shared_mutex> lock(reg.mutex);
    reg.map.erase(alias_ptr);
    reg.wrapper_map.erase(alias_ptr);
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
    if (IsEqualIID(riid, IID_IOpenScarlettEnhancedBarrierStats)) {
        *ppvObject = static_cast<IOpenScarlettEnhancedBarrierStats*>(this);
        AddRef();
        RecordList("ID3D12GraphicsCommandListX::QueryInterface", 0, S_OK);
        return S_OK;
    }
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    if (IsEqualIID(riid, OS_D3D12_IID(ID3D12GraphicsCommandList7)) && ext_proxy_) {
        // Expose CommandList7 via ext_proxy_ even on inbox D3D12 without native host_list7_,
        // since ExecuteEnhancedBarriers() unwraps proxies and cleanly falls back to legacy ResourceBarrier.
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
    if (device_x_ && device_x_->IsRetailScarlettAbi() && last_bound_rtv0_res_ != nullptr) {
        ID3D12Resource* rtv_res = last_bound_rtv0_res_;
        last_bound_rtv0_res_ = nullptr;
        const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
            rtv_res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (before != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource = rtv_res;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = before;
            tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            host_list_->ResourceBarrier(1u, &tb);
        }
    }
    const HRESULT hr = host_list_->Close();
    is_closed_.store(true, std::memory_order_release);
    if (device_x_) {
        device_x_->CheckCpuDeviceRemoved(
            "CommandListX::Close", static_cast<uint64_t>(hr), static_cast<uint64_t>(list_type_));
    }
    last_close_failed_.store(FAILED(hr), std::memory_order_release);
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Close() -> hr=0x%08X", static_cast<unsigned>(hr));
        AppendDiagOp(buf);
    }
    if (FAILED(hr) && device_x_ && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
        std::fprintf(stderr, "[OpenScarlett][d3d12_xs] CommandListX::Close FAILED hr=0x%08lX type=%d\n",
                     static_cast<unsigned long>(hr), static_cast<int>(list_type_));
        const auto ops = CopyDiagOps();
        if (!ops.empty()) {
            const size_t start_op = (ops.size() > 64u) ? (ops.size() - 64u) : 0u;
            for (size_t k = start_op; k < ops.size(); ++k) {
                std::fprintf(stderr, "  [DiagOp #%zu] %s\n", k, ops[k].c_str());
            }
        }
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(device_x_->HostDevice()->QueryInterface(
                OS_D3D12_IID(ID3D12InfoQueue), reinterpret_cast<void**>(&iq))) && iq) {
            const UINT64 n = iq->GetNumStoredMessages();
            std::fprintf(stderr, "[OpenScarlett][d3d12_xs] InfoQueue messages=%llu\n",
                         static_cast<unsigned long long>(n));
            const UINT64 start = (n > 25ull) ? (n - 25ull) : 0ull;
            for (UINT64 i = start; i < n; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                if (len > 0) {
                    std::vector<uint8_t> buf(len);
                    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                    if (SUCCEEDED(iq->GetMessage(i, msg, &len)) && msg->pDescription) {
                        std::fprintf(stderr, "  [D3D12 sev=%d id=%d] %s\n",
                                     static_cast<int>(msg->Severity),
                                     static_cast<int>(msg->ID),
                                     msg->pDescription);
                    }
                }
            }
            iq->Release();
        }
        std::fflush(stderr);
    }
    RecordList("ID3D12GraphicsCommandListX::Close", 9, hr);
    return hr;
}

bool CommandListX::IsDiagTraceEnabled() noexcept {
    static const bool enabled = (DiagTraceLogPath()[0] != '\0');
    return enabled;
}

const char* CommandListX::DiagTraceLogPath() noexcept {
    static const std::string path = []() {
        char buf[512]{};
        const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_D3D12_LOG", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            return std::string(buf, n);
        }
        return std::string{};
    }();
    return path.c_str();
}

void CommandListX::AppendDiagOp(std::string op) {
    std::lock_guard<std::mutex> lk(diag_ops_mutex_);
    if (diag_ops_.size() < 512u) {
        diag_ops_.push_back(std::move(op));
    }
}

std::vector<std::string> CommandListX::CopyDiagOps() const {
    std::lock_guard<std::mutex> lk(diag_ops_mutex_);
    return diag_ops_;
}

void CommandListX::ExtractTransientResources(std::vector<ID3D12Resource*>& out) {
    std::lock_guard<std::mutex> lk(tiled_transient_mutex_);
    if (!tiled_transient_resources_.empty()) {
        out.insert(out.end(), tiled_transient_resources_.begin(), tiled_transient_resources_.end());
        tiled_transient_resources_.clear();
    }
}

HRESULT STDMETHODCALLTYPE CommandListX::Reset(
    ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) {
    last_close_failed_.store(false, std::memory_order_release);
    is_closed_.store(false, std::memory_order_release);
    RetireUploadArenaPages(/*immediate_release=*/false);
    last_pso_is_standin_ = false;
    last_gfx_rs_is_empty_ = false;
    last_cmp_rs_is_empty_ = false;
    last_pso_proxy_ = nullptr;
    last_bound_rtv_count_ = 0u;
    std::memset(last_bound_rtv_formats_, 0, sizeof(last_bound_rtv_formats_));
    last_bound_dsv_format_ = DXGI_FORMAT_UNKNOWN;
    guest_set_stencil_ref_ = false;
    guest_stencil_ref_ = 0u;
    if (pInitialState && IsKnownProxyPointer(pInitialState)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pInitialState->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::PipelineState) {
                auto* pso_proxy = static_cast<PipelineStateXProxy*>(pInitialState);
                pso_proxy->EnsureLiftedOnBind();
                last_pso_is_standin_ = pso_proxy->IsStandinPso();
                last_pso_proxy_ = pso_proxy;
            }
            insp->Release();
        }
    }
    if (IsDiagTraceEnabled()) {
        std::lock_guard<std::mutex> lk(diag_ops_mutex_);
        diag_ops_.clear();
    }
    ReleaseTiledTransientResources();
    aligned_staging_slot_idx_ = (aligned_staging_slot_idx_ + 1u) % kAlignedStagingRingSlots;
    {
        auto& slot = aligned_staging_ring_[aligned_staging_slot_idx_];
        slot.cursor = 0ull;
        slot.state = D3D12_RESOURCE_STATE_COMMON;
        for (ID3D12Resource* old_res : slot.retired_buffers) {
            if (old_res) old_res->Release();
        }
        slot.retired_buffers.clear();
    }
    RotateAlignTransientRing();
    depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bias_clamp_.store(0.0f, std::memory_order_relaxed);
    slope_scaled_depth_bias_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_min_.store(0.0f, std::memory_order_relaxed);
    depth_bounds_max_.store(1.0f, std::memory_order_relaxed);
    view_instance_mask_.store(0u, std::memory_order_relaxed);
    last_host_compute_rs_ = nullptr;
    last_host_graphics_rs_ = nullptr;
    last_cmp_rs_is_empty_ = false;
    last_gfx_rs_is_empty_ = false;
    last_bound_rtv0_res_ = nullptr;
    last_bound_rtv_count_ = 0u;
    std::memset(last_bound_rtv_formats_, 0, sizeof(last_bound_rtv_formats_));
    last_bound_dsv_format_ = DXGI_FORMAT_UNKNOWN;
    guest_set_stencil_ref_ = false;
    guest_stencil_ref_ = 0u;
    last_pso_is_standin_ = false;
    last_pso_proxy_ = nullptr;
    ID3D12PipelineState* init_host_pso = nullptr;
    if (pInitialState && IsKnownProxyPointer(pInitialState)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pInitialState->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::PipelineState) {
                auto* pso_proxy = static_cast<PipelineStateXProxy*>(pInitialState);
                pso_proxy->EnsureLiftedOnBind();
                last_pso_is_standin_ = pso_proxy->IsStandinPso();
                last_pso_proxy_ = pso_proxy;
                init_host_pso = pso_proxy->HostPipelineState();
            }
            insp->Release();
        }
    }
    if (!init_host_pso) {
        init_host_pso = UnwrapPipelineState(pInitialState);
    }
    last_host_pso_ = init_host_pso;
    bound_cbv_srv_uav_heap_ = nullptr;
    bound_sampler_heap_ = nullptr;
    last_raw_topology_ = 0u;
    last_vb0_raw_ = {};
    last_vb0_host_ = {};
    std::memset(last_vb_raw_, 0, sizeof(last_vb_raw_));
    std::memset(last_vb_host_, 0, sizeof(last_vb_host_));
    std::memset(last_vb_need_upload_, 0, sizeof(last_vb_need_upload_));
    last_vb_valid_mask_ = 0u;
    last_ib_raw_ = {};
    last_ib_host_ = {};
    last_ib_need_upload_ = false;
    std::memset(last_gfx_root_srv_raw_, 0, sizeof(last_gfx_root_srv_raw_));
    std::memset(last_gfx_root_srv_host_, 0, sizeof(last_gfx_root_srv_host_));
    {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        written_mirrored_vas_.clear();
        deferred_cpu_writes_.clear();
        deferred_cpu_waits_.clear();
        deferred_state_transitions_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(ext_state_mutex_);
        sample_positions_per_pixel_ = 0u;
        sample_positions_pixels_ = 0u;
        sample_positions_.clear();
    }
    ID3D12PipelineState* host_pso = init_host_pso;
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
    ID3D12CommandAllocator* use_alloc = UnwrapCommandAllocator(pAllocator);
    if (device_x_ && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice()) {
        // Xbox Scarlett allows multiple ID3D12GraphicsCommandList instances to record
        // simultaneously on the same ID3D12CommandAllocator; PC D3D12 forbids concurrent
        // recording on a shared allocator (causing pushbuffer corruption -> DXGI_ERROR_DEVICE_HUNG).
        // Isolate each retail CommandListX onto its own private per-list allocator ring.
        const uint32_t slot = retail_private_alloc_idx_;
        retail_private_alloc_idx_ = (retail_private_alloc_idx_ + 1u) % kRetailPrivateAllocRingSize;
        if (!retail_private_allocs_[slot]) {
            (void)device_x_->HostDevice()->CreateCommandAllocator(
                list_type_, OS_D3D12_IID(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&retail_private_allocs_[slot]));
        }
        if (retail_private_allocs_[slot]) {
            (void)retail_private_allocs_[slot]->Reset();
            use_alloc = retail_private_allocs_[slot];
        }
    }
    HRESULT hr = host_list_->Reset(use_alloc, last_pso_is_standin_ ? nullptr : host_pso);
    if (FAILED(hr) && device_x_ && device_x_->IsRetailScarlettAbi() && device_x_->HostDevice() && use_alloc) {
        // Recover cleanly if a prior Close() returned E_INVALIDARG and left host_list_ open/poisoned.
        ID3D12GraphicsCommandList* fresh_list = nullptr;
        if (SUCCEEDED(device_x_->HostDevice()->CreateCommandList(
                0u, list_type_, use_alloc, last_pso_is_standin_ ? nullptr : host_pso,
                OS_D3D12_IID(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&fresh_list))) &&
            fresh_list) {
            if (host_list_) host_list_->Release();
            host_list_ = fresh_list;
            if (host_list1_) { host_list1_->Release(); host_list1_ = nullptr; }
            if (host_list2_) { host_list2_->Release(); host_list2_ = nullptr; }
            if (host_list3_) { host_list3_->Release(); host_list3_ = nullptr; }
            if (host_list4_) { host_list4_->Release(); host_list4_ = nullptr; }
            if (host_list5_) { host_list5_->Release(); host_list5_ = nullptr; }
            if (host_list6_) { host_list6_->Release(); host_list6_ = nullptr; }
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
            if (host_list7_) { host_list7_->Release(); host_list7_ = nullptr; }
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList7), reinterpret_cast<void**>(&host_list7_));
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
            if (host_list8_) { host_list8_->Release(); host_list8_ = nullptr; }
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList8), reinterpret_cast<void**>(&host_list8_));
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
            if (host_list9_) { host_list9_->Release(); host_list9_ = nullptr; }
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList9), reinterpret_cast<void**>(&host_list9_));
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
            if (host_list10_) { host_list10_->Release(); host_list10_ = nullptr; }
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList10), reinterpret_cast<void**>(&host_list10_));
#endif
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList1), reinterpret_cast<void**>(&host_list1_));
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList2), reinterpret_cast<void**>(&host_list2_));
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList3), reinterpret_cast<void**>(&host_list3_));
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&host_list4_));
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList5), reinterpret_cast<void**>(&host_list5_));
            (void)host_list_->QueryInterface(OS_D3D12_IID(ID3D12GraphicsCommandList6), reinterpret_cast<void**>(&host_list6_));
            {
                auto& reg = GetCommandListRegistry();
                std::unique_lock<std::shared_mutex> rlock(reg.mutex);
                for (const auto& kv : reg.wrapper_map) {
                    if (kv.second == this) {
                        reg.map[kv.first] = host_list_;
                    }
                }
            }
            hr = S_OK;
        }
    }
    if (IsDiagTraceEnabled()) {
        static std::atomic<uint32_t> s_rst_log{0u};
        if (s_rst_log.fetch_add(1u, std::memory_order_relaxed) < 12u) {
            std::fprintf(stderr,
                         "[OpenScarlett][d3d12_xs] CmdList::Reset(list=%p alloc=%p->%p init=%p) -> hr=0x%08lX\n",
                         static_cast<void*>(host_list_),
                         static_cast<void*>(pAllocator),
                         static_cast<void*>(use_alloc),
                         static_cast<void*>(pInitialState),
                         static_cast<unsigned long>(hr));
            std::fflush(stderr);
        }
    }
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
    last_host_graphics_rs_ = nullptr;
    last_bound_rtv0_res_ = nullptr;
    last_bound_rtv_count_ = 0u;
    std::memset(last_bound_rtv_formats_, 0, sizeof(last_bound_rtv_formats_));
    last_bound_dsv_format_ = DXGI_FORMAT_UNKNOWN;
    guest_set_stencil_ref_ = false;
    guest_stencil_ref_ = 0u;
    last_raw_topology_ = 0u;
    last_vb0_raw_ = {};
    last_vb0_host_ = {};
    std::memset(last_vb_raw_, 0, sizeof(last_vb_raw_));
    std::memset(last_vb_host_, 0, sizeof(last_vb_host_));
    std::memset(last_vb_need_upload_, 0, sizeof(last_vb_need_upload_));
    last_vb_valid_mask_ = 0u;
    last_ib_raw_ = {};
    last_ib_host_ = {};
    last_ib_need_upload_ = false;
    std::memset(last_gfx_root_srv_raw_, 0, sizeof(last_gfx_root_srv_raw_));
    std::memset(last_gfx_root_srv_host_, 0, sizeof(last_gfx_root_srv_host_));
    last_pso_proxy_ = nullptr;
    if (pPipelineState && IsKnownProxyPointer(pPipelineState)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pPipelineState->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::PipelineState) {
                last_pso_proxy_ = static_cast<PipelineStateXProxy*>(pPipelineState);
            }
            insp->Release();
        }
    }
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

namespace {
bool SafeCopyBytes(void* dst, const void* src, size_t len) noexcept {
    if (!dst || !src || len == 0u) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, len);
    return true;
#endif
}

void ResolveRectListVertexLayout(
    DeviceX* device_x,
    ID3D12PipelineState* host_pso,
    uint32_t vb_stride,
    const uint8_t* sample_verts,
    size_t sample_bytes,
    uint32_t& out_pos_off,
    uint32_t& out_pos_comps,
    uint32_t& out_non_float_mask) noexcept {
    out_pos_off = 0u;
    out_pos_comps = (vb_stride == 24u) ? 3u : 2u;
    out_non_float_mask = (vb_stride == 24u) ? (1u << 3u) : 0u;
    if (device_x && device_x->LookupPsoVertexLayout(host_pso, out_pos_off, out_pos_comps, out_non_float_mask)) {
        if (out_pos_off + 8u > vb_stride) out_pos_off = 0u;
        return;
    }
    if (sample_verts && sample_bytes >= vb_stride) {
        if (vb_stride == 20u) {
            uint32_t w0 = 0u, w3 = 0u;
            std::memcpy(&w0, sample_verts + 0u, 4u);
            std::memcpy(&w3, sample_verts + 12u, 4u);
            if ((w0 & 0x7F800000u) == 0x7F800000u && (w3 & 0x7F800000u) != 0x7F800000u) {
                out_pos_off = 12u;
                out_pos_comps = 2u;
                out_non_float_mask = 0x1u;
            }
        } else if (vb_stride == 16u) {
            uint32_t w0 = 0u, w2 = 0u;
            std::memcpy(&w0, sample_verts + 0u, 4u);
            std::memcpy(&w2, sample_verts + 8u, 4u);
            if ((w0 & 0x7F800000u) == 0x7F800000u && (w2 & 0x7F800000u) != 0x7F800000u) {
                out_pos_off = 8u;
                out_pos_comps = 2u;
                out_non_float_mask = 0x3u;
            }
        } else if (vb_stride == 12u) {
            uint32_t w0 = 0u, w1 = 0u;
            std::memcpy(&w0, sample_verts + 0u, 4u);
            std::memcpy(&w1, sample_verts + 4u, 4u);
            if ((w0 & 0x7F800000u) == 0x7F800000u && (w1 & 0x7F800000u) != 0x7F800000u) {
                out_pos_off = 4u;
                out_pos_comps = 2u;
                out_non_float_mask = 0x1u;
            }
        }
    }
    if (out_pos_off + 8u > vb_stride) out_pos_off = 0u;
}

void ExpandRectListVerticesToTriangles(
    const uint8_t* src_buf,
    uint32_t num_rects,
    uint32_t vb_stride,
    uint32_t pos_off,
    uint32_t pos_comps,
    uint32_t non_float_mask,
    std::vector<uint8_t>& dst_buf) {
    dst_buf.resize(static_cast<size_t>(num_rects) * 6u * vb_stride);
    for (uint32_t r = 0u; r < num_rects; ++r) {
        const uint8_t* sv = src_buf + static_cast<size_t>(r) * 3u * vb_stride;
        uint8_t* dv = dst_buf.data() + static_cast<size_t>(r) * 6u * vb_stride;
        std::memcpy(dv, sv, 3u * vb_stride);
        auto rd_f = [&](size_t v, size_t off) -> float {
            float val = 0.0f;
            std::memcpy(&val, sv + v * vb_stride + off, 4u);
            return val;
        };
        auto edge_vec = [&](size_t from, size_t to, double& ex, double& ey, double& ez) {
            ex = static_cast<double>(rd_f(to, pos_off)) - static_cast<double>(rd_f(from, pos_off));
            ey = static_cast<double>(rd_f(to, pos_off + 4u)) - static_cast<double>(rd_f(from, pos_off + 4u));
            ez = (pos_comps >= 3u && pos_off + 12u <= vb_stride)
                     ? (static_cast<double>(rd_f(to, pos_off + 8u)) - static_cast<double>(rd_f(from, pos_off + 8u)))
                     : 0.0;
        };
        auto corner_cos2 = [&](size_t c, size_t a, size_t b) -> double {
            double ax = 0.0, ay = 0.0, az = 0.0;
            double bx = 0.0, by = 0.0, bz = 0.0;
            edge_vec(c, a, ax, ay, az);
            edge_vec(c, b, bx, by, bz);
            const double la2 = ax * ax + ay * ay + az * az;
            const double lb2 = bx * bx + by * by + bz * bz;
            if (la2 <= 1e-24 || lb2 <= 1e-24) return 2.0;
            const double dot = ax * bx + ay * by + az * bz;
            return (dot * dot) / (la2 * lb2);
        };
        const double c0 = corner_cos2(0u, 1u, 2u);
        const double c1 = corner_cos2(1u, 0u, 2u);
        const double c2 = corner_cos2(2u, 0u, 1u);
        size_t ca = 1u, cb = 2u, cc = 0u;
        size_t v3_dst = 4u;
        if (c1 <= c0 && c1 <= c2) {
            ca = 0u; cb = 2u; cc = 1u; v3_dst = 5u;
            std::memcpy(dv + 3u * vb_stride, sv + 0u * vb_stride, vb_stride);
            std::memcpy(dv + 4u * vb_stride, sv + 2u * vb_stride, vb_stride);
            std::memcpy(dv + 5u * vb_stride, sv + 1u * vb_stride, vb_stride);
        } else if (c2 <= c0 && c2 <= c1) {
            ca = 0u; cb = 1u; cc = 2u; v3_dst = 4u;
            std::memcpy(dv + 3u * vb_stride, sv + 0u * vb_stride, vb_stride);
            std::memcpy(dv + 4u * vb_stride, sv + 2u * vb_stride, vb_stride);
            std::memcpy(dv + 5u * vb_stride, sv + 1u * vb_stride, vb_stride);
        } else {
            ca = 1u; cb = 2u; cc = 0u; v3_dst = 4u;
            std::memcpy(dv + 3u * vb_stride, sv + 1u * vb_stride, vb_stride);
            std::memcpy(dv + 4u * vb_stride, sv + 0u * vb_stride, vb_stride);
            std::memcpy(dv + 5u * vb_stride, sv + 2u * vb_stride, vb_stride);
        }
        for (size_t off = 0u; off < vb_stride; off += 4u) {
            uint32_t u0 = 0u, u1 = 0u, u2 = 0u;
            std::memcpy(&u0, sv + 0u * vb_stride + off, 4u);
            std::memcpy(&u1, sv + 1u * vb_stride + off, 4u);
            std::memcpy(&u2, sv + 2u * vb_stride + off, 4u);
            if (u0 == u1 && u1 == u2) continue;
            const uint32_t dw_idx = static_cast<uint32_t>(off >> 2u);
            const bool is_non_float =
                (dw_idx < 32u && ((non_float_mask & (1u << dw_idx)) != 0u)) ||
                ((u0 & 0x7F800000u) == 0x7F800000u) ||
                ((u1 & 0x7F800000u) == 0x7F800000u) ||
                ((u2 & 0x7F800000u) == 0x7F800000u);
            if (is_non_float) {
                uint8_t b3[4]{};
                const uint8_t* ba = sv + ca * vb_stride + off;
                const uint8_t* bb = sv + cb * vb_stride + off;
                const uint8_t* bc = sv + cc * vb_stride + off;
                for (size_t bi = 0u; bi < 4u; ++bi) {
                    const int v = static_cast<int>(ba[bi]) + static_cast<int>(bb[bi]) -
                                  static_cast<int>(bc[bi]);
                    b3[bi] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                }
                std::memcpy(dv + v3_dst * vb_stride + off, b3, 4u);
            } else {
                const float f3 = rd_f(ca, off) + rd_f(cb, off) - rd_f(cc, off);
                std::memcpy(dv + v3_dst * vb_stride + off, &f3, 4u);
            }
        }
    }
}
}  // namespace

void STDMETHODCALLTYPE CommandListX::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount,
    UINT StartVertexLocation, UINT StartInstanceLocation) {
    if (VertexCountPerInstance == 0u || InstanceCount == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::DrawInstanced[ZeroNoop]", 12, S_OK);
        return;
    }
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
    const bool pso_missing =
        device_x_ && device_x_->IsRetailScarlettAbi() && last_host_pso_ == nullptr;
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "DrawInstanced(v=%u, i=%u, standin=%d, no_pso=%d)",
                      VertexCountPerInstance, InstanceCount,
                      last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
        AppendDiagOp(buf);
    }
    if (!skip_host && !last_pso_is_standin_ && !pso_missing) {
        const bool want_zero_ia_rectlist =
            device_x_ && device_x_->IsRetailScarlettAbi() &&
            (last_raw_topology_ == 17u || last_raw_topology_ == 19u) &&
            VertexCountPerInstance >= 3u && (VertexCountPerInstance % 3u) == 0u &&
            last_pso_proxy_ != nullptr &&
            last_pso_proxy_->PendingXdatDesc() != nullptr &&
            last_pso_proxy_->PendingXdatDesc()->il_elems.empty();
        if (last_pso_proxy_ != nullptr) {
            ID3D12PipelineState* target_pso = last_pso_proxy_->GetHostPsoForTargets(
                last_bound_rtv_count_, last_bound_rtv_formats_, last_bound_dsv_format_,
                want_zero_ia_rectlist);
            if (target_pso && target_pso != last_host_pso_) {
                host_list_->SetPipelineState(target_pso);
                last_host_pso_ = target_pso;
            }
            uint8_t s57_ref = 0u;
            if (!guest_set_stencil_ref_ && last_pso_proxy_->HasSlot57StencilRef(s57_ref)) {
                host_list_->OMSetStencilRef(s57_ref);
            }
        }
        // Xbox Scarlett RECTLIST (topology 17/19) draws each rectangle from 3 vertices (V0,V1,V2)
        // with implicit 4th corner V3 = Va + Vb - Vc; QUADLIST (topology 18) draws 4-vertex quads;
        // TRIANGLEFAN (topology 6/20) draws N-2 triangles sharing V0.
        bool drew_rectlist_quad = false;
        const bool zero_ia_pso = device_x_ && device_x_->PsoUsesZeroIaInputs(last_host_pso_);
        if (want_zero_ia_rectlist &&
            !last_pso_proxy_->PendingXdatDesc()->lifted_rectlist_vs_dxil.empty()) {
            host_list_->DrawInstanced(
                (VertexCountPerInstance / 3u) * 6u,
                InstanceCount,
                (StartVertexLocation / 3u) * 6u,
                StartInstanceLocation);
            drew_rectlist_quad = true;
        }
        const UINT vb_stride = last_vb0_raw_.StrideInBytes;
        if (!drew_rectlist_quad && zero_ia_pso && device_x_->IsRetailScarlettAbi() &&
            (last_raw_topology_ == 17u || last_raw_topology_ == 19u) &&
            VertexCountPerInstance >= 3u && (VertexCountPerInstance % 3u) == 0u &&
            VertexCountPerInstance <= 4095u &&
            last_gfx_root_srv_raw_[0] != 0ull) {
            // Software-vertex-fetch RECTLIST PSO (e.g. RAGE9 VS_Clear InputLayout=0 reading stream
            // headers at +0x0 and +0xC from SetGraphicsRootShaderResourceView(0, ...)).
            const uint64_t srv0_cpu = device_x_->ResolveCpuAliasVa(last_gfx_root_srv_raw_[0]);
            uint8_t hdr[64]{};
            if (SafeCopyBytes(hdr, reinterpret_cast<const void*>(static_cast<uintptr_t>(srv0_cpu)), 32u)) {
                uint32_t d0 = 0u, d3 = 0u;
                std::memcpy(&d0, hdr + 0u, 4u);
                std::memcpy(&d3, hdr + 12u, 4u);
                const uint32_t stride0 = (d0 >> 3u) & 60u;
                const uint32_t off0 = (d0 >> 7u) & 0x1FFFFFCu;
                const uint32_t stride1 = (d3 >> 3u) & 60u;
                const uint32_t off1 = (d3 >> 7u) & 0x1FFFFFCu;
                if (stride0 >= 8u && stride0 <= 64u && (stride0 & 3u) == 0u &&
                    off0 >= 16u && off0 <= 65536u) {
                    const uint32_t num_rects = VertexCountPerInstance / 3u;
                    const size_t s0_src_bytes = static_cast<size_t>(VertexCountPerInstance) * stride0;
                    const uint64_t s0_cpu =
                        srv0_cpu + off0 + static_cast<uint64_t>(StartVertexLocation) * stride0;
                    std::vector<uint8_t> s0_src(s0_src_bytes);
                    if (SafeCopyBytes(s0_src.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(s0_cpu)), s0_src_bytes)) {
                        const bool has_s1 =
                            stride1 >= 4u && stride1 <= 64u && (stride1 & 3u) == 0u &&
                            off1 >= 16u && off1 <= 65536u;
                        std::vector<uint8_t> s1_src;
                        bool s1_ok = false;
                        if (has_s1) {
                            const size_t s1_src_bytes = static_cast<size_t>(VertexCountPerInstance) * stride1;
                            const uint64_t s1_cpu =
                                srv0_cpu + off1 + static_cast<uint64_t>(StartVertexLocation) * stride1;
                            s1_src.resize(s1_src_bytes);
                            s1_ok = SafeCopyBytes(
                                s1_src.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(s1_cpu)), s1_src_bytes);
                        }
                        const uint32_t hdr_copy = std::min<uint32_t>(std::min<uint32_t>(off0, has_s1 ? off1 : off0), 64u);
                        (void)SafeCopyBytes(hdr, reinterpret_cast<const void*>(static_cast<uintptr_t>(srv0_cpu)), hdr_copy);
                        const uint32_t new_off0 = (hdr_copy + 15u) & ~15u;
                        const uint32_t s0_dst_bytes = num_rects * 6u * stride0;
                        const uint32_t new_off1 = (new_off0 + s0_dst_bytes + 15u) & ~15u;
                        const uint32_t s1_dst_bytes = s1_ok ? (num_rects * 6u * stride1) : 0u;
                        std::vector<uint8_t> exp_pkg(new_off1 + s1_dst_bytes, 0u);
                        std::memcpy(exp_pkg.data(), hdr, hdr_copy);
                        const uint32_t new_d0 = (d0 & 0x1FFu) | ((new_off0 & 0x1FFFFFCu) << 7u);
                        std::memcpy(exp_pkg.data() + 0u, &new_d0, 4u);
                        if (s1_ok) {
                            const uint32_t new_d3 = (d3 & 0x1FFu) | ((new_off1 & 0x1FFFFFCu) << 7u);
                            std::memcpy(exp_pkg.data() + 12u, &new_d3, 4u);
                        }
                        std::vector<uint8_t> s0_dst;
                        ExpandRectListVerticesToTriangles(
                            s0_src.data(), num_rects, stride0, 0u, (stride0 >= 12u ? 3u : 2u), 0u, s0_dst);
                        std::memcpy(exp_pkg.data() + new_off0, s0_dst.data(), s0_dst.size());
                        if (s1_ok) {
                            // Apply the exact same corner mapping per rectangle from stream 0 to stream 1.
                            for (uint32_t r = 0u; r < num_rects; ++r) {
                                const uint8_t* p0 = s0_src.data() + static_cast<size_t>(r) * 3u * stride0;
                                auto rf0 = [&](size_t v, size_t o) -> double {
                                    float fv = 0.0f;
                                    std::memcpy(&fv, p0 + v * stride0 + o, 4u);
                                    return static_cast<double>(fv);
                                };
                                auto c_cos2 = [&](size_t c, size_t a, size_t b) -> double {
                                    const double ax = rf0(a, 0u) - rf0(c, 0u), ay = rf0(a, 4u) - rf0(c, 4u);
                                    const double bx = rf0(b, 0u) - rf0(c, 0u), by = rf0(b, 4u) - rf0(c, 4u);
                                    const double la2 = ax * ax + ay * ay, lb2 = bx * bx + by * by;
                                    if (la2 <= 1e-24 || lb2 <= 1e-24) return 2.0;
                                    const double dot = ax * bx + ay * by;
                                    return (dot * dot) / (la2 * lb2);
                                };
                                const double cc0 = c_cos2(0u, 1u, 2u);
                                const double cc1 = c_cos2(1u, 0u, 2u);
                                const double cc2 = c_cos2(2u, 0u, 1u);
                                const uint8_t* sv1 = s1_src.data() + static_cast<size_t>(r) * 3u * stride1;
                                uint8_t* dv1 = exp_pkg.data() + new_off1 + static_cast<size_t>(r) * 6u * stride1;
                                std::memcpy(dv1, sv1, 3u * stride1);
                                size_t ca = 1u, cb = 2u, cc = 0u, v3_dst = 4u;
                                if (cc1 <= cc0 && cc1 <= cc2) {
                                    ca = 0u; cb = 2u; cc = 1u; v3_dst = 5u;
                                    std::memcpy(dv1 + 3u * stride1, sv1 + 0u * stride1, stride1);
                                    std::memcpy(dv1 + 4u * stride1, sv1 + 2u * stride1, stride1);
                                } else if (cc2 <= cc0 && cc2 <= cc1) {
                                    ca = 0u; cb = 1u; cc = 2u; v3_dst = 4u;
                                    std::memcpy(dv1 + 3u * stride1, sv1 + 0u * stride1, stride1);
                                    std::memcpy(dv1 + 5u * stride1, sv1 + 1u * stride1, stride1);
                                } else {
                                    ca = 1u; cb = 2u; cc = 0u; v3_dst = 4u;
                                    std::memcpy(dv1 + 3u * stride1, sv1 + 1u * stride1, stride1);
                                    std::memcpy(dv1 + 5u * stride1, sv1 + 2u * stride1, stride1);
                                }
                                for (size_t off = 0u; off < stride1; off += 4u) {
                                    float fa = 0.0f, fb = 0.0f, fc = 0.0f;
                                    std::memcpy(&fa, sv1 + ca * stride1 + off, 4u);
                                    std::memcpy(&fb, sv1 + cb * stride1 + off, 4u);
                                    std::memcpy(&fc, sv1 + cc * stride1 + off, 4u);
                                    const float f3 = fa + fb - fc;
                                    std::memcpy(dv1 + v3_dst * stride1 + off, &f3, 4u);
                                }
                            }
                        }
                        const D3D12_GPU_VIRTUAL_ADDRESS exp_gpu =
                            device_x_->UploadTransientGuestBufferForDesktop(
                                static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(exp_pkg.data())),
                                exp_pkg.size(), 256ull, /*allow_same_frame_cache=*/false);
                        if (exp_gpu != 0ull) {
                            host_list_->SetGraphicsRootShaderResourceView(0u, exp_gpu);
                            host_list_->DrawInstanced(num_rects * 6u, InstanceCount, 0u, StartInstanceLocation);
                            if (last_gfx_root_srv_host_[0] != 0ull) {
                                host_list_->SetGraphicsRootShaderResourceView(0u, last_gfx_root_srv_host_[0]);
                            }
                            drew_rectlist_quad = true;
                        }
                    }
                }
            }
        }
        if (!zero_ia_pso && device_x_ && device_x_->IsRetailScarlettAbi() &&
            last_vb0_raw_.BufferLocation != 0ull &&
            vb_stride >= 8u && vb_stride <= 128u && (vb_stride & 3u) == 0u) {
            if ((last_raw_topology_ == 17u || last_raw_topology_ == 19u) &&
                VertexCountPerInstance >= 3u && (VertexCountPerInstance % 3u) == 0u &&
                VertexCountPerInstance <= 65535u &&
                last_vb0_raw_.SizeInBytes >= (StartVertexLocation + VertexCountPerInstance) * vb_stride) {
                const uint32_t num_rects = VertexCountPerInstance / 3u;
                const size_t src_bytes = static_cast<size_t>(VertexCountPerInstance) * vb_stride;
                const uint64_t raw_va =
                    last_vb0_raw_.BufferLocation + static_cast<uint64_t>(StartVertexLocation) * vb_stride;
                const uint64_t cpu_va = device_x_->ResolveCpuAliasVa(raw_va);
                std::vector<uint8_t> src_buf(src_bytes);
                if (SafeCopyBytes(src_buf.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu_va)), src_bytes)) {
                    uint32_t pos_off = 0u, pos_comps = 2u, non_float_mask = 0u;
                    ResolveRectListVertexLayout(
                        device_x_, last_host_pso_, vb_stride, src_buf.data(), src_buf.size(),
                        pos_off, pos_comps, non_float_mask);
                    std::vector<uint8_t> dst_buf;
                    ExpandRectListVerticesToTriangles(
                        src_buf.data(), num_rects, vb_stride, pos_off, pos_comps, non_float_mask, dst_buf);
                    const D3D12_GPU_VIRTUAL_ADDRESS quad_gpu =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_buf.data())),
                            dst_buf.size(), 256ull, /*allow_same_frame_cache=*/false);
                    if (quad_gpu != 0ull) {
                        D3D12_VERTEX_BUFFER_VIEW qv{};
                        qv.BufferLocation = quad_gpu;
                        qv.SizeInBytes = static_cast<UINT>(dst_buf.size());
                        qv.StrideInBytes = vb_stride;
                        host_list_->IASetVertexBuffers(0u, 1u, &qv);
                        host_list_->DrawInstanced(num_rects * 6u, InstanceCount, 0u, StartInstanceLocation);
                        host_list_->IASetVertexBuffers(0u, 1u, &last_vb0_host_);
                        drew_rectlist_quad = true;
                    }
                }
            } else if (last_raw_topology_ == 18u &&
                       VertexCountPerInstance >= 4u && (VertexCountPerInstance % 4u) == 0u &&
                       VertexCountPerInstance <= 65536u &&
                       last_vb0_raw_.SizeInBytes >= (StartVertexLocation + VertexCountPerInstance) * vb_stride) {
                const uint32_t num_quads = VertexCountPerInstance / 4u;
                const size_t src_bytes = static_cast<size_t>(VertexCountPerInstance) * vb_stride;
                const size_t dst_bytes = static_cast<size_t>(num_quads) * 6u * vb_stride;
                const uint64_t raw_va =
                    last_vb0_raw_.BufferLocation + static_cast<uint64_t>(StartVertexLocation) * vb_stride;
                const uint64_t cpu_va = device_x_->ResolveCpuAliasVa(raw_va);
                std::vector<uint8_t> src_buf(src_bytes);
                if (SafeCopyBytes(src_buf.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu_va)), src_bytes)) {
                    std::vector<uint8_t> dst_buf(dst_bytes);
                    for (uint32_t q = 0u; q < num_quads; ++q) {
                        const uint8_t* sv = src_buf.data() + static_cast<size_t>(q) * 4u * vb_stride;
                        uint8_t* dv = dst_buf.data() + static_cast<size_t>(q) * 6u * vb_stride;
                        std::memcpy(dv + 0u * vb_stride, sv + 0u * vb_stride, vb_stride);
                        std::memcpy(dv + 1u * vb_stride, sv + 1u * vb_stride, vb_stride);
                        std::memcpy(dv + 2u * vb_stride, sv + 2u * vb_stride, vb_stride);
                        std::memcpy(dv + 3u * vb_stride, sv + 0u * vb_stride, vb_stride);
                        std::memcpy(dv + 4u * vb_stride, sv + 2u * vb_stride, vb_stride);
                        std::memcpy(dv + 5u * vb_stride, sv + 3u * vb_stride, vb_stride);
                    }
                    const D3D12_GPU_VIRTUAL_ADDRESS quad_gpu =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_buf.data())),
                            dst_bytes, 256ull, /*allow_same_frame_cache=*/false);
                    if (quad_gpu != 0ull) {
                        D3D12_VERTEX_BUFFER_VIEW qv{};
                        qv.BufferLocation = quad_gpu;
                        qv.SizeInBytes = static_cast<UINT>(dst_bytes);
                        qv.StrideInBytes = vb_stride;
                        host_list_->IASetVertexBuffers(0u, 1u, &qv);
                        host_list_->DrawInstanced(num_quads * 6u, InstanceCount, 0u, StartInstanceLocation);
                        host_list_->IASetVertexBuffers(0u, 1u, &last_vb0_host_);
                        drew_rectlist_quad = true;
                    }
                }
            } else if ((last_raw_topology_ == 6u || last_raw_topology_ == 20u) &&
                       VertexCountPerInstance >= 3u && VertexCountPerInstance <= 65535u &&
                       last_vb0_raw_.SizeInBytes >= (StartVertexLocation + VertexCountPerInstance) * vb_stride) {
                const uint32_t num_tris = VertexCountPerInstance - 2u;
                const size_t src_bytes = static_cast<size_t>(VertexCountPerInstance) * vb_stride;
                const size_t dst_bytes = static_cast<size_t>(num_tris) * 3u * vb_stride;
                const uint64_t raw_va =
                    last_vb0_raw_.BufferLocation + static_cast<uint64_t>(StartVertexLocation) * vb_stride;
                const uint64_t cpu_va = device_x_->ResolveCpuAliasVa(raw_va);
                std::vector<uint8_t> src_buf(src_bytes);
                if (SafeCopyBytes(src_buf.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(cpu_va)), src_bytes)) {
                    std::vector<uint8_t> dst_buf(dst_bytes);
                    for (uint32_t t = 0u; t < num_tris; ++t) {
                        uint8_t* dv = dst_buf.data() + static_cast<size_t>(t) * 3u * vb_stride;
                        std::memcpy(dv + 0u * vb_stride, src_buf.data() + 0u, vb_stride);
                        std::memcpy(dv + 1u * vb_stride, src_buf.data() + static_cast<size_t>(t + 1u) * vb_stride, vb_stride);
                        std::memcpy(dv + 2u * vb_stride, src_buf.data() + static_cast<size_t>(t + 2u) * vb_stride, vb_stride);
                    }
                    const D3D12_GPU_VIRTUAL_ADDRESS fan_gpu =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_buf.data())),
                            dst_bytes, 256ull, /*allow_same_frame_cache=*/false);
                    if (fan_gpu != 0ull) {
                        D3D12_VERTEX_BUFFER_VIEW fv{};
                        fv.BufferLocation = fan_gpu;
                        fv.SizeInBytes = static_cast<UINT>(dst_bytes);
                        fv.StrideInBytes = vb_stride;
                        host_list_->IASetVertexBuffers(0u, 1u, &fv);
                        host_list_->DrawInstanced(num_tris * 3u, InstanceCount, 0u, StartInstanceLocation);
                        host_list_->IASetVertexBuffers(0u, 1u, &last_vb0_host_);
                        drew_rectlist_quad = true;
                    }
                }
            }
        }
        static const bool s_diag_bink_di = []() {
            char b[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_DIAG_BINK", b, sizeof(b)) > 0 && b[0] == '1';
        }();
        const void* bink_pso = g_bink_host_pso.load(std::memory_order_acquire);
        if (s_diag_bink_di && vb_stride == 24u && bink_pso != nullptr && last_host_pso_ == bink_pso) {
            static std::atomic<uint32_t> s_bdi_cnt{0u};
            if (s_bdi_cnt.fetch_add(1u, std::memory_order_relaxed) < 20u) {
                const D3D12_RESOURCE_DESC rtd = last_bound_rtv0_res_ ? last_bound_rtv0_res_->GetDesc() : D3D12_RESOURCE_DESC{};
                std::fprintf(stderr,
                             "[OpenScarlett][BINK-DI] v=%u inst=%u topo=%u rect=%d pso=%p rtv=%p(%llux%u fmt=%d) vb=0x%llX(sz=%u)",
                             VertexCountPerInstance, InstanceCount, last_raw_topology_,
                             drew_rectlist_quad ? 1 : 0, static_cast<void*>(last_host_pso_),
                             static_cast<void*>(last_bound_rtv0_res_),
                             static_cast<unsigned long long>(rtd.Width), rtd.Height, static_cast<int>(rtd.Format),
                             static_cast<unsigned long long>(last_vb0_raw_.BufferLocation), last_vb0_raw_.SizeInBytes);
                if (device_x_ && last_vb0_raw_.BufferLocation != 0ull && last_vb0_raw_.SizeInBytes >= 72u) {
                    const uint64_t cva = device_x_->ResolveCpuAliasVa(
                        last_vb0_raw_.BufferLocation + static_cast<uint64_t>(StartVertexLocation) * 24ull);
                    uint8_t vraw[96]{};
                    const size_t rlen = std::min<size_t>(sizeof(vraw), static_cast<size_t>(VertexCountPerInstance) * 24u);
                    if (SafeCopyBytes(vraw, reinterpret_cast<const void*>(static_cast<uintptr_t>(cva)), rlen)) {
                        const uint32_t nv = std::min<uint32_t>(VertexCountPerInstance, 4u);
                        for (uint32_t vi = 0u; vi < nv; ++vi) {
                            float px = 0.f, py = 0.f, pz = 0.f, u = 0.f, v = 0.f;
                            uint32_t col = 0u;
                            std::memcpy(&px, vraw + vi * 24u + 0u, 4u);
                            std::memcpy(&py, vraw + vi * 24u + 4u, 4u);
                            std::memcpy(&pz, vraw + vi * 24u + 8u, 4u);
                            std::memcpy(&col, vraw + vi * 24u + 12u, 4u);
                            std::memcpy(&u, vraw + vi * 24u + 16u, 4u);
                            std::memcpy(&v, vraw + vi * 24u + 20u, 4u);
                            std::fprintf(stderr, " v%u(%.2f,%.2f,%.2f c=%08X uv=%.3f,%.3f)", vi, px, py, pz, col, u, v);
                        }
                    }
                }
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
            }
        }
        if (!drew_rectlist_quad) {
            UINT eff_start_vert = StartVertexLocation;
            bool rebound_vb = false;
            if (!zero_ia_pso && device_x_ && device_x_->IsRetailScarlettAbi()) {
                const bool only_vb0 = (last_vb_valid_mask_ & ~1u) == 0u;
                for (uint32_t s = 0u; s < 8u; ++s) {
                    if (!last_vb_need_upload_[s]) continue;
                    const auto& raw_vb = last_vb_raw_[s];
                    if (raw_vb.BufferLocation == 0ull || raw_vb.SizeInBytes == 0u) continue;
                    const uint32_t st = raw_vb.StrideInBytes;
                    uint64_t slice_va = raw_vb.BufferLocation;
                    uint64_t slice_bytes = raw_vb.SizeInBytes;
                    if (st > 0u) {
                        if (only_vb0 && s == 0u) {
                            const uint64_t off_b = static_cast<uint64_t>(StartVertexLocation) * st;
                            const uint64_t need_b = static_cast<uint64_t>(VertexCountPerInstance) * st;
                            if (off_b < raw_vb.SizeInBytes && need_b > 0ull) {
                                slice_va = raw_vb.BufferLocation + off_b;
                                slice_bytes = std::min<uint64_t>(need_b, raw_vb.SizeInBytes - off_b);
                                eff_start_vert = 0u;
                            }
                        } else {
                            const uint64_t max_elems = std::max<uint64_t>(
                                static_cast<uint64_t>(StartVertexLocation) + VertexCountPerInstance,
                                static_cast<uint64_t>(StartInstanceLocation) + InstanceCount);
                            slice_bytes = std::min<uint64_t>(raw_vb.SizeInBytes, max_elems * st);
                        }
                    }
                    slice_bytes = std::min<uint64_t>(slice_bytes, 4ull * 1024ull * 1024ull);
                    const D3D12_GPU_VIRTUAL_ADDRESS up_va =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            slice_va, slice_bytes, 16ull, /*allow_same_frame_cache=*/(slice_bytes > 65536ull));
                    if (up_va != 0ull) {
                        D3D12_VERTEX_BUFFER_VIEW svb{};
                        svb.BufferLocation = up_va;
                        svb.SizeInBytes = static_cast<UINT>(slice_bytes);
                        svb.StrideInBytes = raw_vb.StrideInBytes;
                        host_list_->IASetVertexBuffers(s, 1u, &svb);
                        rebound_vb = true;
                    }
                }
            }
            host_list_->DrawInstanced(
                VertexCountPerInstance, InstanceCount, eff_start_vert, StartInstanceLocation);
            if (rebound_vb) {
                for (uint32_t s = 0u; s < 8u; ++s) {
                    if (last_vb_need_upload_[s] && last_vb_host_[s].BufferLocation != 0ull) {
                        host_list_->IASetVertexBuffers(s, 1u, &last_vb_host_[s]);
                    }
                }
            }
        }
    } else if (IsDiagTraceEnabled() && (last_pso_is_standin_ || pso_missing)) {
        static std::atomic<uint64_t> s_skip_di{0ull};
        const uint64_t n = s_skip_di.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if (n == 1ull || (n % 500ull) == 0ull) {
            std::fprintf(stderr, "[OpenScarlett][CL] DrawInstanced SKIPPED #%llu (v=%u standin=%d no_pso=%d)\n",
                         static_cast<unsigned long long>(n), VertexCountPerInstance,
                         last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
            std::fflush(stderr);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::DrawInstanced", 12, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DrawIndexedInstanced(
    UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation) {
    if (IndexCountPerInstance == 0u || InstanceCount == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::DrawIndexedInstanced[ZeroNoop]", 13, S_OK);
        return;
    }
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
    const bool pso_missing =
        device_x_ && device_x_->IsRetailScarlettAbi() && last_host_pso_ == nullptr;
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "DrawIndexedInstanced(idx=%u, i=%u, standin=%d, no_pso=%d)",
                      IndexCountPerInstance, InstanceCount,
                      last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
        AppendDiagOp(buf);
    }
    if (!skip_host && !last_pso_is_standin_ && !pso_missing) {
        if (device_x_ && last_bound_rtv0_res_) {
            device_x_->SetLastDiiRtv(last_bound_rtv0_res_);
        }
        if (last_pso_proxy_ != nullptr) {
            ID3D12PipelineState* target_pso = last_pso_proxy_->GetHostPsoForTargets(
                last_bound_rtv_count_, last_bound_rtv_formats_, last_bound_dsv_format_);
            if (target_pso && target_pso != last_host_pso_) {
                host_list_->SetPipelineState(target_pso);
                last_host_pso_ = target_pso;
            }
            uint8_t s57_ref = 0u;
            if (!guest_set_stencil_ref_ && last_pso_proxy_->HasSlot57StencilRef(s57_ref)) {
                host_list_->OMSetStencilRef(s57_ref);
            }
        }
        static const bool trace_sf_dii = []() {
            char v[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_SF_DRAW", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        static std::atomic<uint32_t> s_sf_dii_log{0u};
        if (trace_sf_dii && s_sf_dii_log.fetch_add(1u, std::memory_order_relaxed) < 10u) {
            std::fprintf(stderr,
                         "[OpenScarlett][SF-DII] idx=%u inst=%u startIdx=%u baseV=%d pso=%p empty_rs=%d\n",
                         IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                         static_cast<void*>(last_host_pso_), last_gfx_rs_is_empty_ ? 1 : 0);
            std::fflush(stderr);
        }
        bool drew_indexed_quad = false;
        if (device_x_ && device_x_->IsRetailScarlettAbi() &&
            last_ib_raw_.BufferLocation != 0ull &&
            (last_ib_raw_.Format == DXGI_FORMAT_R16_UINT || last_ib_raw_.Format == DXGI_FORMAT_R32_UINT)) {
            const uint32_t idx_bytes = (last_ib_raw_.Format == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
            const size_t src_ib_bytes = static_cast<size_t>(IndexCountPerInstance) * idx_bytes;
            if ((last_raw_topology_ == 17u || last_raw_topology_ == 19u) &&
                IndexCountPerInstance >= 3u && (IndexCountPerInstance % 3u) == 0u &&
                IndexCountPerInstance <= 65535u &&
                last_ib_raw_.SizeInBytes >= static_cast<UINT>(StartIndexLocation) * idx_bytes + src_ib_bytes &&
                last_vb0_raw_.BufferLocation != 0ull &&
                last_vb0_raw_.StrideInBytes >= 8u && last_vb0_raw_.StrideInBytes <= 128u &&
                (last_vb0_raw_.StrideInBytes & 3u) == 0u) {
                const uint32_t vb_stride = last_vb0_raw_.StrideInBytes;
                const uint32_t num_rects = IndexCountPerInstance / 3u;
                const uint64_t ib_cpu = device_x_->ResolveCpuAliasVa(
                    last_ib_raw_.BufferLocation + static_cast<uint64_t>(StartIndexLocation) * idx_bytes);
                const uint64_t vb_cpu_base = device_x_->ResolveCpuAliasVa(last_vb0_raw_.BufferLocation);
                std::vector<uint8_t> src_ib(src_ib_bytes);
                if (SafeCopyBytes(src_ib.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(ib_cpu)), src_ib_bytes)) {
                    std::vector<uint8_t> deindexed_verts(static_cast<size_t>(IndexCountPerInstance) * vb_stride);
                    bool all_v_ok = true;
                    for (uint32_t k = 0u; k < IndexCountPerInstance; ++k) {
                        uint32_t raw_idx = 0u;
                        if (idx_bytes == 4u) {
                            std::memcpy(&raw_idx, src_ib.data() + static_cast<size_t>(k) * 4u, 4u);
                        } else {
                            uint16_t i16 = 0u;
                            std::memcpy(&i16, src_ib.data() + static_cast<size_t>(k) * 2u, 2u);
                            raw_idx = i16;
                        }
                        const int64_t v_idx = static_cast<int64_t>(raw_idx) + static_cast<int64_t>(BaseVertexLocation);
                        if (v_idx < 0 ||
                            (static_cast<uint64_t>(v_idx) + 1ull) * vb_stride > last_vb0_raw_.SizeInBytes ||
                            !SafeCopyBytes(
                                deindexed_verts.data() + static_cast<size_t>(k) * vb_stride,
                                reinterpret_cast<const void*>(static_cast<uintptr_t>(
                                    vb_cpu_base + static_cast<uint64_t>(v_idx) * vb_stride)),
                                vb_stride)) {
                            all_v_ok = false;
                            break;
                        }
                    }
                    if (all_v_ok) {
                        uint32_t pos_off = 0u, pos_comps = 2u, non_float_mask = 0u;
                        ResolveRectListVertexLayout(
                            device_x_, last_host_pso_, vb_stride, deindexed_verts.data(), deindexed_verts.size(),
                            pos_off, pos_comps, non_float_mask);
                        std::vector<uint8_t> dst_buf;
                        ExpandRectListVerticesToTriangles(
                            deindexed_verts.data(), num_rects, vb_stride, pos_off, pos_comps, non_float_mask, dst_buf);
                        const D3D12_GPU_VIRTUAL_ADDRESS quad_gpu =
                            device_x_->UploadTransientGuestBufferForDesktop(
                                static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_buf.data())),
                                dst_buf.size(), 256ull, /*allow_same_frame_cache=*/false);
                        if (quad_gpu != 0ull) {
                            D3D12_VERTEX_BUFFER_VIEW qv{};
                            qv.BufferLocation = quad_gpu;
                            qv.SizeInBytes = static_cast<UINT>(dst_buf.size());
                            qv.StrideInBytes = vb_stride;
                            host_list_->IASetVertexBuffers(0u, 1u, &qv);
                            host_list_->DrawInstanced(num_rects * 6u, InstanceCount, 0u, StartInstanceLocation);
                            host_list_->IASetVertexBuffers(0u, 1u, &last_vb0_host_);
                            drew_indexed_quad = true;
                        }
                    }
                }
            } else if (last_raw_topology_ == 18u &&
                       IndexCountPerInstance >= 4u && (IndexCountPerInstance % 4u) == 0u &&
                       IndexCountPerInstance <= 65536u &&
                       last_ib_raw_.SizeInBytes >= static_cast<UINT>(StartIndexLocation) * idx_bytes + src_ib_bytes) {
                const uint32_t num_quads = IndexCountPerInstance / 4u;
                const size_t dst_bytes = static_cast<size_t>(num_quads) * 6u * idx_bytes;
                const uint64_t ib_cpu = device_x_->ResolveCpuAliasVa(
                    last_ib_raw_.BufferLocation + static_cast<uint64_t>(StartIndexLocation) * idx_bytes);
                std::vector<uint8_t> src_ib(src_ib_bytes);
                if (SafeCopyBytes(src_ib.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(ib_cpu)), src_ib_bytes)) {
                    std::vector<uint8_t> dst_ib(dst_bytes);
                    for (uint32_t q = 0u; q < num_quads; ++q) {
                        const uint8_t* si = src_ib.data() + static_cast<size_t>(q) * 4u * idx_bytes;
                        uint8_t* di = dst_ib.data() + static_cast<size_t>(q) * 6u * idx_bytes;
                        std::memcpy(di + 0u * idx_bytes, si + 0u * idx_bytes, idx_bytes);
                        std::memcpy(di + 1u * idx_bytes, si + 1u * idx_bytes, idx_bytes);
                        std::memcpy(di + 2u * idx_bytes, si + 2u * idx_bytes, idx_bytes);
                        std::memcpy(di + 3u * idx_bytes, si + 0u * idx_bytes, idx_bytes);
                        std::memcpy(di + 4u * idx_bytes, si + 2u * idx_bytes, idx_bytes);
                        std::memcpy(di + 5u * idx_bytes, si + 3u * idx_bytes, idx_bytes);
                    }
                    const D3D12_GPU_VIRTUAL_ADDRESS qib_gpu =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_ib.data())),
                            dst_bytes, 256ull, /*allow_same_frame_cache=*/false);
                    if (qib_gpu != 0ull) {
                        D3D12_INDEX_BUFFER_VIEW qib{};
                        qib.BufferLocation = qib_gpu;
                        qib.SizeInBytes = static_cast<UINT>(dst_bytes);
                        qib.Format = last_ib_raw_.Format;
                        host_list_->IASetIndexBuffer(&qib);
                        host_list_->DrawIndexedInstanced(
                            num_quads * 6u, InstanceCount, 0u, BaseVertexLocation, StartInstanceLocation);
                        host_list_->IASetIndexBuffer(&last_ib_host_);
                        drew_indexed_quad = true;
                    }
                }
            } else if ((last_raw_topology_ == 6u || last_raw_topology_ == 20u) &&
                       IndexCountPerInstance >= 3u && IndexCountPerInstance <= 65535u &&
                       last_ib_raw_.SizeInBytes >= static_cast<UINT>(StartIndexLocation) * idx_bytes + src_ib_bytes) {
                const uint32_t num_tris = IndexCountPerInstance - 2u;
                const size_t dst_bytes = static_cast<size_t>(num_tris) * 3u * idx_bytes;
                const uint64_t ib_cpu = device_x_->ResolveCpuAliasVa(
                    last_ib_raw_.BufferLocation + static_cast<uint64_t>(StartIndexLocation) * idx_bytes);
                std::vector<uint8_t> src_ib(src_ib_bytes);
                if (SafeCopyBytes(src_ib.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(ib_cpu)), src_ib_bytes)) {
                    std::vector<uint8_t> dst_ib(dst_bytes);
                    for (uint32_t t = 0u; t < num_tris; ++t) {
                        uint8_t* di = dst_ib.data() + static_cast<size_t>(t) * 3u * idx_bytes;
                        std::memcpy(di + 0u * idx_bytes, src_ib.data() + 0u, idx_bytes);
                        std::memcpy(di + 1u * idx_bytes, src_ib.data() + static_cast<size_t>(t + 1u) * idx_bytes, idx_bytes);
                        std::memcpy(di + 2u * idx_bytes, src_ib.data() + static_cast<size_t>(t + 2u) * idx_bytes, idx_bytes);
                    }
                    const D3D12_GPU_VIRTUAL_ADDRESS fib_gpu =
                        device_x_->UploadTransientGuestBufferForDesktop(
                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(dst_ib.data())),
                            dst_bytes, 256ull, /*allow_same_frame_cache=*/false);
                    if (fib_gpu != 0ull) {
                        D3D12_INDEX_BUFFER_VIEW fib{};
                        fib.BufferLocation = fib_gpu;
                        fib.SizeInBytes = static_cast<UINT>(dst_bytes);
                        fib.Format = last_ib_raw_.Format;
                        host_list_->IASetIndexBuffer(&fib);
                        host_list_->DrawIndexedInstanced(
                            num_tris * 3u, InstanceCount, 0u, BaseVertexLocation, StartInstanceLocation);
                        host_list_->IASetIndexBuffer(&last_ib_host_);
                        drew_indexed_quad = true;
                    }
                }
            }
        }
        if (!drew_indexed_quad) {
            UINT eff_start_index = StartIndexLocation;
            INT eff_base_vertex = BaseVertexLocation;
            bool rebound_ib = false;
            bool rebound_vb = false;
            if (device_x_ && device_x_->IsRetailScarlettAbi() &&
                !device_x_->PsoUsesZeroIaInputs(last_host_pso_)) {
                bool have_idx_bounds = false;
                uint32_t min_idx = UINT32_MAX;
                uint32_t max_idx = 0u;
                const bool any_vb_upload =
                    last_vb_need_upload_[0] || last_vb_need_upload_[1] ||
                    last_vb_need_upload_[2] || last_vb_need_upload_[3] ||
                    last_vb_need_upload_[4] || last_vb_need_upload_[5] ||
                    last_vb_need_upload_[6] || last_vb_need_upload_[7];
                if ((last_ib_need_upload_ || any_vb_upload) &&
                    last_ib_raw_.BufferLocation != 0ull &&
                    (last_ib_raw_.Format == DXGI_FORMAT_R16_UINT || last_ib_raw_.Format == DXGI_FORMAT_R32_UINT)) {
                    const uint32_t idx_bytes = (last_ib_raw_.Format == DXGI_FORMAT_R32_UINT) ? 4u : 2u;
                    const uint64_t ib_off_b = static_cast<uint64_t>(StartIndexLocation) * idx_bytes;
                    const uint64_t ib_slice_b = static_cast<uint64_t>(IndexCountPerInstance) * idx_bytes;
                    if (ib_off_b < last_ib_raw_.SizeInBytes && ib_slice_b > 0ull &&
                        ib_off_b + ib_slice_b <= last_ib_raw_.SizeInBytes &&
                        ib_slice_b <= 4ull * 1024ull * 1024ull) {
                        const uint64_t ib_slice_va = last_ib_raw_.BufferLocation + ib_off_b;
                        const uint64_t ib_cpu = device_x_->ResolveCpuAliasVa(ib_slice_va);
                        if (any_vb_upload && IndexCountPerInstance <= 262144u) {
                            std::vector<uint8_t> tmp_ib(static_cast<size_t>(ib_slice_b));
                            if (SafeCopyBytes(tmp_ib.data(),
                                              reinterpret_cast<const void*>(static_cast<uintptr_t>(ib_cpu)),
                                              tmp_ib.size())) {
                                const uint32_t prim_cut = (idx_bytes == 4u) ? 0xFFFFFFFFu : 0xFFFFu;
                                for (uint32_t k = 0u; k < IndexCountPerInstance; ++k) {
                                    uint32_t v = 0u;
                                    if (idx_bytes == 4u) {
                                        std::memcpy(&v, tmp_ib.data() + static_cast<size_t>(k) * 4u, 4u);
                                    } else {
                                        uint16_t v16 = 0u;
                                        std::memcpy(&v16, tmp_ib.data() + static_cast<size_t>(k) * 2u, 2u);
                                        v = v16;
                                    }
                                    if (v == prim_cut) continue;
                                    if (v < min_idx) min_idx = v;
                                    if (v > max_idx) max_idx = v;
                                }
                                have_idx_bounds = (min_idx <= max_idx);
                                if (last_ib_need_upload_) {
                                    const D3D12_GPU_VIRTUAL_ADDRESS up_ib =
                                        device_x_->UploadTransientGuestBufferForDesktop(
                                            static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(tmp_ib.data())),
                                            tmp_ib.size(), 16ull, /*allow_same_frame_cache=*/false);
                                    if (up_ib != 0ull) {
                                        D3D12_INDEX_BUFFER_VIEW sib{};
                                        sib.BufferLocation = up_ib;
                                        sib.SizeInBytes = static_cast<UINT>(tmp_ib.size());
                                        sib.Format = last_ib_raw_.Format;
                                        host_list_->IASetIndexBuffer(&sib);
                                        eff_start_index = 0u;
                                        rebound_ib = true;
                                    }
                                }
                            }
                        } else if (last_ib_need_upload_) {
                            const D3D12_GPU_VIRTUAL_ADDRESS up_ib =
                                device_x_->UploadTransientGuestBufferForDesktop(
                                    ib_slice_va, ib_slice_b, 16ull,
                                    /*allow_same_frame_cache=*/(ib_slice_b > 65536ull));
                            if (up_ib != 0ull) {
                                D3D12_INDEX_BUFFER_VIEW sib{};
                                sib.BufferLocation = up_ib;
                                sib.SizeInBytes = static_cast<UINT>(ib_slice_b);
                                sib.Format = last_ib_raw_.Format;
                                host_list_->IASetIndexBuffer(&sib);
                                eff_start_index = 0u;
                                rebound_ib = true;
                            }
                        }
                    }
                }
                if (any_vb_upload) {
                    const bool only_vb0 = (last_vb_valid_mask_ & ~1u) == 0u;
                    for (uint32_t s = 0u; s < 8u; ++s) {
                        if (!last_vb_need_upload_[s]) continue;
                        const auto& raw_vb = last_vb_raw_[s];
                        if (raw_vb.BufferLocation == 0ull || raw_vb.SizeInBytes == 0u) continue;
                        const uint32_t st = raw_vb.StrideInBytes;
                        uint64_t slice_va = raw_vb.BufferLocation;
                        uint64_t slice_bytes = raw_vb.SizeInBytes;
                        if (st > 0u && have_idx_bounds) {
                            const int64_t first_v = std::max<int64_t>(
                                0ll, static_cast<int64_t>(min_idx) + static_cast<int64_t>(BaseVertexLocation));
                            const int64_t last_v =
                                static_cast<int64_t>(max_idx) + static_cast<int64_t>(BaseVertexLocation);
                            if (last_v >= first_v) {
                                if (only_vb0 && s == 0u) {
                                    const uint64_t off_b = static_cast<uint64_t>(first_v) * st;
                                    const uint64_t need_b =
                                        static_cast<uint64_t>(last_v - first_v + 1ll) * st;
                                    if (off_b < raw_vb.SizeInBytes && need_b > 0ull) {
                                        slice_va = raw_vb.BufferLocation + off_b;
                                        slice_bytes = std::min<uint64_t>(need_b, raw_vb.SizeInBytes - off_b);
                                        eff_base_vertex = BaseVertexLocation - static_cast<INT>(first_v);
                                    }
                                } else {
                                    const uint64_t max_elems = std::max<uint64_t>(
                                        static_cast<uint64_t>(last_v + 1ll),
                                        static_cast<uint64_t>(StartInstanceLocation) + InstanceCount);
                                    slice_bytes = std::min<uint64_t>(raw_vb.SizeInBytes, max_elems * st);
                                }
                            }
                        }
                        slice_bytes = std::min<uint64_t>(slice_bytes, 4ull * 1024ull * 1024ull);
                        const D3D12_GPU_VIRTUAL_ADDRESS up_vb =
                            device_x_->UploadTransientGuestBufferForDesktop(
                                slice_va, slice_bytes, 16ull,
                                /*allow_same_frame_cache=*/(slice_bytes > 65536ull));
                        if (up_vb != 0ull) {
                            D3D12_VERTEX_BUFFER_VIEW svb{};
                            svb.BufferLocation = up_vb;
                            svb.SizeInBytes = static_cast<UINT>(slice_bytes);
                            svb.StrideInBytes = raw_vb.StrideInBytes;
                            host_list_->IASetVertexBuffers(s, 1u, &svb);
                            rebound_vb = true;
                        }
                    }
                }
            }
            host_list_->DrawIndexedInstanced(
                IndexCountPerInstance, InstanceCount, eff_start_index,
                eff_base_vertex, StartInstanceLocation);
            if (rebound_ib && last_ib_host_.BufferLocation != 0ull) {
                host_list_->IASetIndexBuffer(&last_ib_host_);
            }
            if (rebound_vb) {
                for (uint32_t s = 0u; s < 8u; ++s) {
                    if (last_vb_need_upload_[s] && last_vb_host_[s].BufferLocation != 0ull) {
                        host_list_->IASetVertexBuffers(s, 1u, &last_vb_host_[s]);
                    }
                }
            }
        }
    } else if (IsDiagTraceEnabled() && (last_pso_is_standin_ || pso_missing)) {
        static std::atomic<uint64_t> s_skip_dii{0ull};
        const uint64_t n = s_skip_dii.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if (n == 1ull || (n % 500ull) == 0ull) {
            std::fprintf(stderr, "[OpenScarlett][CL] DrawIndexedInstanced SKIPPED #%llu (idx=%u standin=%d no_pso=%d)\n",
                         static_cast<unsigned long long>(n), IndexCountPerInstance,
                         last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
            std::fflush(stderr);
        }
    }
    RecordList("ID3D12GraphicsCommandListX::DrawIndexedInstanced", 13, S_OK);
}

void STDMETHODCALLTYPE CommandListX::Dispatch(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (ThreadGroupCountX == 0u || ThreadGroupCountY == 0u || ThreadGroupCountZ == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::Dispatch[ZeroNoop]", 14, S_OK);
        return;
    }
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
    const bool pso_missing =
        device_x_ && device_x_->IsRetailScarlettAbi() && last_host_pso_ == nullptr;
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "Dispatch(%u,%u,%u standin=%d, no_pso=%d)",
                      ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ,
                      last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
        AppendDiagOp(buf);
    }
    if (!skip_host && !last_pso_is_standin_ && !pso_missing) {
        constexpr UINT kMaxDim = 65535u;
        if (ThreadGroupCountX <= kMaxDim &&
            ThreadGroupCountY <= kMaxDim &&
            ThreadGroupCountZ <= kMaxDim) {
            host_list_->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        } else {
            for (UINT z = 0u; z < ThreadGroupCountZ; z += kMaxDim) {
                const UINT cz = std::min<UINT>(ThreadGroupCountZ - z, kMaxDim);
                for (UINT y = 0u; y < ThreadGroupCountY; y += kMaxDim) {
                    const UINT cy = std::min<UINT>(ThreadGroupCountY - y, kMaxDim);
                    for (UINT x = 0u; x < ThreadGroupCountX; x += kMaxDim) {
                        const UINT cx = std::min<UINT>(ThreadGroupCountX - x, kMaxDim);
                        host_list_->Dispatch(cx, cy, cz);
                    }
                }
            }
        }
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
    if (NumBytes == 0ull || !host_dst || !host_src) {
        RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
        return;
    }
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CopyBufferRegion(dst=%p+%llu src=%p+%llu bytes=%llu)",
                      static_cast<void*>(host_dst), static_cast<unsigned long long>(DstOffset),
                      static_cast<void*>(host_src), static_cast<unsigned long long>(SrcOffset),
                      static_cast<unsigned long long>(NumBytes));
        AppendDiagOp(buf);
    }
    if (host_dst == host_src) {
        if (DstOffset == SrcOffset) {
            // No-op self-copy: Desktop D3D12 rejects overlapping CopyBufferRegion (error #550).
            RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
            return;
        }
        const bool overlaps = (DstOffset < SrcOffset + NumBytes) && (SrcOffset < DstOffset + NumBytes);
        if (overlaps && device_x_ && device_x_->HostDevice()) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = AlignUpPlaced64(NumBytes, 256ull);
            bd.Height = 1u;
            bd.DepthOrArraySize = 1u;
            bd.MipLevels = 1u;
            bd.Format = DXGI_FORMAT_UNKNOWN;
            bd.SampleDesc.Count = 1u;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* staging = nullptr;
            if (SUCCEEDED(device_x_->HostDevice()->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&staging))) &&
                staging) {
                host_list_->CopyBufferRegion(staging, 0ull, host_src, SrcOffset, NumBytes);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = staging;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                host_list_->ResourceBarrier(1u, &b);
                host_list_->CopyBufferRegion(host_dst, DstOffset, staging, 0ull, NumBytes);
                device_x_->NotifyGpuWrittenBufferRange(host_dst, DstOffset, NumBytes);
                TrackAlignTransientResource(staging);
                RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
                return;
            }
        }
    }
    if (device_x_) {
        device_x_->NotifyGpuWrittenBufferRange(host_dst, DstOffset, NumBytes);
    }
    host_list_->CopyBufferRegion(host_dst, DstOffset, host_src, SrcOffset, NumBytes);
    if (IsDiagTraceEnabled()) {
        const D3D12_RESOURCE_DESC dd = host_dst ? host_dst->GetDesc() : D3D12_RESOURCE_DESC{};
        const D3D12_RESOURCE_DESC sd = host_src ? host_src->GetDesc() : D3D12_RESOURCE_DESC{};
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "CopyBufferRegion dst=%p(w=%llu)@%llu src=%p(w=%llu)@%llu bytes=%llu",
                      static_cast<void*>(host_dst), static_cast<unsigned long long>(dd.Width),
                      static_cast<unsigned long long>(DstOffset),
                      static_cast<void*>(host_src), static_cast<unsigned long long>(sd.Width),
                      static_cast<unsigned long long>(SrcOffset),
                      static_cast<unsigned long long>(NumBytes));
        AppendDiagOp(buf);
    }
    RecordList("ID3D12GraphicsCommandListX::CopyBufferRegion", 15, S_OK);
}

namespace {

bool IsPlanarDepthStencilTextureFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return true;
        default:
            return false;
    }
}

bool IsAnyDepthStencilTextureFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return true;
        default:
            return false;
    }
}

DXGI_FORMAT NormalizePlanarDepthStencilFootprintFormat(
    ID3D12Device* host_dev,
    const D3D12_RESOURCE_DESC& tdesc,
    uint32_t subresource_index,
    uint32_t plane_slice,
    DXGI_FORMAT cur_fp_fmt) noexcept {
    // Query host D3D12 runtime's exact canonical copyable footprint format for this planar subresource
    // (e.g. R32_TYPELESS for D24S8/D32S8 plane 0, R8_TYPELESS for stencil plane 1).
    DXGI_FORMAT canonical = DXGI_FORMAT_UNKNOWN;
    if (host_dev) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        host_dev->GetCopyableFootprints(&tdesc, subresource_index, 1u, 0ull, &fp, nullptr, nullptr, nullptr);
        canonical = fp.Footprint.Format;
    }
    if (canonical == DXGI_FORMAT_UNKNOWN) {
        canonical = (plane_slice == 0u) ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8_TYPELESS;
    }
    switch (cur_fp_fmt) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            return canonical;
        default:
            return cur_fp_fmt;
    }
}

uint64_t ComputePlacedFootprintByteSize(const D3D12_SUBRESOURCE_FOOTPRINT& fp) noexcept {
    const bool is_bc =
        (fp.Format >= DXGI_FORMAT_BC1_TYPELESS && fp.Format <= DXGI_FORMAT_BC5_SNORM) ||
        (fp.Format >= DXGI_FORMAT_BC6H_TYPELESS && fp.Format <= DXGI_FORMAT_BC7_UNORM_SRGB);
    const uint32_t block_h = is_bc ? 4u : 1u;
    const uint64_t rows = std::max<uint64_t>(1ull, (static_cast<uint64_t>(fp.Height) + block_h - 1ull) / block_h);
    const uint64_t depth = std::max<uint64_t>(1ull, static_cast<uint64_t>(fp.Depth));
    return static_cast<uint64_t>(fp.RowPitch) * rows * depth;
}

}  // namespace

void STDMETHODCALLTYPE CommandListX::CopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
    const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) {
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "CopyTextureRegion(dst=%p src=%p)",
                      pDst ? static_cast<void*>(pDst->pResource) : nullptr,
                      pSrc ? static_cast<void*>(pSrc->pResource) : nullptr);
        AppendDiagOp(buf);
    }
    if (!pDst || !pSrc) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[NullNoop]", 16, S_OK);
        return;
    }
    if (pSrcBox &&
        (pSrcBox->left >= pSrcBox->right ||
         pSrcBox->top >= pSrcBox->bottom ||
         pSrcBox->front >= pSrcBox->back)) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[EmptyBoxNoop]", 16, S_OK);
        return;
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = *pDst;
    D3D12_TEXTURE_COPY_LOCATION src_loc = *pSrc;
    dst_loc.pResource = UnwrapResource(pDst->pResource);
    src_loc.pResource = UnwrapResource(pSrc->pResource);
    if (!dst_loc.pResource || !src_loc.pResource) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[NullResNoop]", 16, S_OK);
        return;
    }
    D3D12_TEXTURE_COPY_LOCATION* dst_ptr = &dst_loc;
    D3D12_TEXTURE_COPY_LOCATION* src_ptr = &src_loc;

    // Normalize depth/stencil planar footprint formats and track depth/stencil plane + footprint-offset copies (AnyBox #54/#57/#67).
    bool is_depth_stencil_copy = false;
    bool has_footprint_offset = false;
    auto inspect_tex_and_normalize_fp = [&](const D3D12_TEXTURE_COPY_LOCATION& tex_loc,
                                            D3D12_TEXTURE_COPY_LOCATION& fp_loc) {
        if (tex_loc.Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX || !tex_loc.pResource) return;
        const D3D12_RESOURCE_DESC tdesc = tex_loc.pResource->GetDesc();
        if (IsAnyDepthStencilTextureFormat(tdesc.Format)) {
            is_depth_stencil_copy = true;
        }
        if (fp_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            if (fp_loc.PlacedFootprint.Offset != 0ull) {
                has_footprint_offset = true;
            }
            if (IsPlanarDepthStencilTextureFormat(tdesc.Format)) {
                const uint32_t mips = std::max<uint32_t>(1u, tdesc.MipLevels);
                const uint32_t arr =
                    (tdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                        ? 1u
                        : std::max<uint32_t>(1u, tdesc.DepthOrArraySize);
                const uint32_t plane_slice = tex_loc.SubresourceIndex / (mips * arr);
                fp_loc.PlacedFootprint.Footprint.Format = NormalizePlanarDepthStencilFootprintFormat(
                    device_x_ ? device_x_->HostDevice() : nullptr,
                    tdesc, tex_loc.SubresourceIndex, plane_slice,
                    fp_loc.PlacedFootprint.Footprint.Format);
            }
        }
    };
    inspect_tex_and_normalize_fp(dst_loc, src_loc);
    inspect_tex_and_normalize_fp(src_loc, dst_loc);
    if (dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
        src_loc.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        if (IsAnyDepthStencilTextureFormat(dst_loc.pResource->GetDesc().Format) ||
            IsAnyDepthStencilTextureFormat(src_loc.pResource->GetDesc().Format)) {
            is_depth_stencil_copy = true;
        }
    }
    if (is_depth_stencil_copy && device_x_) {
        device_x_->RecordDepthStencilPlaneCopy();
    }
    if (has_footprint_offset && device_x_) {
        device_x_->RecordFootprintOffsetCopy();
    }

    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::CopyTexture;
        op.dst_resource = dst_ptr->pResource;
        op.src_resource = src_ptr->pResource;
        op.copy_dst_x = DstX;
        op.copy_dst_y = DstY;
        op.copy_dst_z = DstZ;
        if (src_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            op.copy_footprint_offset = src_loc.PlacedFootprint.Offset;
        } else if (dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            op.copy_footprint_offset = dst_loc.PlacedFootprint.Offset;
        }
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }

    // Sprint 13 placed tiled texture bridge (64KB tile-pool mapping, subresource pitch, untile/retile).
    if (TryBridgePlacedTiledCopyTextureRegion(*dst_ptr, DstX, DstY, DstZ, *src_ptr, pSrcBox)) {
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[PlacedTiled]", 16, S_OK);
        RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
        return;
    }

    // Live XgGpuUntiler intercept when source or destination uses a GFX10 swizzle layout.
    if (device_x_ &&
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

    // Xbox Series X allows 64/256-byte PlacedFootprint.Offset alignment (smaller than desktop PC
    // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512) and smaller RowPitch alignment (desktop requires 256).
    // Sub-allocate 512-byte-aligned slices from our reusable 4-slot DEFAULT staging ring using
    // CopyBufferRegion (which has 1-byte offset alignment) with zero per-call WDDM allocations!
    D3D12_TEXTURE_COPY_LOCATION aligned_src_loc{};
    if (device_x_ && device_x_->HostDevice() && dst_ptr && src_ptr &&
        src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && src_ptr->pResource != nullptr) {
        const UINT64 src_off = src_ptr->PlacedFootprint.Offset;
        const auto& fp = src_ptr->PlacedFootprint.Footprint;
        const bool bad_offset = (src_off & 511ull) != 0ull;
        const bool bad_pitch = (fp.RowPitch & 255u) != 0u;
        if (bad_offset || bad_pitch) {
            const bool is_bc =
                (fp.Format >= DXGI_FORMAT_BC1_TYPELESS && fp.Format <= DXGI_FORMAT_BC5_SNORM) ||
                (fp.Format >= DXGI_FORMAT_BC6H_TYPELESS && fp.Format <= DXGI_FORMAT_BC7_UNORM_SRGB);
            const UINT block_h = is_bc ? 4u : 1u;
            const UINT64 num_rows = std::max<UINT64>(1ull, (static_cast<UINT64>(fp.Height) + block_h - 1ull) / block_h);
            const UINT64 num_slices = std::max<UINT64>(1ull, static_cast<UINT64>(fp.Depth));
            const UINT64 total_rows = num_rows * num_slices;
            const UINT aligned_pitch = (fp.RowPitch + 255u) & ~255u;
            const UINT64 src_buf_width = src_ptr->pResource->GetDesc().Width;
            const UINT64 src_avail = (src_buf_width > src_off) ? (src_buf_width - src_off) : 0ull;
            const UINT64 slice_bytes = (total_rows * static_cast<UINT64>(aligned_pitch) + 511ull) & ~511ull;

            ID3D12Resource* stage_buf = nullptr;
            UINT64 stage_off = 0ull;
            D3D12_RESOURCE_STATES stage_state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (src_avail > 0ull && slice_bytes > 0ull &&
                AcquireAlignedStagingSlice(slice_bytes, stage_buf, stage_off, stage_state) && stage_buf) {
                if (stage_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    TransitionAlignedStagingState(D3D12_RESOURCE_STATE_COPY_DEST);
                }
                if (!bad_pitch) {
                    const UINT64 raw_bytes = total_rows * static_cast<UINT64>(fp.RowPitch);
                    const UINT64 copy_bytes = std::min<UINT64>(raw_bytes, src_avail);
                    host_list_->CopyBufferRegion(stage_buf, stage_off, src_ptr->pResource, src_off, copy_bytes);
                } else {
                    for (UINT64 r = 0ull; r < total_rows; ++r) {
                        const UINT64 r_src = src_off + r * static_cast<UINT64>(fp.RowPitch);
                        if (r_src >= src_buf_width) break;
                        const UINT64 r_len = std::min<UINT64>(fp.RowPitch, src_buf_width - r_src);
                        host_list_->CopyBufferRegion(stage_buf, stage_off + r * static_cast<UINT64>(aligned_pitch),
                                                     src_ptr->pResource, r_src, r_len);
                    }
                }
                TransitionAlignedStagingState(D3D12_RESOURCE_STATE_COPY_SOURCE);

                aligned_src_loc = *src_ptr;
                aligned_src_loc.pResource = stage_buf;
                aligned_src_loc.PlacedFootprint.Offset = stage_off;
                aligned_src_loc.PlacedFootprint.Footprint.RowPitch = aligned_pitch;
                src_ptr = &aligned_src_loc;
            }
        }
    }

    if (device_x_ && device_x_->HostDevice() && dst_ptr && src_ptr &&
        dst_loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && dst_loc.pResource != nullptr &&
        (dst_loc.PlacedFootprint.Offset & (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1ull)) != 0ull) {
        const uint64_t fp_bytes = ComputePlacedFootprintByteSize(dst_loc.PlacedFootprint.Footprint);
        const D3D12_RESOURCE_DESC dst_desc = dst_loc.pResource->GetDesc();
        const uint64_t avail = (dst_loc.PlacedFootprint.Offset < dst_desc.Width)
                                   ? (dst_desc.Width - dst_loc.PlacedFootprint.Offset)
                                   : 0ull;
        const uint64_t copy_bytes = std::min<uint64_t>(fp_bytes, avail);
        const UINT64 slice_bytes = (fp_bytes + 511ull) & ~511ull;
        ID3D12Resource* stage_buf = nullptr;
        UINT64 stage_off = 0ull;
        D3D12_RESOURCE_STATES stage_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (copy_bytes > 0ull &&
            AcquireAlignedStagingSlice(slice_bytes, stage_buf, stage_off, stage_state) && stage_buf) {
            if (stage_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                TransitionAlignedStagingState(D3D12_RESOURCE_STATE_COPY_DEST);
            }
            D3D12_TEXTURE_COPY_LOCATION aligned_dst = dst_loc;
            aligned_dst.pResource = stage_buf;
            aligned_dst.PlacedFootprint.Offset = stage_off;
            host_list_->CopyTextureRegion(&aligned_dst, DstX, DstY, DstZ, src_ptr, pSrcBox);
            TransitionAlignedStagingState(D3D12_RESOURCE_STATE_COPY_SOURCE);
            host_list_->CopyBufferRegion(
                dst_loc.pResource, dst_loc.PlacedFootprint.Offset, stage_buf, stage_off, copy_bytes);
            device_x_->NotifyGpuWrittenBufferRange(
                dst_loc.pResource, dst_loc.PlacedFootprint.Offset, copy_bytes);
            RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion[AlignedFootprintStage]", 16, S_OK);
            RecordList("ID3D12GraphicsCommandListX::CopyTextureRegion", 16, S_OK);
            return;
        }
    }

    if (device_x_ && dst_ptr && dst_ptr->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        device_x_->NotifyGpuWrittenTexture(dst_ptr->pResource);
    } else if (device_x_ && dst_ptr && dst_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
               dst_ptr->pResource != nullptr) {
        const uint64_t fp_bytes = ComputePlacedFootprintByteSize(dst_ptr->PlacedFootprint.Footprint);
        device_x_->NotifyGpuWrittenBufferRange(
            dst_ptr->pResource, dst_ptr->PlacedFootprint.Offset, fp_bytes);
    }
    {
        static const bool s_diag_bink_cp = []() {
            char b[16]{};
            return ::GetEnvironmentVariableA("OPENSCARLETT_DIAG_BINK", b, sizeof(b)) > 0 && b[0] == '1';
        }();
        if (s_diag_bink_cp && dst_ptr && src_ptr && dst_ptr->pResource && src_ptr->pResource) {
            const D3D12_RESOURCE_DESC dd = dst_ptr->pResource->GetDesc();
            const D3D12_RESOURCE_DESC sd = src_ptr->pResource->GetDesc();
            if ((dd.Width >= 960u && (dd.Format == DXGI_FORMAT_R8_UNORM || dd.Format == DXGI_FORMAT_R8_TYPELESS)) ||
                (sd.Width >= 960u && (sd.Format == DXGI_FORMAT_R8_UNORM || sd.Format == DXGI_FORMAT_R8_TYPELESS))) {
                static std::atomic<uint32_t> s_bcp_cnt{0u};
                if (s_bcp_cnt.fetch_add(1u, std::memory_order_relaxed) < 24u) {
                    std::fprintf(stderr,
                                 "[OpenScarlett][BINK-COPY-TEX] dst=%p(t=%d %llux%u f=%d) src=%p(t=%d %llux%u f=%d off=0x%llX)\n",
                                 static_cast<void*>(dst_ptr->pResource), static_cast<int>(dst_ptr->Type),
                                 static_cast<unsigned long long>(dd.Width), dd.Height, static_cast<int>(dd.Format),
                                 static_cast<void*>(src_ptr->pResource), static_cast<int>(src_ptr->Type),
                                 static_cast<unsigned long long>(sd.Width), sd.Height, static_cast<int>(sd.Format),
                                 src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
                                     ? static_cast<unsigned long long>(src_ptr->PlacedFootprint.Offset) : 0ull);
                    std::fflush(stderr);
                }
            }
        }
    }
    host_list_->CopyTextureRegion(dst_ptr, DstX, DstY, DstZ, src_ptr, pSrcBox);
    if (IsDiagTraceEnabled() && dst_ptr && src_ptr) {
        const D3D12_RESOURCE_DESC dd = dst_ptr->pResource ? dst_ptr->pResource->GetDesc() : D3D12_RESOURCE_DESC{};
        const D3D12_RESOURCE_DESC sd = src_ptr->pResource ? src_ptr->pResource->GetDesc() : D3D12_RESOURCE_DESC{};
        char buf[300];
        std::snprintf(buf, sizeof(buf),
                      "CopyTextureRegion dst=%p(t=%d,fmt=%d,w=%llu,h=%u)@(%u,%u,%u) src=%p(t=%d,fmt=%d,w=%llu,off=%llu,fp_fmt=%d,fp_w=%u,fp_h=%u,rp=%u)",
                      static_cast<void*>(dst_ptr->pResource), static_cast<int>(dst_ptr->Type),
                      static_cast<int>(dd.Format), static_cast<unsigned long long>(dd.Width), dd.Height,
                      DstX, DstY, DstZ,
                      static_cast<void*>(src_ptr->pResource), static_cast<int>(src_ptr->Type),
                      static_cast<int>(sd.Format), static_cast<unsigned long long>(sd.Width),
                      src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? static_cast<unsigned long long>(src_ptr->PlacedFootprint.Offset) : 0ull,
                      src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? static_cast<int>(src_ptr->PlacedFootprint.Footprint.Format) : -1,
                      src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? src_ptr->PlacedFootprint.Footprint.Width : 0u,
                      src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? src_ptr->PlacedFootprint.Footprint.Height : 0u,
                      src_ptr->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? src_ptr->PlacedFootprint.Footprint.RowPitch : 0u);
        AppendDiagOp(buf);
    }
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
    if (IsDiagTraceEnabled()) {
        AppendDiagOp("CopyResource");
    }
    if (!host_dst || !host_src) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::CopyResource[NullResNoop]", 17, S_OK);
        return;
    }
    if (TryBridgePlacedTiledCopyResource(host_dst, host_src)) {
        RecordList("ID3D12GraphicsCommandListX::CopyResource[PlacedTiled]", 17, S_OK);
        RecordList("ID3D12GraphicsCommandListX::CopyResource", 17, S_OK);
        return;
    }
    if (device_x_) {
        device_x_->NotifyGpuWrittenTexture(host_dst);
        device_x_->NotifyGpuWrittenBuffer(host_dst);
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
    if (!host_tiled || !host_buf) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::CopyTiles[NullResNoop]", 18, S_OK);
        return;
    }
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
    ID3D12Resource* host_dst = UnwrapResource(pDstResource);
    ID3D12Resource* host_src = UnwrapResource(pSrcResource);
    if (!host_dst || !host_src) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::ResolveSubresource[NullResNoop]", 19, S_OK);
        return;
    }
    if (host_dst && host_src && host_list1_) {
        const D3D12_RESOURCE_DESC dd = host_dst->GetDesc();
        const D3D12_RESOURCE_DESC sd = host_src->GetDesc();
        const UINT dst_mip = DstSubresource % std::max<UINT>(1u, dd.MipLevels);
        const UINT src_mip = SrcSubresource % std::max<UINT>(1u, sd.MipLevels);
        const UINT dst_w = static_cast<UINT>(std::max<UINT64>(1ull, dd.Width >> dst_mip));
        const UINT dst_h = std::max<UINT>(1u, dd.Height >> dst_mip);
        const UINT src_w = static_cast<UINT>(std::max<UINT64>(1ull, sd.Width >> src_mip));
        const UINT src_h = std::max<UINT>(1u, sd.Height >> src_mip);
        if (src_w != dst_w || src_h != dst_h) {
            D3D12_RECT rect{};
            rect.left = 0;
            rect.top = 0;
            rect.right = static_cast<LONG>(std::min(src_w, dst_w));
            rect.bottom = static_cast<LONG>(std::min(src_h, dst_h));
            host_list1_->ResolveSubresourceRegion(
                host_dst, DstSubresource, 0u, 0u,
                host_src, SrcSubresource, &rect,
                Format, D3D12_RESOLVE_MODE_AVERAGE);
            RecordList("ID3D12GraphicsCommandListX::ResolveSubresource", 19, S_OK);
            return;
        }
    }
    host_list_->ResolveSubresource(
        host_dst, DstSubresource, host_src, SrcSubresource, Format);
    RecordList("ID3D12GraphicsCommandListX::ResolveSubresource", 19, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) {
    D3D12_PRIMITIVE_TOPOLOGY clean_topo = PrimitiveTopology;
    const uint32_t raw_t = static_cast<uint32_t>(PrimitiveTopology);
    last_raw_topology_ = raw_t;
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        // Xbox Scarlett console-only topologies (e.g. 19=RECTLIST, 18=QUADLIST, 17=LINELOOP,
        // 20=TRIANGLEFAN, 0=UNDEFINED) are invalid in PC D3D12 and hang NVIDIA pushbuffers.
        const bool valid_pc_topo =
            (raw_t >= 1u && raw_t <= 5u) ||
            (raw_t >= 10u && raw_t <= 13u) ||
            (raw_t >= 33u && raw_t <= 64u);
        if (!valid_pc_topo) {
            static std::atomic<uint32_t> s_topo_log{0u};
            if (s_topo_log.fetch_add(1u, std::memory_order_relaxed) < 5u) {
                std::fprintf(stderr,
                             "[OpenScarlett][d3d12_xs] IASetPrimitiveTopology: sanitized Xbox topology %u -> TRIANGLELIST\n",
                             raw_t);
                std::fflush(stderr);
            }
            clean_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }
    if (IsDiagTraceEnabled()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "IASetPrimitiveTopology(raw=%u->%d)",
                      raw_t, static_cast<int>(clean_topo));
        AppendDiagOp(buf);
    }
    host_list_->IASetPrimitiveTopology(clean_topo);
    RecordList("ID3D12GraphicsCommandListX::IASetPrimitiveTopology", 20, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetViewports(
    UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
    if (IsDiagTraceEnabled() && NumViewports > 0u && pViewports) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "RSSetViewports(n=%u v0=(%.0f,%.0f %.0fx%.0f))",
                      NumViewports, pViewports[0].TopLeftX, pViewports[0].TopLeftY,
                      pViewports[0].Width, pViewports[0].Height);
        AppendDiagOp(buf);
    }
    if (NumViewports == 0u || !pViewports) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::RSSetViewports[ZeroNoop]", 21, S_OK);
        return;
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_vp_width_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Width));
        vk_vp_height_ = static_cast<uint32_t>(std::max(0.0f, pViewports[0].Height));
    }
    host_list_->RSSetViewports(NumViewports, pViewports);
    RecordList("ID3D12GraphicsCommandListX::RSSetViewports", 21, S_OK);
}

void STDMETHODCALLTYPE CommandListX::RSSetScissorRects(
    UINT NumRects, const D3D12_RECT* pRects) {
    if (IsDiagTraceEnabled() && NumRects > 0u && pRects) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "RSSetScissorRects(n=%u r0=(%ld,%ld,%ld,%ld))",
                      NumRects, pRects[0].left, pRects[0].top, pRects[0].right, pRects[0].bottom);
        AppendDiagOp(buf);
    }
    if (NumRects == 0u || !pRects) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::RSSetScissorRects[ZeroNoop]", 22, S_OK);
        return;
    }
    host_list_->RSSetScissorRects(NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::RSSetScissorRects", 22, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetBlendFactor(const FLOAT BlendFactor[4]) {
    host_list_->OMSetBlendFactor(BlendFactor);
    RecordList("ID3D12GraphicsCommandListX::OMSetBlendFactor", 23, S_OK);
}

void STDMETHODCALLTYPE CommandListX::OMSetStencilRef(UINT StencilRef) {
    guest_set_stencil_ref_ = true;
    guest_stencil_ref_ = StencilRef;
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
    ID3D12PipelineState* host_pso = nullptr;
    last_pso_is_standin_ = false;
    last_pso_proxy_ = nullptr;
    if (pPipelineState && IsKnownProxyPointer(pPipelineState)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pPipelineState->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::PipelineState) {
                auto* pso_proxy = static_cast<PipelineStateXProxy*>(pPipelineState);
                pso_proxy->EnsureLiftedOnBind();
                last_pso_is_standin_ = pso_proxy->IsStandinPso();
                last_pso_proxy_ = pso_proxy;
                host_pso = pso_proxy->HostPipelineState();
            }
            insp->Release();
        }
    }
    if (!host_pso) {
        host_pso = UnwrapPipelineState(pPipelineState);
    }
    last_host_pso_ = host_pso;
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetPipelineState(%p standin=%d)",
                      static_cast<void*>(host_pso), last_pso_is_standin_ ? 1 : 0);
        AppendDiagOp(buf);
    }
    if (!skip_host && !last_pso_is_standin_ && host_pso != nullptr) {
        host_list_->SetPipelineState(host_pso);
    }
    RecordList("ID3D12GraphicsCommandListX::SetPipelineState", 25, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ResourceBarrier(
    UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (IsDiagTraceEnabled() && pBarriers && NumBarriers > 0u) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ResourceBarrier(n=%u type0=%d)",
                      NumBarriers, static_cast<int>(pBarriers[0].Type));
        AppendDiagOp(buf);
    }
    if (!pBarriers || NumBarriers == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::ResourceBarrier[ZeroNoop]", 26, S_OK);
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
    uint32_t global_uav_barriers = 0u;
    for (UINT i = 0u; i < NumBarriers; ++i) {
        D3D12_RESOURCE_BARRIER b = pBarriers[i];
        bool emit = true;
        switch (b.Type) {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
                bool has_cached_info = false;
                bool is_upload_or_readback = false;
                bool is_buffer = false;
                b.Transition.pResource = UnwrapResourceWithBarrierInfo(
                    pBarriers[i].Transition.pResource,
                    has_cached_info,
                    is_upload_or_readback,
                    is_buffer);
                // Desktop D3D12 forbids state transitions on UPLOAD/READBACK heaps (unlike Xbox UMA).
                if (b.Transition.pResource != nullptr) {
                    if (!has_cached_info) {
                        D3D12_HEAP_PROPERTIES hp{};
                        if (SUCCEEDED(b.Transition.pResource->GetHeapProperties(&hp, nullptr)) &&
                            (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK)) {
                            is_upload_or_readback = true;
                        }
                        if (!is_upload_or_readback) {
                            is_buffer = (b.Transition.pResource->GetDesc().Dimension ==
                                         D3D12_RESOURCE_DIMENSION_BUFFER);
                        }
                    }
                    if (is_upload_or_readback) {
                        emit = false;
                        break;
                    }
                    // Sprint 16: Dedicated Copy Engine queue parity (xvalkyrie 90ef28a).
                    // D3D12 Copy command lists only permit COMMON, COPY_SOURCE, and COPY_DEST;
                    // filter out non-copy barrier states (e.g. RENDER_TARGET, PIXEL_SHADER_RESOURCE,
                    // UNORDERED_ACCESS) and drop redundant COMMON->COMMON transitions cleanly.
                    if (list_type_ == D3D12_COMMAND_LIST_TYPE_COPY) {
                        constexpr UINT kCopyAllowedMask =
                            static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                            static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST);
                        const UINT orig_sb = static_cast<UINT>(b.Transition.StateBefore);
                        const UINT orig_sa = static_cast<UINT>(b.Transition.StateAfter);
                        const UINT copy_sb = orig_sb & kCopyAllowedMask;
                        const UINT copy_sa = orig_sa & kCopyAllowedMask;
                        if (copy_sb != orig_sb || copy_sa != orig_sa) {
                            b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(copy_sb);
                            b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(copy_sa);
                            if (device_x_) device_x_->RecordCopyQueueBarrierSanitized(false);
                        }
                        if (b.Transition.StateBefore == b.Transition.StateAfter) {
                            emit = false;
                            break;
                        }
                    }
                    if (device_x_ && device_x_->IsRetailScarlettAbi() &&
                        device_x_->AdapterInfo().raytracing_tier == 0u) {
                        constexpr UINT kRtAsBit =
                            static_cast<UINT>(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
                        if ((static_cast<UINT>(b.Transition.StateBefore) & kRtAsBit) != 0u) {
                            UINT sb = static_cast<UINT>(b.Transition.StateBefore) & ~kRtAsBit;
                            if (sb == 0u) sb = static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(sb);
                        }
                        if ((static_cast<UINT>(b.Transition.StateAfter) & kRtAsBit) != 0u) {
                            UINT sa = static_cast<UINT>(b.Transition.StateAfter) & ~kRtAsBit;
                            if (sa == 0u) sa = static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(sa);
                        }
                        if (b.Transition.StateBefore == b.Transition.StateAfter) {
                            emit = false;
                            break;
                        }
                    }
                    // Desktop D3D12 forbids Xbox-exclusive high state bits and read+write mixed states
                    // (e.g. UNORDERED_ACCESS | NON_PIXEL_SHADER_RESOURCE) which poison Close() with 0x80070057.
                    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
                        const D3D12_RESOURCE_FLAGS rflags = b.Transition.pResource->GetDesc().Flags;
                        auto sanitize_retail_state = [is_buffer, rflags](D3D12_RESOURCE_STATES st) noexcept -> D3D12_RESOURCE_STATES {
                            constexpr UINT kValidDesktopMask =
                                static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_READ) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_STREAM_OUT) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_DEST) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_COPY_SOURCE) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
                                static_cast<UINT>(D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
                            UINT u = static_cast<UINT>(st) & kValidDesktopMask;
                            if (!is_buffer) {
                                constexpr UINT kBufOnly =
                                    static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
                                    static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
                                    static_cast<UINT>(D3D12_RESOURCE_STATE_STREAM_OUT) |
                                    static_cast<UINT>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                                u &= ~kBufOnly;
                            }
                            if ((rflags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
                                u &= ~static_cast<UINT>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                            }
                            if ((rflags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
                                u &= ~static_cast<UINT>(D3D12_RESOURCE_STATE_RENDER_TARGET);
                            }
                            if ((rflags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                                u &= ~static_cast<UINT>(D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ);
                            }
                            if ((rflags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
                                u &= ~static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                            }
                            if ((u & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0u) return D3D12_RESOURCE_STATE_RENDER_TARGET;
                            if ((u & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0u) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                            if ((u & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0u) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
                            if ((u & D3D12_RESOURCE_STATE_COPY_DEST) != 0u) return D3D12_RESOURCE_STATE_COPY_DEST;
                            if ((u & D3D12_RESOURCE_STATE_RESOLVE_DEST) != 0u) return D3D12_RESOURCE_STATE_RESOLVE_DEST;
                            if ((u & D3D12_RESOURCE_STATE_STREAM_OUT) != 0u) return D3D12_RESOURCE_STATE_STREAM_OUT;
                            return static_cast<D3D12_RESOURCE_STATES>(u);
                        };
                        b.Transition.StateBefore = sanitize_retail_state(b.Transition.StateBefore);
                        b.Transition.StateAfter = sanitize_retail_state(b.Transition.StateAfter);
                        if (b.Transition.StateBefore == b.Transition.StateAfter) {
                            emit = false;
                            break;
                        }
                    }
                    // Desktop D3D12 forbids buffer-only state bits on texture resources
                    // (Xbox titles sometimes pass GENERIC_READ or VB/IB bits on textures).
                    if (!is_buffer) {
                        constexpr UINT kBufferOnlyBits =
                            static_cast<UINT>(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
                            static_cast<UINT>(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
                            static_cast<UINT>(D3D12_RESOURCE_STATE_STREAM_OUT) |
                            static_cast<UINT>(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                        const UINT sb = static_cast<UINT>(b.Transition.StateBefore) & ~kBufferOnlyBits;
                        const UINT sa = static_cast<UINT>(b.Transition.StateAfter) & ~kBufferOnlyBits;
                        b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(sb);
                        b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(sa);
                        if (b.Transition.StateBefore == b.Transition.StateAfter) {
                            emit = false;
                            break;
                        }
                    }
                }
                if (b.Transition.pResource == nullptr ||
                    b.Transition.StateBefore == b.Transition.StateAfter) {
                    emit = false;
                    break;
                }
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
                    {
                        std::lock_guard<std::mutex> lk(aaa_rec_mutex_);
                        deferred_state_transitions_.push_back(
                            DeferredStateTransition{b.Transition.pResource, b.Transition.StateAfter});
                    }
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
                    if (device_x_ && device_x_->IsRetailScarlettAbi() &&
                        (b.UAV.pResource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
                        // Desktop D3D12 marks the command list E_INVALIDARG (Close -> 0x80070057) if a
                        // per-resource UAV barrier references a resource created without ALLOW_UNORDERED_ACCESS.
                        b.UAV.pResource = nullptr;
                        ++global_uav_barriers;
                    }
                } else {
                    // Console global UAV barrier (UAV.pResource == nullptr, xcl #68):
                    // valid on DIRECT/COMPUTE lists in D3D12 and maps conservatively to a full
                    // VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT barrier on Vulkan.
                    ++global_uav_barriers;
                    if (device_x_) device_x_->RecordGlobalUavBarrier();
                }
                if (list_type_ == D3D12_COMMAND_LIST_TYPE_COPY) {
                    // UAV barriers are forbidden on D3D12 Copy command lists.
                    if (device_x_) device_x_->RecordCopyQueueBarrierSanitized(true);
                    emit = false;
                }
                break;
            default:
                break;
        }
        if (emit) {
            if (IsDiagTraceEnabled() && b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                char buf[200];
                const D3D12_RESOURCE_DESC rdesc = b.Transition.pResource ? b.Transition.pResource->GetDesc() : D3D12_RESOURCE_DESC{};
                std::snprintf(buf, sizeof(buf),
                              "Barrier res=%p dim=%d fmt=%d w=%llu h=%u sub=%u 0x%X->0x%X flg=0x%X",
                              static_cast<void*>(b.Transition.pResource),
                              static_cast<int>(rdesc.Dimension), static_cast<int>(rdesc.Format),
                              static_cast<unsigned long long>(rdesc.Width), rdesc.Height,
                              b.Transition.Subresource,
                              static_cast<unsigned>(b.Transition.StateBefore),
                              static_cast<unsigned>(b.Transition.StateAfter),
                              static_cast<unsigned>(b.Flags));
                AppendDiagOp(buf);
            }
            if (device_x_ && device_x_->IsRetailScarlettAbi() &&
                b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                b.Transition.pResource != nullptr) {
                const D3D12_RESOURCE_STATES tracked =
                    device_x_->GetRetailResourceState(b.Transition.pResource, b.Transition.StateBefore);
                if (tracked != b.Transition.StateBefore) {
                    b.Transition.StateBefore = tracked;
                }
                if (b.Transition.StateBefore == b.Transition.StateAfter) {
                    continue;
                }
                device_x_->SetRetailResourceState(b.Transition.pResource, b.Transition.StateAfter);
            }
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
        op.barrier_count = std::max<uint32_t>(
            out_count + static_cast<uint32_t>(extra_tiled_barriers.size()),
            global_uav_barriers);
        op.global_uav_barrier_count = global_uav_barriers;
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

HRESULT STDMETHODCALLTYPE CommandListX::GetEnhancedBarrierStats(
    D3D12XBOX_ENHANCED_BARRIER_STATS* out_stats) const noexcept {
    if (!out_stats) return E_POINTER;
    out_stats->barrier_calls = eb_barrier_calls_.load(std::memory_order_relaxed);
    out_stats->native_barrier_calls = eb_native_calls_.load(std::memory_order_relaxed);
    out_stats->legacy_fallback_calls = eb_legacy_fallbacks_.load(std::memory_order_relaxed);
    out_stats->buffer_barriers_processed = eb_buffer_barriers_.load(std::memory_order_relaxed);
    out_stats->texture_barriers_processed = eb_texture_barriers_.load(std::memory_order_relaxed);
    out_stats->global_barriers_processed = eb_global_barriers_.load(std::memory_order_relaxed);
    out_stats->proxy_resources_unwrapped = eb_resources_unwrapped_.load(std::memory_order_relaxed);
    out_stats->va_mirror_notifications = eb_va_mirror_notifs_.load(std::memory_order_relaxed);
    out_stats->unified_texture_notifications = eb_unified_tex_notifs_.load(std::memory_order_relaxed);
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    out_stats->host_has_cmdlist7 = host_list7_ ? 1u : 0u;
#else
    out_stats->host_has_cmdlist7 = 0u;
#endif
    return S_OK;
}

#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
namespace {

D3D12_RESOURCE_STATES BarrierAccessToLegacyState(D3D12_BARRIER_ACCESS access) noexcept {
    if (access == D3D12_BARRIER_ACCESS_COMMON || access == D3D12_BARRIER_ACCESS_NO_ACCESS) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    if ((access & D3D12_BARRIER_ACCESS_RENDER_TARGET) != 0) {
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if ((access & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0) {
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if ((access & D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE) != 0) {
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if ((access & D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ) != 0) {
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if ((access & D3D12_BARRIER_ACCESS_COPY_DEST) != 0) {
        return D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if ((access & D3D12_BARRIER_ACCESS_COPY_SOURCE) != 0) {
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if ((access & D3D12_BARRIER_ACCESS_RESOLVE_DEST) != 0) {
        return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    }
    if ((access & D3D12_BARRIER_ACCESS_RESOLVE_SOURCE) != 0) {
        return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    }
    if ((access & D3D12_BARRIER_ACCESS_STREAM_OUTPUT) != 0) {
        return D3D12_RESOURCE_STATE_STREAM_OUT;
    }
    if ((access & D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ) != 0 ||
        (access & D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE) != 0) {
        return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    }
    D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_COMMON;
    if ((access & (D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER)) != 0) {
        read_state |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if ((access & D3D12_BARRIER_ACCESS_INDEX_BUFFER) != 0) {
        read_state |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if ((access & D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT) != 0) {
        read_state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    if ((access & D3D12_BARRIER_ACCESS_SHADER_RESOURCE) != 0) {
        read_state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    return read_state;
}

D3D12_RESOURCE_STATES BarrierLayoutToLegacyState(
    D3D12_BARRIER_LAYOUT layout,
    D3D12_BARRIER_ACCESS fallback_access) noexcept {
    switch (layout) {
        case D3D12_BARRIER_LAYOUT_UNDEFINED:
            return D3D12_RESOURCE_STATE_COMMON;
        case D3D12_BARRIER_LAYOUT_COMMON:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON: {
            if (fallback_access != D3D12_BARRIER_ACCESS_COMMON &&
                fallback_access != D3D12_BARRIER_ACCESS_NO_ACCESS) {
                const D3D12_RESOURCE_STATES acc_st = BarrierAccessToLegacyState(fallback_access);
                if (acc_st != D3D12_RESOURCE_STATE_COMMON) {
                    return acc_st;
                }
            }
            return D3D12_RESOURCE_STATE_COMMON;
        }
        case D3D12_BARRIER_LAYOUT_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case D3D12_BARRIER_LAYOUT_COPY_DEST:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
            return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
            return D3D12_RESOURCE_STATE_RESOLVE_DEST;
        case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:
            return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
        default:
            return BarrierAccessToLegacyState(fallback_access);
    }
}

bool IsEnhancedWriteAccess(D3D12_BARRIER_ACCESS access) noexcept {
    constexpr uint32_t kWriteMask =
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_COPY_DEST) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RENDER_TARGET) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RESOLVE_DEST) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_STREAM_OUTPUT);
    return (static_cast<uint32_t>(access) & kWriteMask) != 0u;
}

bool IsEnhancedReadAccess(D3D12_BARRIER_ACCESS access) noexcept {
    constexpr uint32_t kReadMask =
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_VERTEX_BUFFER) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_CONSTANT_BUFFER) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_INDEX_BUFFER) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_SHADER_RESOURCE) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_COPY_SOURCE) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RESOLVE_SOURCE) |
        static_cast<uint32_t>(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);
    return (static_cast<uint32_t>(access) & kReadMask) != 0u;
}

bool HostSupportsEnhancedBarriers(ID3D12Device* host_dev) noexcept {
    if (!host_dev) return false;
#if defined(D3D12_SDK_VERSION) && (D3D12_SDK_VERSION >= 608)
    static std::mutex s_eb_mu;
    static std::unordered_map<ID3D12Device*, bool> s_eb_cache;
    {
        std::lock_guard<std::mutex> lk(s_eb_mu);
        auto it = s_eb_cache.find(host_dev);
        if (it != s_eb_cache.end()) return it->second;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12{};
    const bool supported =
        SUCCEEDED(host_dev->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12))) &&
        (opt12.EnhancedBarriersSupported != FALSE);
    {
        std::lock_guard<std::mutex> lk(s_eb_mu);
        s_eb_cache[host_dev] = supported;
    }
    return supported;
#else
    return false;
#endif
}

}  // namespace

void CommandListX::ExecuteEnhancedBarriers(
    UINT32 num_barrier_groups,
    const D3D12_BARRIER_GROUP* barrier_groups) {
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Barrier(groups=%u)", num_barrier_groups);
        AppendDiagOp(buf);
    }
    eb_barrier_calls_.fetch_add(1ull, std::memory_order_relaxed);
    const bool is_retail = (device_x_ && device_x_->IsRetailScarlettAbi());
    const bool use_native_eb =
        (!is_retail) && (host_list7_ != nullptr) && device_x_ &&
        HostSupportsEnhancedBarriers(device_x_->HostDevice());

    if (!barrier_groups || num_barrier_groups == 0u) {
        if (use_native_eb) {
            host_list7_->Barrier(num_barrier_groups, barrier_groups);
            eb_native_calls_.fetch_add(1ull, std::memory_order_relaxed);
        }
        RecordList("ID3D12GraphicsCommandListX::Barrier", 80, S_OK);
        return;
    }

    const bool track_unified =
        device_x_ && (device_x_->HasRegisteredUnifiedTextures() ||
                      device_x_->HasMirroredTitleVaRegions());

    std::vector<D3D12_BARRIER_GROUP> unwrapped_groups(num_barrier_groups);
    std::vector<std::vector<D3D12_BUFFER_BARRIER>> buffer_storage;
    std::vector<std::vector<D3D12_TEXTURE_BARRIER>> texture_storage;
    buffer_storage.reserve(num_barrier_groups);
    texture_storage.reserve(num_barrier_groups);

    std::vector<D3D12_RESOURCE_BARRIER> legacy_barriers;
    uint64_t unwrapped_count = 0ull;
    uint64_t va_notifs = 0ull;
    uint64_t unified_notifs = 0ull;
    uint32_t total_individual_barriers = 0u;

    for (UINT32 g = 0u; g < num_barrier_groups; ++g) {
        D3D12_BARRIER_GROUP grp = barrier_groups[g];
        unwrapped_groups[g] = grp;

        if (grp.Type == D3D12_BARRIER_TYPE_GLOBAL) {
            eb_global_barriers_.fetch_add(grp.NumBarriers, std::memory_order_relaxed);
            total_individual_barriers += grp.NumBarriers;
            if (grp.pGlobalBarriers && grp.NumBarriers > 0u) {
                for (UINT32 i = 0u; i < grp.NumBarriers; ++i) {
                    const D3D12_GLOBAL_BARRIER& gb = grp.pGlobalBarriers[i];
                    if ((gb.AccessBefore & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0 ||
                        (gb.AccessAfter & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0) {
                        D3D12_RESOURCE_BARRIER lb{};
                        lb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        lb.UAV.pResource = nullptr;
                        legacy_barriers.push_back(lb);
                    }
                }
            }
        } else if (grp.Type == D3D12_BARRIER_TYPE_BUFFER) {
            eb_buffer_barriers_.fetch_add(grp.NumBarriers, std::memory_order_relaxed);
            total_individual_barriers += grp.NumBarriers;
            if (grp.pBufferBarriers && grp.NumBarriers > 0u) {
                buffer_storage.emplace_back(grp.pBufferBarriers, grp.pBufferBarriers + grp.NumBarriers);
                auto& buf_vec = buffer_storage.back();
                for (UINT32 i = 0u; i < grp.NumBarriers; ++i) {
                    D3D12_BUFFER_BARRIER& bb = buf_vec[i];
                    ID3D12Resource* orig_res = bb.pResource;
                    ID3D12Resource* host_res = UnwrapResource(orig_res);
                    if (orig_res && host_res != orig_res) {
                        ++unwrapped_count;
                    }
                    bb.pResource = host_res;

                    if (host_res) {
                        const D3D12_RESOURCE_STATES before_st = BarrierAccessToLegacyState(bb.AccessBefore);
                        const D3D12_RESOURCE_STATES after_st = BarrierAccessToLegacyState(bb.AccessAfter);
                        const D3D12_RESOURCE_STATES eff_before =
                            is_retail ? device_x_->GetRetailResourceState(host_res, before_st) : before_st;
                        if (track_unified) {
                            {
                                std::lock_guard<std::mutex> lk(aaa_rec_mutex_);
                                deferred_state_transitions_.push_back(
                                    DeferredStateTransition{host_res, after_st});
                            }
                            ++unified_notifs;
                            uint64_t va = 0ull;
                            if (device_x_->LookupMirroredTitleVaForResource(host_res, va) && va != 0ull) {
                                ++va_notifs;
                                if (IsEnhancedReadAccess(bb.AccessAfter)) {
                                    std::vector<openscarlett::DirtyPageRange> dirty_ranges;
                                    if (device_x_->GetTitleVaMirror().QueryDirtyPageRanges(va, false, dirty_ranges).ok() &&
                                        !dirty_ranges.empty()) {
                                        openscarlett::FlushStats fstats{};
                                        (void)device_x_->GetTitleVaMirror().FlushDirtyPagesToGpu(
                                            host_list_, va, &fstats, after_st);
                                    }
                                }
                            }
                        }
                        if (IsEnhancedWriteAccess(bb.AccessBefore) || IsEnhancedWriteAccess(bb.AccessAfter)) {
                            RecordMirroredResourceGpuWrite(host_res);
                        }
                        if (bb.AccessAfter != D3D12_BARRIER_ACCESS_NO_ACCESS && eff_before != after_st) {
                            D3D12_RESOURCE_BARRIER lb{};
                            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            lb.Transition.pResource = host_res;
                            lb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            lb.Transition.StateBefore = eff_before;
                            lb.Transition.StateAfter = after_st;
                            legacy_barriers.push_back(lb);
                        } else if ((bb.AccessBefore & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0 ||
                                   (bb.AccessAfter & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0) {
                            D3D12_RESOURCE_BARRIER lb{};
                            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            lb.UAV.pResource = host_res;
                            legacy_barriers.push_back(lb);
                        }
                    }
                }
                unwrapped_groups[g].pBufferBarriers = buf_vec.data();
            }
        } else if (grp.Type == D3D12_BARRIER_TYPE_TEXTURE) {
            eb_texture_barriers_.fetch_add(grp.NumBarriers, std::memory_order_relaxed);
            total_individual_barriers += grp.NumBarriers;
            if (grp.pTextureBarriers && grp.NumBarriers > 0u) {
                texture_storage.emplace_back(grp.pTextureBarriers, grp.pTextureBarriers + grp.NumBarriers);
                auto& tex_vec = texture_storage.back();
                for (UINT32 i = 0u; i < grp.NumBarriers; ++i) {
                    D3D12_TEXTURE_BARRIER& tb = tex_vec[i];
                    ID3D12Resource* orig_res = tb.pResource;
                    ID3D12Resource* host_res = UnwrapResource(orig_res);
                    if (orig_res && host_res != orig_res) {
                        ++unwrapped_count;
                    }
                    tb.pResource = host_res;

                    if (host_res) {
                        const D3D12_RESOURCE_STATES before_st =
                            BarrierLayoutToLegacyState(tb.LayoutBefore, tb.AccessBefore);
                        const D3D12_RESOURCE_STATES after_st =
                            BarrierLayoutToLegacyState(tb.LayoutAfter, tb.AccessAfter);
                        const D3D12_RESOURCE_STATES eff_before =
                            is_retail ? device_x_->GetRetailResourceState(host_res, before_st) : before_st;
                        if (track_unified) {
                            {
                                std::lock_guard<std::mutex> lk(aaa_rec_mutex_);
                                deferred_state_transitions_.push_back(
                                    DeferredStateTransition{host_res, after_st});
                            }
                            ++unified_notifs;
                            uint64_t va = 0ull;
                            if (device_x_->LookupMirroredTitleVaForResource(host_res, va) && va != 0ull) {
                                ++va_notifs;
                            }
                        }
                        if (IsEnhancedWriteAccess(tb.AccessBefore) || IsEnhancedWriteAccess(tb.AccessAfter) ||
                            after_st == D3D12_RESOURCE_STATE_COPY_DEST ||
                            after_st == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
                            after_st == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                            RecordMirroredResourceGpuWrite(host_res);
                        }
                        if (tb.LayoutAfter != D3D12_BARRIER_LAYOUT_UNDEFINED && eff_before != after_st) {
                            D3D12_RESOURCE_BARRIER lb{};
                            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            lb.Transition.pResource = host_res;
                            lb.Transition.Subresource =
                                (tb.Subresources.NumMipLevels == 0u)
                                    ? tb.Subresources.IndexOrFirstMipLevel
                                    : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            lb.Transition.StateBefore = eff_before;
                            lb.Transition.StateAfter = after_st;
                            legacy_barriers.push_back(lb);
                        } else if ((tb.AccessBefore & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0 ||
                                   (tb.AccessAfter & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) != 0) {
                            D3D12_RESOURCE_BARRIER lb{};
                            lb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                            lb.UAV.pResource = host_res;
                            legacy_barriers.push_back(lb);
                        }
                    }
                }
                unwrapped_groups[g].pTextureBarriers = tex_vec.data();
            }
        }
    }

    if (unwrapped_count > 0ull) {
        eb_resources_unwrapped_.fetch_add(unwrapped_count, std::memory_order_relaxed);
    }
    if (va_notifs > 0ull) {
        eb_va_mirror_notifs_.fetch_add(va_notifs, std::memory_order_relaxed);
    }
    if (unified_notifs > 0ull) {
        eb_unified_tex_notifs_.fetch_add(unified_notifs, std::memory_order_relaxed);
    }

    if (is_retail) {
        if (!legacy_barriers.empty()) {
            this->ResourceBarrier(static_cast<UINT>(legacy_barriers.size()), legacy_barriers.data());
        }
        eb_legacy_fallbacks_.fetch_add(1ull, std::memory_order_relaxed);
        RecordList("ID3D12GraphicsCommandListX::Barrier", 80, S_OK);
        return;
    }

    std::vector<D3D12_RESOURCE_BARRIER> extra_tiled_barriers;
    if (!legacy_barriers.empty()) {
        TrackPlacedTiledResourceBarriers(
            static_cast<UINT>(legacy_barriers.size()), legacy_barriers.data(), extra_tiled_barriers);
    }

    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::Barrier;
        op.barrier_count = std::max<uint32_t>(1u, total_individual_barriers);
        vk_batch_.ops.push_back(std::move(op));
        vk_batch_.has_host_commands = true;
    }

    if (use_native_eb) {
        host_list7_->Barrier(num_barrier_groups, unwrapped_groups.data());
        eb_native_calls_.fetch_add(1ull, std::memory_order_relaxed);
    } else if (host_list_) {
        // Legacy fallback for Windows 10 22H2 inbox D3D12 without EnhancedBarriersSupported.
        if (!extra_tiled_barriers.empty()) {
            host_list_->ResourceBarrier(
                static_cast<UINT>(extra_tiled_barriers.size()), extra_tiled_barriers.data());
        }
        if (!legacy_barriers.empty()) {
            host_list_->ResourceBarrier(
                static_cast<UINT>(legacy_barriers.size()), legacy_barriers.data());
        }
        eb_legacy_fallbacks_.fetch_add(1ull, std::memory_order_relaxed);
    }
    RecordList("ID3D12GraphicsCommandListX::Barrier", 80, S_OK);
}
#endif  // __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__

void STDMETHODCALLTYPE CommandListX::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    if (IsDiagTraceEnabled()) AppendDiagOp("ExecuteBundle");
    host_list_->ExecuteBundle(UnwrapHostGraphicsList(pCommandList));
    RecordList("ID3D12GraphicsCommandListX::ExecuteBundle", 27, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetDescriptorHeaps(
    UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) {
    if (IsDiagTraceEnabled()) {
        char buf[160];
        uint64_t g0 = 0u, g1 = 0u;
        UINT n0 = 0u, n1 = 0u;
        if (ppDescriptorHeaps && NumDescriptorHeaps >= 1u && ppDescriptorHeaps[0]) {
            ID3D12DescriptorHeap* h0 = UnwrapDescriptorHeap(ppDescriptorHeaps[0]);
            if (h0) {
                n0 = h0->GetDesc().NumDescriptors;
                g0 = h0->GetGPUDescriptorHandleForHeapStart().ptr;
            }
        }
        if (ppDescriptorHeaps && NumDescriptorHeaps >= 2u && ppDescriptorHeaps[1]) {
            ID3D12DescriptorHeap* h1 = UnwrapDescriptorHeap(ppDescriptorHeaps[1]);
            if (h1) {
                n1 = h1->GetDesc().NumDescriptors;
                g1 = h1->GetGPUDescriptorHandleForHeapStart().ptr;
            }
        }
        std::snprintf(buf, sizeof(buf), "SetDescriptorHeaps(n=%u h0=0x%llX[%u] h1=0x%llX[%u])",
                      NumDescriptorHeaps,
                      static_cast<unsigned long long>(g0), n0,
                      static_cast<unsigned long long>(g1), n1);
        AppendDiagOp(buf);
    }
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
        if (bound_cbv_srv_uav_heap_) {
            device_x_->NotifyDescriptorHeapBoundOnCommandList(bound_cbv_srv_uav_heap_);
        }
    }
    host_list_->SetDescriptorHeaps(NumDescriptorHeaps, unwrapped);
    RecordList("ID3D12GraphicsCommandListX::SetDescriptorHeaps", 28, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootSignature(
    ID3D12RootSignature* pRootSignature) {
    ID3D12RootSignature* host_rs = UnwrapRootSignature(pRootSignature);
    last_host_compute_rs_ = host_rs;
    last_cmp_rs_is_empty_ = false;
    if (pRootSignature && IsKnownProxyPointer(pRootSignature)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pRootSignature->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::RootSignature) {
                last_cmp_rs_is_empty_ = static_cast<RootSignatureXProxy*>(pRootSignature)->IsEmptyFallback();
            }
            insp->Release();
        }
    }
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetComputeRootSig(%p empty=%d)",
                      static_cast<void*>(host_rs), last_cmp_rs_is_empty_ ? 1 : 0);
        std::string rs_info = device_x_ ? device_x_->DescribeDiagRootSig(host_rs) : std::string{};
        AppendDiagOp(rs_info.empty() ? std::string(buf) : (std::string(buf) + " " + rs_info));
    }
    if (host_rs) {
        host_list_->SetComputeRootSignature(host_rs);
    }
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootSignature", 29, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootSignature(
    ID3D12RootSignature* pRootSignature) {
    ID3D12RootSignature* host_rs = UnwrapRootSignature(pRootSignature);
    last_host_graphics_rs_ = host_rs;
    last_gfx_rs_is_empty_ = false;
    if (pRootSignature && IsKnownProxyPointer(pRootSignature)) {
        IOpenScarlettProxyInspect* insp = nullptr;
        if (SUCCEEDED(pRootSignature->QueryInterface(IID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(&insp))) && insp) {
            if (insp->GetProxyKind() == D3D12XProxyKind::RootSignature) {
                last_gfx_rs_is_empty_ = static_cast<RootSignatureXProxy*>(pRootSignature)->IsEmptyFallback();
            }
            insp->Release();
        }
    }
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetGraphicsRootSig(%p empty=%d)",
                      static_cast<void*>(host_rs), last_gfx_rs_is_empty_ ? 1 : 0);
        std::string rs_info = device_x_ ? device_x_->DescribeDiagRootSig(host_rs) : std::string{};
        AppendDiagOp(rs_info.empty() ? std::string(buf) : (std::string(buf) + " " + rs_info));
    }
    if (host_rs) {
        host_list_->SetGraphicsRootSignature(host_rs);
    }
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootSignature", 30, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    bool is_sampler_param = false;
    const bool is_table_param =
        !device_x_ || device_x_->IsValidRootDescriptorTableParam(
                          last_host_compute_rs_, RootParameterIndex, &is_sampler_param);
    if (device_x_) {
        BaseDescriptor = device_x_->TranslateGpuDescriptorHandle(BaseDescriptor);
        if (is_table_param && !is_sampler_param) {
            BaseDescriptor = device_x_->MaterializeGuestDescriptorTable(
                BaseDescriptor, last_host_compute_rs_, RootParameterIndex, nullptr, nullptr, /*is_compute=*/true);
        }
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, BaseDescriptor.ptr);
        device_x_->RecordBridgeTableBind();
    }
    bool valid_for_host = is_table_param && (BaseDescriptor.ptr != 0ull);
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        ID3D12DescriptorHeap* req_heap =
            is_sampler_param ? bound_sampler_heap_ : bound_cbv_srv_uav_heap_;
        if (!req_heap) {
            valid_for_host = false;
        } else {
            const D3D12_DESCRIPTOR_HEAP_DESC hd = req_heap->GetDesc();
            const uint64_t h_start = req_heap->GetGPUDescriptorHandleForHeapStart().ptr;
            const uint32_t inc = device_x_->GetDescriptorHandleIncrementSize(hd.Type);
            const uint64_t h_end = h_start + static_cast<uint64_t>(hd.NumDescriptors) * inc;
            if (BaseDescriptor.ptr < h_start || BaseDescriptor.ptr >= h_end) {
                BaseDescriptor.ptr = h_start;
            }
            valid_for_host = (BaseDescriptor.ptr != 0ull);
        }
    }
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "SetCmpRootDescTable(rp=%u samp=%d gpu=0x%llX)",
                      RootParameterIndex, is_sampler_param ? 1 : 0,
                      static_cast<unsigned long long>(BaseDescriptor.ptr));
        AppendDiagOp(buf);
    }
    if (valid_for_host && !last_cmp_rs_is_empty_) {
        host_list_->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootDescriptorTable", 31, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootDescriptorTable(
    UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) {
    const D3D12_GPU_DESCRIPTOR_HANDLE orig_gpu = BaseDescriptor;
    bool is_sampler_param = false;
    const bool is_table_param =
        !device_x_ || device_x_->IsValidRootDescriptorTableParam(
                          last_host_graphics_rs_, RootParameterIndex, &is_sampler_param);
    uint32_t mat_hits = 0u;
    uint32_t mat_slots = 0u;
    if (device_x_) {
        BaseDescriptor = device_x_->TranslateGpuDescriptorHandle(BaseDescriptor);
        if (is_table_param && !is_sampler_param) {
            BaseDescriptor = device_x_->MaterializeGuestDescriptorTable(
                BaseDescriptor, last_host_graphics_rs_, RootParameterIndex, &mat_hits, &mat_slots);
        }
        const uint32_t sgpr_slot = (RootParameterIndex * 2u) % 14u;
        (void)device_x_->GetDescriptorBridge().SetRootSgprTablePtr(sgpr_slot, BaseDescriptor.ptr);
        device_x_->RecordBridgeTableBind();
    }
    bool valid_for_host = is_table_param && (BaseDescriptor.ptr != 0ull);
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        ID3D12DescriptorHeap* req_heap =
            is_sampler_param ? bound_sampler_heap_ : bound_cbv_srv_uav_heap_;
        if (!req_heap) {
            valid_for_host = false;
        } else {
            const D3D12_DESCRIPTOR_HEAP_DESC hd = req_heap->GetDesc();
            const uint64_t h_start = req_heap->GetGPUDescriptorHandleForHeapStart().ptr;
            const uint32_t inc = device_x_->GetDescriptorHandleIncrementSize(hd.Type);
            const uint64_t h_end = h_start + static_cast<uint64_t>(hd.NumDescriptors) * inc;
            if (BaseDescriptor.ptr < h_start || BaseDescriptor.ptr >= h_end) {
                BaseDescriptor.ptr = h_start;
            }
            valid_for_host = (BaseDescriptor.ptr != 0ull);
        }
    }
    if (IsDiagTraceEnabled()) {
        std::string extra = (!is_sampler_param && device_x_)
                                ? device_x_->DescribeDiagDescriptorTable(orig_gpu, 4u)
                                : std::string{};
        char buf[160];
        std::snprintf(buf, sizeof(buf), "SetGfxRootDescTable(rp=%u samp=%d gpu=0x%llX->0x%llX mat=%u/%u skip=%d)",
                      RootParameterIndex, is_sampler_param ? 1 : 0,
                      static_cast<unsigned long long>(orig_gpu.ptr),
                      static_cast<unsigned long long>(BaseDescriptor.ptr),
                      mat_hits, mat_slots,
                      last_gfx_rs_is_empty_ ? 1 : 0);
        AppendDiagOp(extra.empty() ? std::string(buf) : (std::string(buf) + " " + extra));
    }
    if (valid_for_host && !last_gfx_rs_is_empty_) {
        host_list_->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
    }
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
    if (!last_cmp_rs_is_empty_) {
        host_list_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetGfxRoot32BitConst(rp=%u off=%u)", RootParameterIndex, DestOffsetIn32BitValues);
        AppendDiagOp(buf);
    }
    if (!last_gfx_rs_is_empty_) {
        host_list_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
    }
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
    if (!last_cmp_rs_is_empty_) {
        host_list_->SetComputeRoot32BitConstants(
            RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetGfxRoot32BitConsts(rp=%u n=%u off=%u)",
                      RootParameterIndex, Num32BitValuesToSet, DestOffsetIn32BitValues);
        AppendDiagOp(buf);
    }
    if (!last_gfx_rs_is_empty_) {
        host_list_->SetGraphicsRoot32BitConstants(
            RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
    }
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRoot32BitConstants", 36, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrUploadRootCbvGpuVa(BufferLocation) : BufferLocation;
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
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "SetCmpRootCBV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex, static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_cmp_rs_is_empty_ && translated != 0ull) {
        host_list_->SetComputeRootConstantBufferView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootConstantBufferView", 37, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootConstantBufferView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrUploadRootCbvGpuVa(BufferLocation) : BufferLocation;
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
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetGfxRootCBV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex,
                      static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_gfx_rs_is_empty_ && translated != 0ull) {
        host_list_->SetGraphicsRootConstantBufferView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootConstantBufferView", 38, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrUploadRootSrvGpuVa(BufferLocation) : BufferLocation;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_cmp_srvs_.size() <= RootParameterIndex) vk_cmp_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_cmp_srvs_[RootParameterIndex] = translated;
    }
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "SetCmpRootSRV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex, static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_cmp_rs_is_empty_ && translated != 0ull) {
        host_list_->SetComputeRootShaderResourceView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootShaderResourceView", 39, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootShaderResourceView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrUploadRootSrvGpuVa(BufferLocation) : BufferLocation;
    if (RootParameterIndex < 16u) {
        last_gfx_root_srv_raw_[RootParameterIndex] = BufferLocation;
        last_gfx_root_srv_host_[RootParameterIndex] = translated;
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (vk_gfx_srvs_.size() <= RootParameterIndex) vk_gfx_srvs_.resize(static_cast<size_t>(RootParameterIndex) + 1u, 0ull);
        vk_gfx_srvs_[RootParameterIndex] = translated;
    }
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetGfxRootSRV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex,
                      static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_gfx_rs_is_empty_ && translated != 0ull) {
        host_list_->SetGraphicsRootShaderResourceView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootShaderResourceView", 40, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetComputeRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrDummyRootUavGpuVa(BufferLocation) : BufferLocation;
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
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "SetCmpRootUAV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex, static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_cmp_rs_is_empty_ && translated != 0ull) {
        host_list_->SetComputeRootUnorderedAccessView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetComputeRootUnorderedAccessView", 41, S_OK);
}

void STDMETHODCALLTYPE CommandListX::SetGraphicsRootUnorderedAccessView(
    UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        device_x_ ? device_x_->ResolveOrDummyRootUavGpuVa(BufferLocation) : BufferLocation;
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
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetGfxRootUAV(rp=%u raw=0x%llX -> 0x%llX)",
                      RootParameterIndex,
                      static_cast<unsigned long long>(BufferLocation),
                      static_cast<unsigned long long>(translated));
        AppendDiagOp(buf);
    }
    if (!last_gfx_rs_is_empty_ && translated != 0ull) {
        host_list_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, translated);
    }
    RecordList("ID3D12GraphicsCommandListX::SetGraphicsRootUnorderedAccessView", 42, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) {
    D3D12_INDEX_BUFFER_VIEW translated_view{};
    const D3D12_INDEX_BUFFER_VIEW* pass_view = pView;
    if (pView) {
        translated_view = *pView;
        bool defer_ib_upload = false;
        if (device_x_ && translated_view.BufferLocation != 0ull) {
            translated_view.BufferLocation =
                device_x_->TranslateGuestGpuVa(translated_view.BufferLocation);
            const bool unmapped_va = (translated_view.BufferLocation == pView->BufferLocation);
            bool need_guest_upload = unmapped_va;
            if (!need_guest_upload && device_x_->IsRetailScarlettAbi()) {
                uint64_t ib_off = 0ull;
                ID3D12Resource* ib_res = device_x_->ResolveBufferResourceByGpuVa(pView->BufferLocation, &ib_off);
                if (ib_res && !device_x_->IsGpuWrittenBufferRange(ib_res, ib_off, pView->SizeInBytes)) {
                    need_guest_upload = true;
                }
            }
            defer_ib_upload =
                device_x_->IsRetailScarlettAbi() && need_guest_upload && pView->SizeInBytes > 0u;
            if (unmapped_va && defer_ib_upload) {
                translated_view.BufferLocation = 0ull;
                translated_view.SizeInBytes = 0u;
            }
        }
        pass_view = (translated_view.BufferLocation != 0ull) ? &translated_view : nullptr;
        last_ib_raw_ = *pView;
        last_ib_host_ = translated_view;
        last_ib_need_upload_ = defer_ib_upload;
    } else {
        last_ib_raw_ = {};
        last_ib_host_ = {};
        last_ib_need_upload_ = false;
    }
    if (IsDiagTraceEnabled() && pView) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "IASetIndexBuffer(raw=0x%llX->0x%llX sz=%u fmt=%d)",
                      static_cast<unsigned long long>(pView->BufferLocation),
                      static_cast<unsigned long long>(translated_view.BufferLocation),
                      pView->SizeInBytes, static_cast<int>(pView->Format));
        AppendDiagOp(buf);
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        if (pass_view) {
            vk_cur_ibv_ = *pass_view;
            vk_has_ibv_ = true;
        } else {
            vk_has_ibv_ = false;
        }
    }
    static const bool trace_sf = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_SF_DRAW", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_sf_ib_log{0u};
    if (trace_sf && pView && s_sf_ib_log.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        uint16_t ib16[6]{};
        if (pView->BufferLocation >= 0x10000ull && pView->SizeInBytes >= sizeof(ib16)) {
            (void)SafeCopyBytes(ib16, reinterpret_cast<const void*>(static_cast<uintptr_t>(pView->BufferLocation)), sizeof(ib16));
        }
        std::fprintf(stderr,
                     "[OpenScarlett][SF-IB] raw=0x%llX -> host=0x%llX (mapped=%d) sz=%u fmt=%d idx0..5=[%u,%u,%u,%u,%u,%u]\n",
                     static_cast<unsigned long long>(pView->BufferLocation),
                     static_cast<unsigned long long>(translated_view.BufferLocation),
                     (translated_view.BufferLocation != pView->BufferLocation) ? 1 : 0,
                     pView->SizeInBytes, static_cast<int>(pView->Format),
                     ib16[0], ib16[1], ib16[2], ib16[3], ib16[4], ib16[5]);
        std::fflush(stderr);
    }
    host_list_->IASetIndexBuffer(pass_view);
    RecordList("ID3D12GraphicsCommandListX::IASetIndexBuffer", 43, S_OK);
}

void STDMETHODCALLTYPE CommandListX::IASetVertexBuffers(
    UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) {
    if (NumViews == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::IASetVertexBuffers[ZeroNoop]", 44, S_OK);
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW stack_views[16]{};
    std::vector<D3D12_VERTEX_BUFFER_VIEW> heap_views;
    D3D12_VERTEX_BUFFER_VIEW* translated = stack_views;
    const D3D12_VERTEX_BUFFER_VIEW* pass_views = pViews;
    if (pViews) {
        if (NumViews > 16u) {
            heap_views.resize(NumViews);
            translated = heap_views.data();
        }
        for (UINT i = 0u; i < NumViews; ++i) {
            translated[i] = pViews[i];
            bool defer_vb_upload = false;
            if (device_x_ && translated[i].BufferLocation != 0ull) {
                translated[i].BufferLocation =
                    device_x_->TranslateGuestGpuVa(translated[i].BufferLocation);
                const bool unmapped_va = (translated[i].BufferLocation == pViews[i].BufferLocation);
                bool need_guest_upload = unmapped_va;
                if (!need_guest_upload && device_x_->IsRetailScarlettAbi()) {
                    uint64_t vb_off = 0ull;
                    ID3D12Resource* vb_res =
                        device_x_->ResolveBufferResourceByGpuVa(pViews[i].BufferLocation, &vb_off);
                    if (vb_res && !device_x_->IsGpuWrittenBufferRange(vb_res, vb_off, pViews[i].SizeInBytes)) {
                        need_guest_upload = true;
                    }
                }
                defer_vb_upload =
                    device_x_->IsRetailScarlettAbi() && need_guest_upload && pViews[i].SizeInBytes > 0u;
                if (unmapped_va && defer_vb_upload) {
                    translated[i].BufferLocation = 0ull;
                    translated[i].SizeInBytes = 0u;
                }
            }
            const UINT slot = StartSlot + i;
            if (slot < 8u) {
                last_vb_raw_[slot] = pViews[i];
                last_vb_host_[slot] = translated[i];
                last_vb_need_upload_[slot] = defer_vb_upload;
                if (pViews[i].BufferLocation != 0ull && pViews[i].SizeInBytes > 0u) {
                    last_vb_valid_mask_ |= static_cast<uint8_t>(1u << slot);
                } else {
                    last_vb_valid_mask_ &= static_cast<uint8_t>(~(1u << slot));
                }
            }
        }
        pass_views = translated;
        if (StartSlot == 0u && NumViews > 0u) {
            last_vb0_raw_ = pViews[0];
            last_vb0_host_ = translated[0];
        }
    } else {
        for (UINT i = 0u; i < NumViews; ++i) {
            const UINT slot = StartSlot + i;
            if (slot < 8u) {
                last_vb_raw_[slot] = {};
                last_vb_host_[slot] = {};
                last_vb_need_upload_[slot] = false;
                last_vb_valid_mask_ &= static_cast<uint8_t>(~(1u << slot));
            }
        }
        if (StartSlot == 0u && NumViews > 0u) {
            last_vb0_raw_ = {};
            last_vb0_host_ = {};
        }
    }
    if (IsDiagTraceEnabled() && pViews) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "IASetVertexBuffers(slot=%u n=%u raw=0x%llX->0x%llX)",
                      StartSlot, NumViews,
                      static_cast<unsigned long long>(pViews[0].BufferLocation),
                      static_cast<unsigned long long>(translated[0].BufferLocation));
        AppendDiagOp(buf);
    }
    static const bool trace_sf_vb = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_SF_DRAW", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_sf_vb_log{0u};
    if (trace_sf_vb && pViews && pViews[0].StrideInBytes == 16u &&
        s_sf_vb_log.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        const uint32_t sz = pViews[0].SizeInBytes;
        std::fprintf(stderr,
                     "[OpenScarlett][SF-VB] raw=0x%llX -> host=0x%llX (mapped=%d) sz=%u stride=16",
                     static_cast<unsigned long long>(pViews[0].BufferLocation),
                     static_cast<unsigned long long>(translated[0].BufferLocation),
                     (translated[0].BufferLocation != pViews[0].BufferLocation) ? 1 : 0, sz);
        if (pViews[0].BufferLocation >= 0x10000ull && sz >= 48u) {
            uint8_t vb[48]{};
            if (SafeCopyBytes(vb, reinterpret_cast<const void*>(static_cast<uintptr_t>(pViews[0].BufferLocation)), sizeof(vb))) {
                for (uint32_t vi = 0u; vi < 3u; ++vi) {
                    uint32_t c0 = 0u, c1 = 0u;
                    float px = 0.f, py = 0.f;
                    std::memcpy(&c0, vb + vi * 16u + 0u, 4u);
                    std::memcpy(&c1, vb + vi * 16u + 4u, 4u);
                    std::memcpy(&px, vb + vi * 16u + 8u, 4u);
                    std::memcpy(&py, vb + vi * 16u + 12u, 4u);
                    std::fprintf(stderr, " v%u(c0=0x%08X c1=0x%08X pos=%.1f,%.1f)", vi, c0, c1, px, py);
                }
            }
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    static const bool trace_dd = []() {
        char v[16]{};
        return ::GetEnvironmentVariableA("OPENSCARLETT_TRACE_DRAW_DATA", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    static std::atomic<uint32_t> s_vb_log{0u};
    if (trace_dd && pViews && pViews[0].BufferLocation >= 0x10000000ull &&
        s_vb_log.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        const uint32_t stride = pViews[0].StrideInBytes;
        const uint32_t sz = pViews[0].SizeInBytes;
        std::fprintf(stderr,
                     "[OpenScarlett][VB] raw=0x%llX -> host=0x%llX sz=%u stride=%u\n",
                     static_cast<unsigned long long>(pViews[0].BufferLocation),
                     static_cast<unsigned long long>(translated[0].BufferLocation), sz, stride);
        if (stride == 24u && sz >= 72u) {
            const auto* vb = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pViews[0].BufferLocation));
            for (uint32_t vi = 0u; vi < 3u; ++vi) {
                float px = 0, py = 0, pz = 0, u = 0, v = 0;
                uint32_t col = 0;
                std::memcpy(&px, vb + vi * 24u + 0u, 4u);
                std::memcpy(&py, vb + vi * 24u + 4u, 4u);
                std::memcpy(&pz, vb + vi * 24u + 8u, 4u);
                std::memcpy(&col, vb + vi * 24u + 12u, 4u);
                std::memcpy(&u, vb + vi * 24u + 16u, 4u);
                std::memcpy(&v, vb + vi * 24u + 20u, 4u);
                std::fprintf(stderr, "  v[%u] pos=(%.3f, %.3f, %.3f) col=0x%08X uv=(%.3f, %.3f)\n",
                             vi, px, py, pz, col, u, v);
            }
        }
        std::fflush(stderr);
    }
    if (device_x_ && device_x_->IsVulkanRoutingActive() && StartSlot == 0u && pass_views) {
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        vk_cur_vbv_ = pass_views[0];
        vk_has_vbv_ = true;
    }
    host_list_->IASetVertexBuffers(StartSlot, NumViews, pass_views);
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
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        D3D12_CPU_DESCRIPTOR_HANDLE clean_rtvs[8]{};
        UINT clean_n = 0u;
        const UINT cap = std::min<UINT>(NumRenderTargetDescriptors, 8u);
        const UINT rtv_inc = device_x_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0u; i < cap; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h =
                RTsSingleHandleToDescriptorRange
                    ? D3D12_CPU_DESCRIPTOR_HANDLE{pRenderTargetDescriptors[0].ptr + static_cast<SIZE_T>(i) * rtv_inc}
                    : pRenderTargetDescriptors[i];
            if (device_x_->IsValidRetailRtvHandle(h.ptr)) {
                clean_rtvs[clean_n++] = h;
            }
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE* clean_dsv =
            (pDepthStencilDescriptor && device_x_->IsValidRetailDsvHandle(pDepthStencilDescriptor->ptr))
                ? pDepthStencilDescriptor
                : nullptr;
        ID3D12Resource* dsv_res =
            clean_dsv ? device_x_->LookupRetailDsvResource(clean_dsv->ptr) : nullptr;
        if (clean_dsv && dsv_res && clean_n > 0u) {
            const D3D12_RESOURCE_DESC dd = dsv_res->GetDesc();
            for (UINT k = 0u; k < clean_n; ++k) {
                ID3D12Resource* rk = device_x_->LookupRetailRtvResource(clean_rtvs[k].ptr);
                if (!rk) continue;
                const D3D12_RESOURCE_DESC rd = rk->GetDesc();
                if (dd.Width < rd.Width || dd.Height < rd.Height ||
                    dd.SampleDesc.Count != rd.SampleDesc.Count) {
                    static std::atomic<uint32_t> s_dsv_drop{0u};
                    if (s_dsv_drop.fetch_add(1u, std::memory_order_relaxed) < 5u) {
                        std::fprintf(
                            stderr,
                            "[OpenScarlett][d3d12_xs] OMSetRenderTargets: dropped mismatched stale DSV "
                            "(%llux%u samp=%u) for RTV%u (%llux%u samp=%u)\n",
                            static_cast<unsigned long long>(dd.Width), dd.Height, dd.SampleDesc.Count,
                            k, static_cast<unsigned long long>(rd.Width), rd.Height, rd.SampleDesc.Count);
                        std::fflush(stderr);
                    }
                    clean_dsv = nullptr;
                    dsv_res = nullptr;
                    break;
                }
            }
        }
        for (UINT k = 0u; k < clean_n; ++k) {
            if (ID3D12Resource* rk = device_x_->LookupRetailRtvResource(clean_rtvs[k].ptr)) {
                device_x_->NotifyGpuWrittenTexture(rk);
            }
        }
        ID3D12Resource* prev_rtv0 = last_bound_rtv0_res_;
        ID3D12Resource* next_rtv0 =
            (clean_n > 0u) ? device_x_->LookupRetailRtvResource(clean_rtvs[0].ptr) : nullptr;
        // Scarlett titles synchronize passes via FlushGpuCachesX instead of D3D12 ResourceBarrier.
        // Emit explicit RENDER_TARGET -> PIXEL_SHADER_RESOURCE (flushing ROP/DCC decompression so
        // subsequent passes like UI blit sample fresh pixels) and -> RENDER_TARGET transitions.
        if (prev_rtv0 && prev_rtv0 != next_rtv0) {
            const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
                prev_rtv0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (before != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = prev_rtv0;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = before;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                host_list_->ResourceBarrier(1u, &tb);
            }
        }
        if (next_rtv0) {
            const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
                next_rtv0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = next_rtv0;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = before;
                tb.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                host_list_->ResourceBarrier(1u, &tb);
            }
        }
        if (dsv_res) {
            const D3D12_RESOURCE_STATES target_dsv_st =
                (clean_dsv && device_x_->IsReadOnlyRetailDsvHandle(clean_dsv->ptr))
                    ? D3D12_RESOURCE_STATE_DEPTH_READ
                    : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
                dsv_res, target_dsv_st, D3D12_RESOURCE_STATE_COMMON);
            if (before != target_dsv_st) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = dsv_res;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = before;
                tb.Transition.StateAfter = target_dsv_st;
                host_list_->ResourceBarrier(1u, &tb);
            }
        }
        last_bound_rtv0_res_ = next_rtv0;
        last_bound_rtv_count_ = clean_n;
        for (UINT k = 0u; k < 8u; ++k) {
            last_bound_rtv_formats_[k] =
                (k < clean_n) ? device_x_->LookupRetailRtvFormat(clean_rtvs[k].ptr) : DXGI_FORMAT_UNKNOWN;
        }
        last_bound_dsv_format_ =
            clean_dsv ? device_x_->LookupRetailDsvFormat(clean_dsv->ptr) : DXGI_FORMAT_UNKNOWN;
        if (IsDiagTraceEnabled()) {
            char buf[220];
            const D3D12_RESOURCE_DESC dd_diag = dsv_res ? dsv_res->GetDesc() : D3D12_RESOURCE_DESC{};
            std::snprintf(buf, sizeof(buf),
                          "OMSetRenderTargets(n=%u->%u rtv0=0x%llX dsv=%d->%d dsv_h=0x%llX dsv=%p[%llux%u D=%u fmt=%d])",
                          NumRenderTargetDescriptors, clean_n,
                          clean_n > 0u ? static_cast<unsigned long long>(clean_rtvs[0].ptr) : 0ull,
                          pDepthStencilDescriptor ? 1 : 0, clean_dsv ? 1 : 0,
                          clean_dsv ? static_cast<unsigned long long>(clean_dsv->ptr) : 0ull,
                          static_cast<void*>(dsv_res),
                          static_cast<unsigned long long>(dd_diag.Width), dd_diag.Height,
                          dd_diag.DepthOrArraySize, static_cast<int>(dd_diag.Format));
            std::string rtv_info = (clean_n > 0u && device_x_) ? device_x_->DescribeDiagRtv(clean_rtvs[0].ptr) : std::string{};
            AppendDiagOp(rtv_info.empty() ? std::string(buf) : (std::string(buf) + " [" + rtv_info + "]"));
        }
        host_list_->OMSetRenderTargets(clean_n, clean_n > 0u ? clean_rtvs : nullptr, FALSE, clean_dsv);
    } else {
        if (IsDiagTraceEnabled()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "OMSetRenderTargets(n=%u dsv=%d)",
                          NumRenderTargetDescriptors, pDepthStencilDescriptor ? 1 : 0);
            AppendDiagOp(buf);
        }
        host_list_->OMSetRenderTargets(
            NumRenderTargetDescriptors, pRenderTargetDescriptors,
            RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
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
    const bool invalid_dsv =
        device_x_ && device_x_->IsRetailScarlettAbi() &&
        !device_x_->IsValidRetailDsvHandle(DepthStencilView.ptr);
    if (IsDiagTraceEnabled()) {
        AppendDiagOp(invalid_dsv ? "ClearDepthStencilView[SKIP_INVALID]" : "ClearDepthStencilView");
    }
    if (!invalid_dsv) {
        if (device_x_ && device_x_->IsRetailScarlettAbi()) {
            if (ID3D12Resource* dsv_res = device_x_->LookupRetailDsvResource(DepthStencilView.ptr)) {
                const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
                    dsv_res, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON);
                if (before != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = dsv_res;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = before;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    host_list_->ResourceBarrier(1u, &b);
                }
            }
        }
        host_list_->ClearDepthStencilView(
            DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
    }
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
    const bool invalid_rtv =
        device_x_ && device_x_->IsRetailScarlettAbi() &&
        !device_x_->IsValidRetailRtvHandle(RenderTargetView.ptr);
    if (IsDiagTraceEnabled()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "ClearRenderTargetView%s(cpu=0x%llX nr=%u r0=(%ld,%ld,%ld,%ld))",
                      invalid_rtv ? "[SKIP_INVALID]" : "",
                      static_cast<unsigned long long>(RenderTargetView.ptr),
                      NumRects,
                      (NumRects > 0u && pRects) ? pRects[0].left : 0L,
                      (NumRects > 0u && pRects) ? pRects[0].top : 0L,
                      (NumRects > 0u && pRects) ? pRects[0].right : 0L,
                      (NumRects > 0u && pRects) ? pRects[0].bottom : 0L);
        AppendDiagOp(buf);
    }
    if (!invalid_rtv) {
        static constexpr FLOAT kZeroClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const FLOAT* eff_rgba = ColorRGBA ? ColorRGBA : kZeroClear;
        ID3D12Resource* rtv_res =
            (device_x_ && device_x_->IsRetailScarlettAbi())
                ? device_x_->LookupRetailRtvResource(RenderTargetView.ptr)
                : nullptr;
        if (rtv_res) {
            device_x_->NotifyGpuWrittenTexture(rtv_res);
            static const bool s_log_clears = []() {
                char b[16]{};
                return ::GetEnvironmentVariableA("OPENSCARLETT_CAPTURE_DIR", b, sizeof(b)) > 0;
            }();
            if (s_log_clears) {
                static std::atomic<uint32_t> s_clr_cnt{0u};
                const uint32_t cn = s_clr_cnt.fetch_add(1u, std::memory_order_relaxed);
                if (cn < 16u) {
                    const D3D12_RESOURCE_DESC rd = rtv_res->GetDesc();
                    std::fprintf(stderr,
                                 "[OpenScarlett][d3d12_xs] CLEAR-RTV #%u res=%p (%llux%u fmt=%d) rgba=(%.3f,%.3f,%.3f,%.3f)\n",
                                 cn, static_cast<void*>(rtv_res),
                                 static_cast<unsigned long long>(rd.Width), rd.Height,
                                 static_cast<int>(rd.Format),
                                 eff_rgba[0], eff_rgba[1], eff_rgba[2], eff_rgba[3]);
                    std::fflush(stderr);
                }
            }
            const D3D12_RESOURCE_STATES before = device_x_->TransitionRetailResourceState(
                rtv_res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            if (before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = rtv_res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = before;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                host_list_->ResourceBarrier(1u, &b);
            }
        }
        host_list_->ClearRenderTargetView(RenderTargetView, eff_rgba, NumRects, pRects);
    }
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
    if (!host_res || (host_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint[NoUavFlagSkip]", 49, S_OK);
        return;
    }
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "ClearUavUint(res=%p, gpu=0x%llX, cpu=0x%llX)",
                      static_cast<void*>(host_res),
                      static_cast<unsigned long long>(ViewGPUHandleInCurrentHeap.ptr),
                      static_cast<unsigned long long>(ViewCPUHandle.ptr));
        AppendDiagOp(buf);
    }
    host_list_->ClearUnorderedAccessViewUint(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, host_res, Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewUint", 49, S_OK);
}

void STDMETHODCALLTYPE CommandListX::ClearUnorderedAccessViewFloat(
    D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
    const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    if (!host_res || (host_res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewFloat[NoUavFlagSkip]", 50, S_OK);
        return;
    }
    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "ClearUavFloat(res=%p, gpu=0x%llX, cpu=0x%llX)",
                      static_cast<void*>(host_res),
                      static_cast<unsigned long long>(ViewGPUHandleInCurrentHeap.ptr),
                      static_cast<unsigned long long>(ViewCPUHandle.ptr));
        AppendDiagOp(buf);
    }
    host_list_->ClearUnorderedAccessViewFloat(
        ViewGPUHandleInCurrentHeap, ViewCPUHandle, host_res, Values, NumRects, pRects);
    RecordList("ID3D12GraphicsCommandListX::ClearUnorderedAccessViewFloat", 50, S_OK);
}

void STDMETHODCALLTYPE CommandListX::DiscardResource(
    ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) {
    ID3D12Resource* host_res = UnwrapResource(pResource);
    if (IsDiagTraceEnabled()) {
        AppendDiagOp("DiscardResource");
    }
    if (!host_res) {
        RecordList("ID3D12GraphicsCommandListX::DiscardResource[NullResNoop]", 51, S_OK);
        return;
    }
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        // Desktop D3D12 DiscardResource faults or marks E_INVALIDARG unless resource has ALLOW_RENDER_TARGET
        // or ALLOW_DEPTH_STENCIL (unlike Xbox UMA render-graph aliasing). DiscardResource is a hint only.
        const D3D12_RESOURCE_FLAGS flg = host_res->GetDesc().Flags;
        if ((flg & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) == 0) {
            RecordList("ID3D12GraphicsCommandListX::DiscardResource[HintNoop]", 51, S_OK);
            return;
        }
    }
    host_list_->DiscardResource(host_res, pRegion);
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "BeginQuery(type=%d idx=%u)", static_cast<int>(Type), Index);
        AppendDiagOp(buf);
    }
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "EndQuery(type=%d idx=%u)", static_cast<int>(Type), Index);
        AppendDiagOp(buf);
    }
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ResolveQueryData(type=%d start=%u n=%u)",
                      static_cast<int>(Type), StartIndex, NumQueries);
        AppendDiagOp(buf);
    }
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SetPredication(buf=%p off=%llu op=%d)",
                      static_cast<void*>(pBuffer), static_cast<unsigned long long>(AlignedBufferOffset),
                      static_cast<int>(Operation));
        AppendDiagOp(buf);
    }
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
    CommandSignatureXProxy* sig_proxy = TryGetCommandSignatureProxy(pCommandSignature);
    ID3D12CommandSignature* host_sig = UnwrapCommandSignature(pCommandSignature);
    ID3D12Resource* host_arg = UnwrapResource(pArgumentBuffer);
    ID3D12Resource* host_cnt = UnwrapResource(pCountBuffer);

    const bool has_count = (host_cnt != nullptr);
    const bool has_root_consts = (sig_proxy != nullptr) && sig_proxy->HasRootConstants();
    const bool is_dispatch_mesh = (sig_proxy != nullptr) && sig_proxy->HasDispatchMesh();

    if (MaxCommandCount == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect[ZeroNoop]", 59, S_OK);
        return;
    }

    // Strict argument-buffer & alignment validation (Sprint 16 ExecuteIndirect completeness).
    if (!pCommandSignature || !host_arg ||
        (ArgumentBufferOffset & 3ull) != 0ull ||
        (has_count && (CountBufferOffset & 3ull) != 0ull)) {
        if (device_x_) {
            device_x_->RecordExecuteIndirectStats(
                has_count, has_root_consts, is_dispatch_mesh,
                false, /*rejected=*/true, false, false);
        }
        RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect[RejectedInvalid]", 59, E_INVALIDARG);
        return;
    }

    const D3D12_RESOURCE_DESC arg_desc = host_arg->GetDesc();
    const UINT64 arg_buf_size = (arg_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) ? arg_desc.Width : 0ull;
    const UINT stride = (sig_proxy && sig_proxy->ByteStride() > 0u) ? sig_proxy->ByteStride() : 16u;
    const UINT min_stride = (sig_proxy && sig_proxy->MinRequiredStride() > 0u) ? sig_proxy->MinRequiredStride() : stride;

    if (arg_buf_size == 0ull || stride < min_stride || ArgumentBufferOffset + min_stride > arg_buf_size) {
        if (device_x_) {
            device_x_->RecordExecuteIndirectStats(
                has_count, has_root_consts, is_dispatch_mesh,
                false, /*rejected=*/true, false, false);
        }
        RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect[RejectedOob]", 59, E_INVALIDARG);
        return;
    }
    if (has_count) {
        const D3D12_RESOURCE_DESC cnt_desc = host_cnt->GetDesc();
        const UINT64 cnt_buf_size = (cnt_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) ? cnt_desc.Width : 0ull;
        if (cnt_buf_size < 4ull || CountBufferOffset + 4ull > cnt_buf_size) {
            if (device_x_) {
                device_x_->RecordExecuteIndirectStats(
                    has_count, has_root_consts, is_dispatch_mesh,
                    false, /*rejected=*/true, false, false);
            }
            RecordList("ID3D12GraphicsCommandListX::ExecuteIndirect[RejectedCountOob]", 59, E_INVALIDARG);
            return;
        }
    }

    // Clamp MaxCommandCount to argument buffer capacity so an oversized console count never faults the GPU.
    const UINT64 avail_bytes = arg_buf_size - ArgumentBufferOffset;
    const UINT64 max_fit64 = 1ull + ((avail_bytes - static_cast<UINT64>(min_stride)) / static_cast<UINT64>(stride));
    UINT effective_max_count = MaxCommandCount;
    bool bounds_clamped = false;
    if (static_cast<UINT64>(effective_max_count) > max_fit64) {
        effective_max_count = static_cast<UINT>(std::min<UINT64>(max_fit64, 0xFFFFFFFFull));
        bounds_clamped = true;
    }

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

    // Translate guest GPU VAs inside indirect argument records (CBV/SRV/UAV/VB/IB addresses)
    // when TitleVA/XMem/custom VA translation is active and the argument buffer is CPU-readable.
    ID3D12Resource* exec_arg_buf = host_arg;
    UINT64 exec_arg_offset = ArgumentBufferOffset;
    bool va_translated = false;
    if (device_x_ && sig_proxy && sig_proxy->HasGpuVaArgs() &&
        device_x_->HasActiveGuestVaTranslation() && effective_max_count > 0u) {
        D3D12_HEAP_PROPERTIES arg_hp{};
        if (SUCCEEDED(host_arg->GetHeapProperties(&arg_hp, nullptr)) &&
            arg_hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
            const UINT64 span_bytes =
                static_cast<UINT64>(effective_max_count - 1u) * stride + min_stride;
            void* src_map = nullptr;
            const D3D12_RANGE rr{
                static_cast<SIZE_T>(ArgumentBufferOffset),
                static_cast<SIZE_T>(ArgumentBufferOffset + span_bytes)};
            if (SUCCEEDED(host_arg->Map(0u, &rr, &src_map)) && src_map) {
                std::vector<uint8_t> patched(static_cast<size_t>(span_bytes));
                std::memcpy(patched.data(),
                            static_cast<const uint8_t*>(src_map) + ArgumentBufferOffset,
                            static_cast<size_t>(span_bytes));
                const D3D12_RANGE wn{0u, 0u};
                host_arg->Unmap(0u, &wn);

                const auto& descs = sig_proxy->ArgumentDescs();
                const auto& offs = sig_proxy->ArgumentOffsets();
                bool any_patched = false;
                for (UINT c = 0u; c < effective_max_count; ++c) {
                    const size_t rec_base = static_cast<size_t>(c) * stride;
                    for (size_t a = 0u; a < descs.size() && a < offs.size(); ++a) {
                        const auto t = descs[a].Type;
                        if (t == D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW ||
                            t == D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW ||
                            t == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW ||
                            t == D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW ||
                            t == D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW) {
                            const size_t va_off = rec_base + offs[a];
                            if (va_off + 8u <= patched.size()) {
                                uint64_t gva = 0ull;
                                std::memcpy(&gva, patched.data() + va_off, 8u);
                                if (gva != 0ull) {
                                    const uint64_t h_va = device_x_->TranslateGuestGpuVa(gva);
                                    if (h_va != gva && h_va != 0ull) {
                                        std::memcpy(patched.data() + va_off, &h_va, 8u);
                                        any_patched = true;
                                    }
                                }
                            }
                        }
                    }
                }
                if (any_patched && device_x_->HostDevice()) {
                    D3D12_HEAP_PROPERTIES up_hp{};
                    up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                    D3D12_RESOURCE_DESC up_rd{};
                    up_rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    up_rd.Width = (span_bytes + 255ull) & ~255ull;
                    up_rd.Height = 1u;
                    up_rd.DepthOrArraySize = 1u;
                    up_rd.MipLevels = 1u;
                    up_rd.SampleDesc.Count = 1u;
                    up_rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    ID3D12Resource* shadow_arg = nullptr;
                    if (SUCCEEDED(device_x_->HostDevice()->CreateCommittedResource(
                            &up_hp, D3D12_HEAP_FLAG_NONE, &up_rd,
                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&shadow_arg))) &&
                        shadow_arg) {
                        void* dst_map = nullptr;
                        const D3D12_RANGE er{0u, 0u};
                        if (SUCCEEDED(shadow_arg->Map(0u, &er, &dst_map)) && dst_map) {
                            std::memcpy(dst_map, patched.data(), patched.size());
                            shadow_arg->Unmap(0u, nullptr);
                            exec_arg_buf = shadow_arg;
                            exec_arg_offset = 0ull;
                            va_translated = true;
                            device_x_->TrackCommandListTransientResource(shadow_arg);
                        } else {
                            shadow_arg->Release();
                        }
                    }
                }
            }
        }
    }

    bool vk_routed = false;
    if (device_x_ && device_x_->IsVulkanRoutingActive()) {
        vk_routed = true;
        std::lock_guard<std::mutex> lock(vk_rec_mutex_);
        VulkanRecordedOp op{};
        op.kind = VulkanRecordedOpKind::ExecuteIndirect;
        op.pso = vk_cur_pso_;
        op.is_spirv_direct = vk_cur_pso_is_spirv_;
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
        op.root_constants = (sig_proxy && sig_proxy->PrimaryActionType() == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)
                                ? vk_cmp_root_constants_
                                : vk_gfx_root_constants_;
        op.root_srvs = vk_cmp_srvs_;
        op.root_uavs = vk_cmp_uavs_;
        op.indirect_arg_resource = exec_arg_buf;
        op.indirect_count_resource = host_cnt;
        op.indirect_arg_offset = exec_arg_offset;
        op.indirect_count_offset = CountBufferOffset;
        op.indirect_max_count = effective_max_count;
        op.indirect_stride = stride;
        op.indirect_action_type = sig_proxy ? static_cast<uint32_t>(sig_proxy->PrimaryActionType()) : 0u;
        op.indirect_action_offset = sig_proxy ? sig_proxy->PrimaryActionOffset() : 0u;
        op.indirect_const_arg_offset = sig_proxy ? sig_proxy->FirstConstantArgOffset() : 0u;
        op.indirect_const_bytes = sig_proxy ? sig_proxy->FirstConstantBytes() : 0u;
        op.indirect_const_dst_offset = sig_proxy ? sig_proxy->FirstConstantDestOffsetBytes() : 0u;
        vk_batch_.ops.push_back(std::move(op));
        if (!vk_cur_pso_is_spirv_) {
            vk_batch_.has_host_commands = true;
        }
    }

    if (device_x_) {
        device_x_->RecordExecuteIndirectStats(
            has_count, has_root_consts, is_dispatch_mesh,
            bounds_clamped, /*rejected=*/false, va_translated, vk_routed);
    }

    // If this is a virtual DispatchMesh fallback on WARP / MeshTier=0 (host_sig == nullptr),
    // skip the native host ExecuteIndirect call safely after validation & Vulkan routing.
    if (host_sig != nullptr && !(vk_routed && vk_cur_pso_is_spirv_)) {
        if (sig_proxy && sig_proxy->PrimaryActionType() != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH &&
            last_pso_proxy_ != nullptr) {
            ID3D12PipelineState* target_pso = last_pso_proxy_->GetHostPsoForTargets(
                last_bound_rtv_count_, last_bound_rtv_formats_, last_bound_dsv_format_, false);
            if (target_pso && target_pso != last_host_pso_) {
                host_list_->SetPipelineState(target_pso);
                last_host_pso_ = target_pso;
            }
            uint8_t s57_ref = 0u;
            if (!guest_set_stencil_ref_ && last_pso_proxy_->HasSlot57StencilRef(s57_ref)) {
                host_list_->OMSetStencilRef(s57_ref);
            }
        }
        UINT host_max_count = effective_max_count;
        ID3D12Resource* exec_cnt_buf = host_cnt;
        UINT64 exec_cnt_offset = CountBufferOffset;
        bool handled_via_warp_replay = false;
        // Windows 10 inbox WARP (pre-Agility 1.613) has known driver defects in
        // ExecuteIndirect when combining root-mutating arguments (CONSTANT/CBV/SRV/UAV)
        // with pCountBuffer or unprimed compute PSOs. When running on WARP with a
        // CPU-readable indirect argument buffer for compute DISPATCH, replay each
        // validated record directly onto host_list_ for 100% bit-exact parity.
        if (device_x_ && device_x_->AdapterInfo().is_warp &&
            sig_proxy && (sig_proxy->HasRootConstants() || sig_proxy->HasGpuVaArgs()) &&
            sig_proxy->PrimaryActionType() == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
            if (exec_cnt_buf != nullptr) {
                D3D12_HEAP_PROPERTIES cnt_hp{};
                if (SUCCEEDED(exec_cnt_buf->GetHeapProperties(&cnt_hp, nullptr)) &&
                    (cnt_hp.Type == D3D12_HEAP_TYPE_UPLOAD || cnt_hp.Type == D3D12_HEAP_TYPE_READBACK)) {
                    void* cnt_map = nullptr;
                    const D3D12_RANGE cr{
                        static_cast<SIZE_T>(CountBufferOffset),
                        static_cast<SIZE_T>(CountBufferOffset + 4ull)};
                    if (SUCCEEDED(exec_cnt_buf->Map(0u, &cr, &cnt_map)) && cnt_map) {
                        uint32_t resolved_cnt = 0u;
                        std::memcpy(&resolved_cnt,
                                    static_cast<const uint8_t*>(cnt_map) + CountBufferOffset, 4u);
                        const D3D12_RANGE wn{0u, 0u};
                        exec_cnt_buf->Unmap(0u, &wn);
                        host_max_count = std::min<UINT>(host_max_count, resolved_cnt);
                        exec_cnt_buf = nullptr;
                        exec_cnt_offset = 0ull;
                    }
                }
            }
            if (exec_cnt_buf == nullptr && host_max_count > 0u && exec_arg_buf != nullptr) {
                D3D12_HEAP_PROPERTIES ahp{};
                if (SUCCEEDED(exec_arg_buf->GetHeapProperties(&ahp, nullptr)) &&
                    (ahp.Type == D3D12_HEAP_TYPE_UPLOAD || ahp.Type == D3D12_HEAP_TYPE_READBACK)) {
                    const UINT64 replay_bytes =
                        static_cast<UINT64>(host_max_count - 1u) * stride + min_stride;
                    void* arg_map = nullptr;
                    const D3D12_RANGE ar{
                        static_cast<SIZE_T>(exec_arg_offset),
                        static_cast<SIZE_T>(exec_arg_offset + replay_bytes)};
                    if (SUCCEEDED(exec_arg_buf->Map(0u, &ar, &arg_map)) && arg_map) {
                        const auto* base = static_cast<const uint8_t*>(arg_map) + exec_arg_offset;
                        const auto& descs = sig_proxy->ArgumentDescs();
                        const auto& offs = sig_proxy->ArgumentOffsets();
                        for (UINT c = 0u; c < host_max_count; ++c) {
                            const uint8_t* rec = base + static_cast<size_t>(c) * stride;
                            for (size_t a = 0u; a < descs.size() && a < offs.size(); ++a) {
                                const auto& ad = descs[a];
                                const uint8_t* ptr = rec + offs[a];
                                switch (ad.Type) {
                                    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
                                        host_list_->SetComputeRoot32BitConstants(
                                            ad.Constant.RootParameterIndex,
                                            ad.Constant.Num32BitValuesToSet,
                                            ptr,
                                            ad.Constant.DestOffsetIn32BitValues);
                                        break;
                                    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
                                        uint64_t va = 0ull;
                                        std::memcpy(&va, ptr, 8u);
                                        host_list_->SetComputeRootConstantBufferView(
                                            ad.ConstantBufferView.RootParameterIndex, va);
                                        break;
                                    }
                                    case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
                                        uint64_t va = 0ull;
                                        std::memcpy(&va, ptr, 8u);
                                        host_list_->SetComputeRootShaderResourceView(
                                            ad.ShaderResourceView.RootParameterIndex, va);
                                        break;
                                    }
                                    case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
                                        uint64_t va = 0ull;
                                        std::memcpy(&va, ptr, 8u);
                                        host_list_->SetComputeRootUnorderedAccessView(
                                            ad.UnorderedAccessView.RootParameterIndex, va);
                                        break;
                                    }
                                    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
                                        uint32_t g[3]{};
                                        std::memcpy(g, ptr, 12u);
                                        if (g[0] > 0u && g[1] > 0u && g[2] > 0u) {
                                            host_list_->Dispatch(g[0], g[1], g[2]);
                                        }
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            }
                        }
                        const D3D12_RANGE wn{0u, 0u};
                        exec_arg_buf->Unmap(0u, &wn);
                        handled_via_warp_replay = true;
                    }
                }
            } else if (exec_cnt_buf == nullptr && host_max_count == 0u) {
                handled_via_warp_replay = true;
            }
        }
        const bool pso_missing =
            device_x_ && device_x_->IsRetailScarlettAbi() && last_host_pso_ == nullptr;
        if (IsDiagTraceEnabled()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "ExecuteIndirect(max=%u, standin=%d, no_pso=%d)",
                          host_max_count, last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
            AppendDiagOp(buf);
        }
        if (!handled_via_warp_replay && host_max_count > 0u && !last_pso_is_standin_ && !pso_missing) {
            // On PC NVIDIA D3D12 drivers, a UAV barrier preceding ExecuteIndirect in the same
            // command list does not stall CP/PFP indirect-argument fetch. Transitioning a
            // 256-byte DEFAULT-heap sync buffer COMMON -> INDIRECT_ARGUMENT -> COMMON around
            // ExecuteIndirect forces the GPU front-end to wait for prior compute UAV writes.
            const bool need_ei_fe_sync =
                device_x_ && !device_x_->AdapterInfo().is_warp &&
                (list_type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
                 list_type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE);
            if (need_ei_fe_sync && !ei_sync_dummy_buf_ && device_x_->HostDevice()) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                hp.CreationNodeMask = 1u;
                hp.VisibleNodeMask = 1u;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = 256ull;
                rd.Height = 1u;
                rd.DepthOrArraySize = 1u;
                rd.MipLevels = 1u;
                rd.Format = DXGI_FORMAT_UNKNOWN;
                rd.SampleDesc.Count = 1u;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                (void)device_x_->HostDevice()->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, OS_D3D12_IID(ID3D12Resource),
                    reinterpret_cast<void**>(&ei_sync_dummy_buf_));
            }
            if (need_ei_fe_sync && ei_sync_dummy_buf_) {
                D3D12_RESOURCE_BARRIER pre_b{};
                pre_b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre_b.Transition.pResource = ei_sync_dummy_buf_;
                pre_b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                pre_b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                pre_b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                host_list_->ResourceBarrier(1u, &pre_b);
            }
            host_list_->ExecuteIndirect(
                host_sig, host_max_count,
                exec_arg_buf, exec_arg_offset, exec_cnt_buf, exec_cnt_offset);
            if (need_ei_fe_sync && ei_sync_dummy_buf_) {
                D3D12_RESOURCE_BARRIER post_b{};
                post_b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                post_b.Transition.pResource = ei_sync_dummy_buf_;
                post_b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                post_b.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
                post_b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                host_list_->ResourceBarrier(1u, &post_b);
            }
        }
    }
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
        RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT", 60, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT", 60);
    }
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
        RecordList("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT64", 61, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::AtomicCopyBufferUINT64", 61);
    }
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

    if (!host_dst || !host_src) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion[NullResNoop]", 64, S_OK);
        return;
    }
    if (host_list1_) {
        host_list1_->ResolveSubresourceRegion(
            host_dst, DstSubresource, DstX, DstY,
            host_src, SrcSubresource, pSrcRect, Format, ResolveMode);
        RecordList("ID3D12GraphicsCommandListX::ResolveSubresourceRegion", 64, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::ResolveSubresourceRegion", 64);
    }
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
        if (device_x_) device_x_->RecordZeroCountNoop();
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

    if (IsDiagTraceEnabled()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "WriteBufferImmediate(n=%u dst0=0x%llX->0x%llX val0=0x%X)",
                      Count,
                      static_cast<unsigned long long>(pParams[0].Dest),
                      static_cast<unsigned long long>(translated[0].Dest),
                      pParams[0].Value);
        AppendDiagOp(buf);
    }

    bool all_host_vas_valid = true;
    if (device_x_ && device_x_->IsRetailScarlettAbi()) {
        for (UINT i = 0u; i < Count; ++i) {
            if (translated[i].Dest == pParams[i].Dest) {
                all_host_vas_valid = false;
                break;
            }
        }
    }

    if (host_list2_ && all_host_vas_valid) {
        if (device_x_) {
            for (UINT i = 0u; i < Count; ++i) {
                uint64_t off = 0ull;
                if (ID3D12Resource* wb_res = device_x_->ResolveBufferResourceByGpuVa(pParams[i].Dest, &off)) {
                    device_x_->NotifyGpuWrittenBufferRange(wb_res, off, 4ull);
                }
            }
        }
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
    if (host_list3_) {
        host_list3_->SetProtectedResourceSession(pProtectedResourceSession);
        RecordList("ID3D12GraphicsCommandListX::SetProtectedResourceSession", 67, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::SetProtectedResourceSession", 67);
    }
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
        RecordList("ID3D12GraphicsCommandListX::BeginRenderPass", 68, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::BeginRenderPass", 68);
    }
}

void STDMETHODCALLTYPE CommandListX::EndRenderPass() {
    if (host_list4_) {
        host_list4_->EndRenderPass();
        RecordList("ID3D12GraphicsCommandListX::EndRenderPass", 69, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::EndRenderPass", 69);
    }
}

void STDMETHODCALLTYPE CommandListX::InitializeMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
    SIZE_T InitializationParametersDataSizeInBytes) {
    if (host_list4_ && pMetaCommand) {
        host_list4_->InitializeMetaCommand(
            pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
        RecordList("ID3D12GraphicsCommandListX::InitializeMetaCommand", 70, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::InitializeMetaCommand", 70);
    }
}

void STDMETHODCALLTYPE CommandListX::ExecuteMetaCommand(
    ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
    SIZE_T ExecutionParametersDataSizeInBytes) {
    if (host_list4_ && pMetaCommand) {
        host_list4_->ExecuteMetaCommand(
            pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
        RecordList("ID3D12GraphicsCommandListX::ExecuteMetaCommand", 71, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::ExecuteMetaCommand", 71);
    }
}

void STDMETHODCALLTYPE CommandListX::BuildRaytracingAccelerationStructure(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
    UINT NumPostbuildInfoDescs,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) {
    const bool rt_supported = !device_x_ || device_x_->AdapterInfo().raytracing_tier >= 10u;
    if (host_list4_ && rt_supported && pDesc) {
        host_list4_->BuildRaytracingAccelerationStructure(
            pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
        RecordList("ID3D12GraphicsCommandListX::BuildRaytracingAccelerationStructure", 72, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::BuildRaytracingAccelerationStructure", 72);
    }
}

void STDMETHODCALLTYPE CommandListX::EmitRaytracingAccelerationStructurePostbuildInfo(
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
    UINT NumSourceAccelerationStructures,
    const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) {
    const bool rt_supported = !device_x_ || device_x_->AdapterInfo().raytracing_tier >= 10u;
    if (host_list4_ && rt_supported && pDesc && NumSourceAccelerationStructures > 0u) {
        host_list4_->EmitRaytracingAccelerationStructurePostbuildInfo(
            pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
        RecordList("ID3D12GraphicsCommandListX::EmitRaytracingAccelerationStructurePostbuildInfo", 73, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::EmitRaytracingAccelerationStructurePostbuildInfo", 73);
    }
}

void STDMETHODCALLTYPE CommandListX::CopyRaytracingAccelerationStructure(
    D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
    D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) {
    const bool rt_supported = !device_x_ || device_x_->AdapterInfo().raytracing_tier >= 10u;
    if (host_list4_ && rt_supported && DestAccelerationStructureData != 0ull && SourceAccelerationStructureData != 0ull) {
        host_list4_->CopyRaytracingAccelerationStructure(
            DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
        RecordList("ID3D12GraphicsCommandListX::CopyRaytracingAccelerationStructure", 74, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::CopyRaytracingAccelerationStructure", 74);
    }
}

void STDMETHODCALLTYPE CommandListX::SetPipelineState1(ID3D12StateObject* pStateObject) {
    ID3D12StateObject* host_so = UnwrapStateObject(pStateObject);
    const bool is_sw_proxy = (pStateObject != nullptr && host_so == nullptr);
    const bool rt_supported = !device_x_ || device_x_->AdapterInfo().raytracing_tier >= 10u;
    if (host_list4_ && !is_sw_proxy && rt_supported && host_so) {
        host_list4_->SetPipelineState1(host_so);
        RecordList("ID3D12GraphicsCommandListX::SetPipelineState1", 75, S_OK);
    } else if (is_sw_proxy) {
        RecordList("ID3D12GraphicsCommandListX::SetPipelineState1", 75, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::SetPipelineState1", 75);
    }
}

void STDMETHODCALLTYPE CommandListX::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) {
    const bool rt_supported = !device_x_ || device_x_->AdapterInfo().raytracing_tier >= 10u;
    if (host_list4_ && rt_supported && pDesc) {
        host_list4_->DispatchRays(pDesc);
        RecordList("ID3D12GraphicsCommandListX::DispatchRays", 76, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::DispatchRays", 76);
    }
}

// ID3D12GraphicsCommandList5 (slots 77..78)
void STDMETHODCALLTYPE CommandListX::RSSetShadingRate(
    D3D12_SHADING_RATE baseShadingRate,
    const D3D12_SHADING_RATE_COMBINER* combiners) {
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "RSSetShadingRate(rate=0x%X comb=%p)",
                      static_cast<unsigned>(baseShadingRate), static_cast<const void*>(combiners));
        AppendDiagOp(buf);
    }
    auto is_valid_rate = [](D3D12_SHADING_RATE r) noexcept -> bool {
        switch (r) {
            case D3D12_SHADING_RATE_1X1:
            case D3D12_SHADING_RATE_1X2:
            case D3D12_SHADING_RATE_2X1:
            case D3D12_SHADING_RATE_2X2:
            case D3D12_SHADING_RATE_2X4:
            case D3D12_SHADING_RATE_4X2:
            case D3D12_SHADING_RATE_4X4:
                return true;
            default:
                return false;
        }
    };
    bool vrs_ok = (host_list5_ != nullptr) && is_valid_rate(baseShadingRate);
    if (vrs_ok && combiners) {
        if (static_cast<uint32_t>(combiners[0]) > static_cast<uint32_t>(D3D12_SHADING_RATE_COMBINER_MAX) ||
            static_cast<uint32_t>(combiners[1]) > static_cast<uint32_t>(D3D12_SHADING_RATE_COMBINER_MAX)) {
            vrs_ok = false;
        }
    }
    if (vrs_ok && device_x_ && device_x_->HostDevice()) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        if (FAILED(device_x_->HostDevice()->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) ||
            o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED) {
            vrs_ok = false;
        } else if (o6.AdditionalShadingRatesSupported == FALSE &&
                   (baseShadingRate == D3D12_SHADING_RATE_2X4 ||
                    baseShadingRate == D3D12_SHADING_RATE_4X2 ||
                    baseShadingRate == D3D12_SHADING_RATE_4X4)) {
            vrs_ok = false;
        }
    }
    if (vrs_ok) {
        host_list5_->RSSetShadingRate(baseShadingRate, combiners);
        RecordList("ID3D12GraphicsCommandListX::RSSetShadingRate", 77, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::RSSetShadingRate", 77);
    }
}

void STDMETHODCALLTYPE CommandListX::RSSetShadingRateImage(ID3D12Resource* shadingRateImage) {
    if (host_list5_) {
        host_list5_->RSSetShadingRateImage(UnwrapResource(shadingRateImage));
        RecordList("ID3D12GraphicsCommandListX::RSSetShadingRateImage", 78, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::RSSetShadingRateImage", 78);
    }
}

// ID3D12GraphicsCommandList6 (slot 79)
void STDMETHODCALLTYPE CommandListX::DispatchMesh(
    UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
    if (device_x_) {
        device_x_->RecordCmdListDispatchMesh(
            ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    }
    if (ThreadGroupCountX == 0u || ThreadGroupCountY == 0u || ThreadGroupCountZ == 0u) {
        if (device_x_) device_x_->RecordZeroCountNoop();
        RecordList("ID3D12GraphicsCommandListX::DispatchMesh[ZeroNoop]", 79, S_OK);
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
    const bool pso_missing =
        device_x_ && device_x_->IsRetailScarlettAbi() && last_host_pso_ == nullptr;
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "DispatchMesh(%u,%u,%u standin=%d, no_pso=%d)",
                      ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ,
                      last_pso_is_standin_ ? 1 : 0, pso_missing ? 1 : 0);
        AppendDiagOp(buf);
    }
    if (!skip_host && !last_pso_is_standin_ && !pso_missing && host_list6_ && host_mesh_supported) {
        if (last_pso_proxy_ != nullptr) {
            ID3D12PipelineState* target_pso = last_pso_proxy_->GetHostPsoForTargets(
                last_bound_rtv_count_, last_bound_rtv_formats_, last_bound_dsv_format_, false);
            if (target_pso && target_pso != last_host_pso_) {
                host_list_->SetPipelineState(target_pso);
                last_host_pso_ = target_pso;
            }
            uint8_t s57_ref = 0u;
            if (!guest_set_stencil_ref_ && last_pso_proxy_->HasSlot57StencilRef(s57_ref)) {
                host_list_->OMSetStencilRef(s57_ref);
            }
        }
        // Respect D3D12 per-axis limit (65535) while dispatching all X, Y, Z dimensions cleanly.
        constexpr UINT kMaxDim = 65535u;
        const UINT gx = std::min<UINT>(ThreadGroupCountX, kMaxDim);
        const UINT gy = std::min<UINT>(ThreadGroupCountY, kMaxDim);
        const UINT gz = std::min<UINT>(ThreadGroupCountZ, kMaxDim);
        host_list6_->DispatchMesh(gx, gy, gz);
        RecordList("ID3D12GraphicsCommandListX::DispatchMesh", 79, S_OK);
    } else if (skip_host || last_pso_is_standin_ || pso_missing) {
        RecordList("ID3D12GraphicsCommandListX::DispatchMesh", 79, S_OK);
    } else {
        RecordUnsupportedExtensionIgnored("ID3D12GraphicsCommandListX::DispatchMesh", 79);
    }
}

void CommandListX::RecordUnsupportedExtensionIgnored(const char* ext_name, int32_t slot) {
    // Leonida #3958: Log telemetry cleanly for unsupported/unrecognized console command-list
    // extensions WITHOUT poisoning or aborting subsequent supported Draw/Dispatch/Copy/Barrier commands.
    if (device_x_) {
        device_x_->RecordUnsupportedCmdListExtensionIgnored();
    }
    openscarlett::CallTelemetry::Global().Record(
        "d3d12_xs.dll",
        (ext_name && ext_name[0] != '\0') ? ext_name : "ID3D12GraphicsCommandListX::UnsupportedExtension",
        openscarlett::CallOutcome::NotImplemented,
        E_NOTIMPL,
        slot);
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
            Address,
            static_cast<uint64_t>(Value),
            static_cast<uint64_t>(Mask),
            Operation,
            false,
            static_cast<uint32_t>(deferred_cpu_writes_.size()),
            0u});
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
        deferred_cpu_waits_.push_back(DeferredCpuWait{
            Address,
            Value,
            Mask,
            Operation,
            true,
            static_cast<uint32_t>(deferred_cpu_writes_.size()),
            0u});
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
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "Write32BitValueX(addr=0x%llX val=0x%X flg=0x%X)",
                      static_cast<unsigned long long>(Address), Value, Flags);
        AppendDiagOp(buf);
    }
    if (Address == 0ull) {
        RecordList("ID3D12GraphicsCommandListX::Write32BitValueX", 86, E_INVALIDARG);
        return;
    }

    const bool is_retail = device_x_ && device_x_->IsRetailScarlettAbi();
    uint64_t buf_off = 0ull;
    ID3D12Resource* host_buf =
        (!is_retail && device_x_) ? device_x_->ResolveBufferResourceByGpuVa(Address, &buf_off) : nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS translated =
        (!is_retail && device_x_) ? device_x_->TranslateGuestGpuVa(Address) : Address;

    bool can_wbi = (!is_retail) && (host_buf != nullptr) && (host_list2_ != nullptr) &&
                   ((translated & 3ull) == 0ull);
    if (can_wbi) {
        D3D12_HEAP_PROPERTIES hp{};
        if (FAILED(host_buf->GetHeapProperties(&hp, nullptr)) ||
            hp.Type == D3D12_HEAP_TYPE_UPLOAD ||
            hp.Type == D3D12_HEAP_TYPE_READBACK) {
            can_wbi = false;
        }
    }

    if (can_wbi) {
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
        deferred_cpu_writes_.push_back(DeferredCpuWrite32{Address, static_cast<uint64_t>(Value), false, false});
    }
    RecordList("ID3D12GraphicsCommandListX::Write32BitValueX", 86, S_OK);
}

void CommandListX::Write64BitValueX(
    D3D12_GPU_VIRTUAL_ADDRESS Address,
    uint64_t Value,
    uint32_t Flags) {
    if (IsDiagTraceEnabled()) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "Write64BitValueX(addr=0x%llX val=0x%llX flg=0x%X)",
                      static_cast<unsigned long long>(Address),
                      static_cast<unsigned long long>(Value), Flags);
        AppendDiagOp(buf);
    }
    if (Address == 0ull) {
        RecordList("ID3D12GraphicsCommandListX::Write64BitValueX", 86, E_INVALIDARG);
        return;
    }
    const bool is_ts = (Value == 0ull) && ((Flags & 1u) != 0u);
    {
        std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
        deferred_cpu_writes_.push_back(DeferredCpuWrite32{Address, Value, true, is_ts});
    }
    RecordList("ID3D12GraphicsCommandListX::Write64BitValueX", 86, S_OK);
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
    if (IsDiagTraceEnabled()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "FlushGpuCacheRange(flg=0x%X addr=0x%llX)",
                      Flags, static_cast<unsigned long long>(BaseAddress));
        AppendDiagOp(buf);
    }
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

void CommandListX::RetireUploadArenaPages(bool immediate_release) noexcept {
    std::vector<UploadArenaPage> retired;
    {
        std::lock_guard<std::mutex> lock(upload_arena_mutex_);
        if (upload_arena_pages_.empty()) {
            return;
        }
        retired.swap(upload_arena_pages_);
    }
    for (auto& page : retired) {
        if (!page.buffer) continue;
        if (page.cpu_ptr) {
            D3D12_RANGE written{0u, static_cast<SIZE_T>(page.cursor)};
            page.buffer->Unmap(0u, &written);
            page.cpu_ptr = nullptr;
        }
        if (!immediate_release && device_x_) {
            device_x_->DeferHostResourceRelease(page.buffer);
            page.buffer->Release();
        } else {
            if (device_x_) {
                device_x_->NotifyResourceDestroyed(page.buffer);
            }
            page.buffer->Release();
        }
        page.buffer = nullptr;
    }
}

void* CommandListX::AllocateUploadX(uint64_t size_bytes, uint64_t alignment) {
    if (!device_x_ || !device_x_->HostDevice() ||
        is_closed_.load(std::memory_order_acquire) ||
        size_bytes == 0ull || size_bytes > (256ull * 1024ull * 1024ull)) {
        return nullptr;
    }
    if (alignment == 0ull) {
        alignment = 16ull;
    }
    if ((alignment & (alignment - 1ull)) != 0ull || alignment > 65536ull) {
        return nullptr;
    }
    const uint64_t eff_align = std::max<uint64_t>(alignment, 16ull);

    std::lock_guard<std::mutex> lock(upload_arena_mutex_);
    if (!upload_arena_pages_.empty()) {
        auto& cur = upload_arena_pages_.back();
        if (cur.cpu_ptr && cur.buffer) {
            const uint64_t base_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cur.cpu_ptr));
            const uint64_t cand_addr =
                (base_addr + cur.cursor + eff_align - 1ull) & ~(eff_align - 1ull);
            const uint64_t new_off = cand_addr - base_addr;
            if (new_off <= cur.capacity && size_bytes <= cur.capacity - new_off) {
                cur.cursor = new_off + size_bytes;
                return reinterpret_cast<void*>(static_cast<uintptr_t>(cand_addr));
            }
        }
    }

    constexpr uint64_t kMinUploadPageBytes = 65536ull;
    const uint64_t raw_need = size_bytes + eff_align;
    const uint64_t page_cap = std::max<uint64_t>(
        kMinUploadPageBytes, (raw_need + 65535ull) & ~65535ull);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0ull;
    desc.Width = page_cap;
    desc.Height = 1u;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* new_buf = nullptr;
    if (FAILED(device_x_->HostDevice()->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&new_buf))) ||
        !new_buf) {
        return nullptr;
    }

    void* mapped = nullptr;
    D3D12_RANGE zero_read{0u, 0u};
    if (FAILED(new_buf->Map(0u, &zero_read, &mapped)) || !mapped) {
        new_buf->Release();
        return nullptr;
    }

    device_x_->RegisterGuestMappedBufferVa(
        new_buf, desc, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mapped)));

    const uint64_t base_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mapped));
    const uint64_t cand_addr = (base_addr + eff_align - 1ull) & ~(eff_align - 1ull);
    const uint64_t new_off = cand_addr - base_addr;
    if (new_off > page_cap || size_bytes > page_cap - new_off) {
        D3D12_RANGE no_write{0u, 0u};
        new_buf->Unmap(0u, &no_write);
        device_x_->NotifyResourceDestroyed(new_buf);
        new_buf->Release();
        return nullptr;
    }

    UploadArenaPage page{};
    page.buffer = new_buf;
    page.cpu_ptr = static_cast<uint8_t*>(mapped);
    page.capacity = page_cap;
    page.cursor = new_off + size_bytes;
    upload_arena_pages_.push_back(page);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(cand_addr));
}

void CommandListX::ExtractDeferredCpuOps(
    std::vector<DeferredCpuWrite32>& out_writes,
    std::vector<DeferredCpuWait>& out_waits) const {
    std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
    const uint32_t base_write_idx = static_cast<uint32_t>(out_writes.size());
    out_writes.insert(out_writes.end(), deferred_cpu_writes_.begin(), deferred_cpu_writes_.end());
    out_waits.reserve(out_waits.size() + deferred_cpu_waits_.size());
    for (DeferredCpuWait w : deferred_cpu_waits_) {
        w.prior_write_count += base_write_idx;
        out_waits.push_back(w);
    }
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
    if (!device_x_ || !host_resource || !device_x_->HasMirroredTitleVaRegions()) return;
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

void CommandListX::ExtractDeferredStateTransitions(
    std::vector<DeferredStateTransition>& out_transitions) const {
    std::lock_guard<std::mutex> lock(aaa_rec_mutex_);
    out_transitions.insert(
        out_transitions.end(),
        deferred_state_transitions_.begin(),
        deferred_state_transitions_.end());
}

// ---------------------------------------------------------------------------
// Sprint 13 Placed Tiled Texture Management, Copy Bridging & Barrier Coherency
// (Leonida #1674)
// ---------------------------------------------------------------------------
bool CommandListX::AcquireAlignedStagingSlice(
    UINT64 required_bytes,
    ID3D12Resource*& out_buf,
    UINT64& out_offset,
    D3D12_RESOURCE_STATES& out_current_state) {
    out_buf = nullptr;
    out_offset = 0ull;
    out_current_state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!device_x_ || !device_x_->HostDevice() || required_bytes == 0ull) return false;

    auto& slot = aligned_staging_ring_[aligned_staging_slot_idx_];
    const UINT64 aligned_cursor = (slot.cursor + 511ull) & ~511ull;
    const UINT64 aligned_req = (required_bytes + 511ull) & ~511ull;

    if (slot.buffer && aligned_cursor + aligned_req <= slot.capacity) {
        out_buf = slot.buffer;
        out_offset = aligned_cursor;
        out_current_state = slot.state;
        slot.cursor = aligned_cursor + aligned_req;
        return true;
    }

    // Grow buffer (min 4 MiB, or next power-of-two / 64 KiB aligned above requested size).
    UINT64 new_cap = std::max<UINT64>(4ull * 1024ull * 1024ull, slot.capacity * 2ull);
    const UINT64 min_need = (aligned_req + 65535ull) & ~65535ull;
    if (new_cap < min_need) new_cap = min_need;

    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0ull;
    desc.Width = new_cap;
    desc.Height = 1u;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* new_buf = nullptr;
    if (FAILED(device_x_->HostDevice()->CreateCommittedResource(
            &def_hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(&new_buf))) ||
        !new_buf) {
        return false;
    }
    if (slot.buffer) {
        slot.retired_buffers.push_back(slot.buffer);
    }
    slot.buffer = new_buf;
    slot.capacity = new_cap;
    slot.cursor = aligned_req;
    slot.state = D3D12_RESOURCE_STATE_COMMON;
    out_buf = slot.buffer;
    out_offset = 0ull;
    out_current_state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

void CommandListX::TransitionAlignedStagingState(D3D12_RESOURCE_STATES new_state) {
    auto& slot = aligned_staging_ring_[aligned_staging_slot_idx_];
    if (!slot.buffer || !host_list_ || slot.state == new_state) return;
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = slot.buffer;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = slot.state;
    tb.Transition.StateAfter = new_state;
    host_list_->ResourceBarrier(1u, &tb);
    slot.state = new_state;
}

void CommandListX::ReleaseAlignedStagingRing() {
    for (uint32_t i = 0u; i < kAlignedStagingRingSlots; ++i) {
        auto& slot = aligned_staging_ring_[i];
        if (slot.buffer) {
            slot.buffer->Release();
            slot.buffer = nullptr;
        }
        for (ID3D12Resource* old_res : slot.retired_buffers) {
            if (old_res) old_res->Release();
        }
        slot.retired_buffers.clear();
        slot.capacity = 0ull;
        slot.cursor = 0ull;
        slot.state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
}

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

void CommandListX::TrackAlignTransientResource(ID3D12Resource* res) {
    if (!res) return;
    std::lock_guard<std::mutex> lock(tiled_transient_mutex_);
    align_transient_ring_[align_transient_slot_].push_back(res);
}

void CommandListX::RotateAlignTransientRing() {
    std::lock_guard<std::mutex> lock(tiled_transient_mutex_);
    align_transient_slot_ = (align_transient_slot_ + 1u) % 6u;
    for (ID3D12Resource* r : align_transient_ring_[align_transient_slot_]) {
        if (r) r->Release();
    }
    align_transient_ring_[align_transient_slot_].clear();
}

void CommandListX::ReleaseAllAlignTransientResources() {
    std::lock_guard<std::mutex> lock(tiled_transient_mutex_);
    for (auto& slot : align_transient_ring_) {
        for (ID3D12Resource* r : slot) {
            if (r) r->Release();
        }
        slot.clear();
    }
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
        reg.count.store(static_cast<uint32_t>(reg.map.size()), std::memory_order_release);
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
    reg.count.store(static_cast<uint32_t>(reg.map.size()), std::memory_order_release);
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
    if (GetPlacedTiledRegistry().count.load(std::memory_order_acquire) == 0u) return false;
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
        // Guard CPU reads from placed/tiled memory with SEH on MSVC and VirtualQuery on non-MSVC (xbox-emulator #153).
#if !defined(_MSC_VER)
        auto is_readable_range = [](const void* ptr, uint64_t len) noexcept -> bool {
            if (!ptr || len == 0ull) return false;
            const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
            if (base > UINTPTR_MAX - static_cast<uintptr_t>(len)) return false;
            uintptr_t cur = base;
            const uintptr_t end = base + static_cast<uintptr_t>(len);
            while (cur < end) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0u) {
                    return false;
                }
                if (mbi.State != MEM_COMMIT) return false;
                constexpr DWORD kReadMask =
                    PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if ((mbi.Protect & kReadMask) == 0u || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
                    return false;
                }
                const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (region_end <= cur) return false;
                cur = region_end;
            }
            return true;
        };
#endif

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

        const uint8_t* raw_tiled_ptr = static_cast<const uint8_t*>(src_mapped) + src_loc.PlacedFootprint.Offset;
        const uint64_t raw_tiled_avail = (src_buf_width > src_loc.PlacedFootprint.Offset)
                                             ? (src_buf_width - src_loc.PlacedFootprint.Offset)
                                             : 0ull;
        std::vector<uint8_t> safe_src_copy;
        bool safe_read_ok = false;
        if (raw_tiled_avail > 0ull) {
            safe_src_copy.resize(static_cast<size_t>(raw_tiled_avail));
#if defined(_MSC_VER)
            auto seh_copy = [](void* dst, const void* src, size_t n) noexcept -> bool {
                __try {
                    std::memcpy(dst, src, n);
                    return true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    return false;
                }
            };
            safe_read_ok = seh_copy(safe_src_copy.data(), raw_tiled_ptr, static_cast<size_t>(raw_tiled_avail));
#else
            if (is_readable_range(raw_tiled_ptr, raw_tiled_avail)) {
                std::memcpy(safe_src_copy.data(), raw_tiled_ptr, static_cast<size_t>(raw_tiled_avail));
                safe_read_ok = true;
            }
#endif
        }
        D3D12_RANGE wn{0u, 0u};
        src_loc.pResource->Unmap(0u, &wn);
        if (!safe_read_ok) {
            return false;
        }

        std::vector<uint8_t> gathered_cpu;
        const uint8_t* tiled_ptr = safe_src_copy.data();
        uint64_t tiled_avail = static_cast<uint64_t>(safe_src_copy.size());
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
    if (GetPlacedTiledRegistry().count.load(std::memory_order_acquire) == 0u) return false;

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
    if (device_x_) {
        device_x_->NotifyGpuWrittenTexture(host_dst);
        device_x_->NotifyGpuWrittenBuffer(host_dst);
    }
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
    if (GetPlacedTiledRegistry().count.load(std::memory_order_acquire) == 0u) return false;
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
    if (reg.count.load(std::memory_order_acquire) == 0u) return;
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
