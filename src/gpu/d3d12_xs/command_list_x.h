#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "proxies_x.h"

namespace openscarlett::d3d12_xs {

// Clean-room COM interface for placed tiled texture management, 64KB tile-pool mapping,
// subresource pitch queries, and untile/retile barrier coherency diagnostics (Leonida #1674).
// Returned via CommandListX::QueryInterface (zero extra DLL exports needed on MinGW-w64).
// {6A94F8C3-7D42-4A1E-9C35-B8E10F4A2D13}
inline constexpr GUID IID_IOpenScarlettTiledPlacedManager = {
    0x6a94f8c3u, 0x7d42u, 0x4a1eu,
    {0x9cu, 0x35u, 0xb8u, 0xe1u, 0x0fu, 0x4au, 0x2du, 0x13u}
};

struct D3D12XBOX_TILED_PLACED_DESC {
    ID3D12Heap* placed_heap = nullptr;
    uint64_t heap_offset_bytes = 0ull;
    uint32_t xg_tile_mode = static_cast<uint32_t>(XG_TILE_MODE_2D_THIN_64KB_S);
    uint32_t initial_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON);
    uint64_t tile_pool_base_offset_bytes = 0ull;  // 0 => default to heap_offset_bytes
};

struct D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH {
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
    uint32_t current_state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON);
    uint32_t dirty_for_flush = 0u;
};

struct D3D12XBOX_TILED_PLACED_TILE_MAPPING {
    uint32_t subresource_index = 0u;
    uint32_t logical_tile_x = 0u;
    uint32_t logical_tile_y = 0u;
    uint32_t logical_tile_z = 0u;
    uint32_t heap_tile_index_64kb = 0u;
    uint64_t pool_byte_offset = 0ull;
    uint32_t mapped = 1u;
};

struct D3D12XBOX_TILED_PLACED_INFO {
    uint64_t heap_offset_bytes = 0ull;
    uint64_t tile_pool_base_offset_bytes = 0ull;
    uint64_t total_surface_bytes = 0ull;
    uint32_t xg_tile_mode = 0u;
    uint32_t subresource_count = 0u;
    uint32_t total_64kb_tiles = 0u;
    uint32_t mapped_tile_count = 0u;
    uint32_t current_state = 0u;
    uint64_t untile_copy_count = 0ull;
    uint64_t retile_copy_count = 0ull;
    uint64_t copy_resource_bridge_count = 0ull;
    uint64_t copy_tiles_bridge_count = 0ull;
    uint64_t barrier_transition_count = 0ull;
    uint64_t aliasing_barrier_count = 0ull;
    uint64_t coherency_flush_count = 0ull;
    uint32_t used_gpu_untile_bridge = 0u;
};

struct IOpenScarlettTiledPlacedManager : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterPlacedTiledTexture(
        ID3D12Resource* resource,
        const D3D12XBOX_TILED_PLACED_DESC* desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterPlacedTiledTexture(
        ID3D12Resource* resource) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdatePlacedTileMappings(
        ID3D12Resource* resource,
        uint32_t mapping_count,
        const D3D12XBOX_TILED_PLACED_TILE_MAPPING* mappings) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryPlacedTiledSubresourcePitch(
        ID3D12Resource* resource,
        uint32_t subresource_index,
        D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH* out_pitch) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryPlacedTiledTextureInfo(
        ID3D12Resource* resource,
        D3D12XBOX_TILED_PLACED_INFO* out_info) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryPlacedTileMapping(
        ID3D12Resource* resource,
        uint32_t subresource_index,
        uint32_t tile_x,
        uint32_t tile_y,
        uint32_t tile_z,
        D3D12XBOX_TILED_PLACED_TILE_MAPPING* out_mapping) = 0;
};

class DeviceX;
class CommandListExtendedProxy;

enum class VulkanRecordedOpKind : uint8_t {
    Barrier = 0,
    ClearRtv = 1,
    ClearDsv = 2,
    CopyBuffer = 3,
    CopyTexture = 4,
    CopyResource = 5,
    DrawInstanced = 6,
    DrawIndexedInstanced = 7,
    Dispatch = 8,
    DispatchMesh = 9,
    ExecuteIndirect = 10,
};

struct VulkanRecordedOp {
    VulkanRecordedOpKind kind = VulkanRecordedOpKind::Barrier;
    ID3D12PipelineState* pso = nullptr;
    bool is_spirv_direct = false;

    // Draw / Dispatch / Mesh params
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t z = 0u;
    uint32_t start_location = 0u;
    int32_t base_vertex = 0;

    // Clear / RT / Viewport state snapshot
    ID3D12Resource* rtv_resource = nullptr;
    ID3D12Resource* dsv_resource = nullptr;
    bool has_clear_color = false;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;
    uint32_t viewport_width = 0u;
    uint32_t viewport_height = 0u;

    // Geometry & root bindings snapshot
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    bool has_vbv = false;
    D3D12_INDEX_BUFFER_VIEW ibv{};
    bool has_ibv = false;
    std::vector<uint32_t> root_constants;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> root_cbvs;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> root_srvs;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> root_uavs;

    // Copy params
    ID3D12Resource* dst_resource = nullptr;
    ID3D12Resource* src_resource = nullptr;
    uint64_t dst_offset = 0ull;
    uint64_t src_offset = 0ull;
    uint64_t num_bytes = 0ull;
    uint32_t copy_dst_x = 0u;
    uint32_t copy_dst_y = 0u;
    uint32_t copy_dst_z = 0u;
    uint64_t copy_footprint_offset = 0ull;
    uint32_t barrier_count = 0u;
    uint32_t global_uav_barrier_count = 0u;

    // Sprint 16 ExecuteIndirect snapshot for Vulkan 1.4 routing
    ID3D12Resource* indirect_arg_resource = nullptr;
    ID3D12Resource* indirect_count_resource = nullptr;
    uint64_t indirect_arg_offset = 0ull;
    uint64_t indirect_count_offset = 0ull;
    uint32_t indirect_max_count = 0u;
    uint32_t indirect_stride = 0u;
    uint32_t indirect_action_type = 0u;  // D3D12_INDIRECT_ARGUMENT_TYPE
    uint32_t indirect_action_offset = 0u;
    uint32_t indirect_const_arg_offset = 0u;
    uint32_t indirect_const_bytes = 0u;
    uint32_t indirect_const_dst_offset = 0u;
};

struct VulkanRecordedBatch {
    std::vector<VulkanRecordedOp> ops;
    bool has_host_commands = false;
};

struct DeferredCpuWrite32 {
    uint64_t address = 0ull;
    uint64_t value = 0ull;
    bool is_64bit = false;
    bool is_timestamp = false;
};

struct DeferredCpuWait {
    uint64_t address = 0ull;
    uint64_t value = 0ull;
    uint64_t mask = 0ull;
    D3D12XBOX_WAIT_VALUE_OP op = D3D12XBOX_WAIT_VALUE_OP_ALWAYS;
    bool is_64bit = false;
};

// {4F534431-3258-4241-5200-000000000032}
inline constexpr GUID IID_IOpenScarlettEnhancedBarrierStats = {
    0x4F534431u, 0x3258u, 0x4241u,
    {0x52u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x32u}
};

struct D3D12XBOX_ENHANCED_BARRIER_STATS {
    uint64_t barrier_calls = 0ull;
    uint64_t native_barrier_calls = 0ull;
    uint64_t legacy_fallback_calls = 0ull;
    uint64_t buffer_barriers_processed = 0ull;
    uint64_t texture_barriers_processed = 0ull;
    uint64_t global_barriers_processed = 0ull;
    uint64_t proxy_resources_unwrapped = 0ull;
    uint64_t va_mirror_notifications = 0ull;
    uint64_t unified_texture_notifications = 0ull;
    uint32_t host_has_cmdlist7 = 0u;
};

struct IOpenScarlettEnhancedBarrierStats : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetEnhancedBarrierStats(
        D3D12XBOX_ENHANCED_BARRIER_STATS* out_stats) const noexcept = 0;
};

class CommandListX final
    : public ID3D12GraphicsCommandListX,
      public IOpenScarlettCommandListExtState,
      public IOpenScarlettTiledPlacedManager,
      public IOpenScarlettEnhancedBarrierStats {
public:
    CommandListX(DeviceX* parent_device, ID3D12GraphicsCommandList* host_list);
    ~CommandListX();

    CommandListX(const CommandListX&) = delete;
    CommandListX& operator=(const CommandListX&) = delete;

    ID3D12GraphicsCommandList* HostList() const noexcept { return host_list_; }

    // Unwraps a CommandListX* back to its underlying host ID3D12CommandList*,
    // or returns the pointer unchanged if it is already a native host command list.
    static ID3D12CommandList* UnwrapHostList(ID3D12CommandList* maybe_wrapped) noexcept;
    static ID3D12GraphicsCommandList* UnwrapHostGraphicsList(
        ID3D12GraphicsCommandList* maybe_wrapped) noexcept;
    static CommandListX* TryFromCommandList(ID3D12CommandList* maybe_wrapped) noexcept;
    static void RegisterExternalAlias(const void* alias_ptr, CommandListX* wrapper) noexcept;
    static void UnregisterExternalAlias(const void* alias_ptr) noexcept;
    void CopyVulkanRecordedBatch(VulkanRecordedBatch& out_batch) const;
    void ExtractPendingMirrorWritebacks(std::vector<uint64_t>& out_vas) const;
    struct DeferredStateTransition {
        ID3D12Resource* host_resource = nullptr;
        D3D12_RESOURCE_STATES after_state = D3D12_RESOURCE_STATE_COMMON;
    };
    void ExtractDeferredCpuOps(
        std::vector<DeferredCpuWrite32>& out_writes,
        std::vector<DeferredCpuWait>& out_waits) const;
    void ExtractDeferredStateTransitions(
        std::vector<DeferredStateTransition>& out_transitions) const;

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

    // ID3D12CommandList (slot 8)
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    // ID3D12GraphicsCommandList (slots 9..59)
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(
        ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE DrawInstanced(
        UINT VertexCountPerInstance, UINT InstanceCount,
        UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(
        UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
        INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE Dispatch(
        UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ,
        const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(
        ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(
        ID3D12Resource* pTiledResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pTileRegionSize,
        ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes,
        D3D12_TILE_COPY_FLAGS Flags) override;
    void STDMETHODCALLTYPE ResolveSubresource(
        ID3D12Resource* pDstResource, UINT DstSubresource,
        ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(
        D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(
        UINT NumViewports, const D3D12_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(
        UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE ResourceBarrier(
        UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(
        UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(
        ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(
        ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
        UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(
        UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
        UINT RootParameterIndex, UINT Num32BitValuesToSet,
        const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(
        UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(
        UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(
        UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(
        UINT NumRenderTargetDescriptors,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
        BOOL RTsSingleHandleToDescriptorRange,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(
        D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags,
        FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4],
        UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
        const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
        D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource,
        const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE DiscardResource(
        ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override;
    void STDMETHODCALLTYPE BeginQuery(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE EndQuery(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE ResolveQueryData(
        ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries,
        ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override;
    void STDMETHODCALLTYPE SetPredication(
        ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset,
        D3D12_PREDICATION_OP Operation) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(
        ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount,
        ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
        ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override;

    // ID3D12GraphicsCommandList1 (slots 60..65)
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
        ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
        ID3D12Resource* pDstBuffer, UINT64 DstOffset,
        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
        ID3D12Resource* const* ppDependentResources,
        const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT Min, FLOAT Max) override;
    void STDMETHODCALLTYPE SetSamplePositions(
        UINT NumSamplesPerPixel, UINT NumPixels,
        D3D12_SAMPLE_POSITION* pSamplePositions) override;
    void STDMETHODCALLTYPE ResolveSubresourceRegion(
        ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
        ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect,
        DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) override;
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT Mask) override;

    // ID3D12GraphicsCommandList2 (slot 66)
    void STDMETHODCALLTYPE WriteBufferImmediate(
        UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams,
        const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) override;

    // ID3D12GraphicsCommandList3 (slot 67)
    void STDMETHODCALLTYPE SetProtectedResourceSession(
        ID3D12ProtectedResourceSession* pProtectedResourceSession) override;

    // ID3D12GraphicsCommandList4 (slots 68..76)
    void STDMETHODCALLTYPE BeginRenderPass(
        UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
        D3D12_RENDER_PASS_FLAGS Flags) override;
    void STDMETHODCALLTYPE EndRenderPass() override;
    void STDMETHODCALLTYPE InitializeMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData,
        SIZE_T InitializationParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE ExecuteMetaCommand(
        ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData,
        SIZE_T ExecutionParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc,
        UINT NumPostbuildInfoDescs,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) override;
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc,
        UINT NumSourceAccelerationStructures,
        const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) override;
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
        D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData,
        D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) override;
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* pStateObject) override;
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) override;

    // ID3D12GraphicsCommandList5 (slots 77..78)
    void STDMETHODCALLTYPE RSSetShadingRate(
        D3D12_SHADING_RATE baseShadingRate,
        const D3D12_SHADING_RATE_COMBINER* combiners) override;
    void STDMETHODCALLTYPE RSSetShadingRateImage(
        ID3D12Resource* shadingRateImage) override;

    // ID3D12GraphicsCommandList6 (slot 79)
    void STDMETHODCALLTYPE DispatchMesh(
        UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;

    // ID3D12GraphicsCommandListX (slots 80..85)
    void STDMETHODCALLTYPE SetGpuVirtualAddressHintX(uint64_t Hint) override;
    ID3D12GraphicsCommandList* STDMETHODCALLTYPE GetUnderlyingHostCommandList() override;
    void STDMETHODCALLTYPE KickoffX() override;
    void STDMETHODCALLTYPE Wait32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    void STDMETHODCALLTYPE Wait64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint64_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    void STDMETHODCALLTYPE RSSetDepthBiasX(
        FLOAT DepthBias,
        FLOAT DepthBiasClamp,
        FLOAT SlopeScaledDepthBias) override;
    void STDMETHODCALLTYPE Write32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Flags) override;
    void Write64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint32_t Flags);
    void STDMETHODCALLTYPE SetPredicationX(
        D3D12_GPU_VIRTUAL_ADDRESS BufferAddress,
        D3D12_PREDICATION_OP Operation,
        uint32_t Flags) override;
    void STDMETHODCALLTYPE FlushGpuCacheRange(
        uint32_t Flags,
        D3D12_GPU_VIRTUAL_ADDRESS BaseAddress,
        SIZE_T SizeInBytes) override;

    // Inspection helper for tests/runtime diagnostics.
    void GetDepthBiasXState(
        float* out_depth_bias,
        float* out_depth_bias_clamp,
        float* out_slope_scaled_depth_bias) const noexcept;
    uint64_t KickoffCount() const noexcept { return kickoff_count_.load(std::memory_order_relaxed); }

    // IOpenScarlettCommandListExtState
    HRESULT STDMETHODCALLTYPE GetCommandListExtState(
        D3D12XBOX_CMDLIST_EXT_STATE* out_state) const noexcept override;

    // IOpenScarlettTiledPlacedManager
    HRESULT STDMETHODCALLTYPE RegisterPlacedTiledTexture(
        ID3D12Resource* resource,
        const D3D12XBOX_TILED_PLACED_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE UnregisterPlacedTiledTexture(
        ID3D12Resource* resource) override;
    HRESULT STDMETHODCALLTYPE UpdatePlacedTileMappings(
        ID3D12Resource* resource,
        uint32_t mapping_count,
        const D3D12XBOX_TILED_PLACED_TILE_MAPPING* mappings) override;
    HRESULT STDMETHODCALLTYPE QueryPlacedTiledSubresourcePitch(
        ID3D12Resource* resource,
        uint32_t subresource_index,
        D3D12XBOX_TILED_PLACED_SUBRESOURCE_PITCH* out_pitch) override;
    HRESULT STDMETHODCALLTYPE QueryPlacedTiledTextureInfo(
        ID3D12Resource* resource,
        D3D12XBOX_TILED_PLACED_INFO* out_info) override;
    HRESULT STDMETHODCALLTYPE QueryPlacedTileMapping(
        ID3D12Resource* resource,
        uint32_t subresource_index,
        uint32_t tile_x,
        uint32_t tile_y,
        uint32_t tile_z,
        D3D12XBOX_TILED_PLACED_TILE_MAPPING* out_mapping) override;

    // IOpenScarlettEnhancedBarrierStats
    HRESULT STDMETHODCALLTYPE GetEnhancedBarrierStats(
        D3D12XBOX_ENHANCED_BARRIER_STATS* out_stats) const noexcept override;

    // Sprint 15: Record an unsupported/unrecognized console command-list extension cleanly
    // without poisoning or aborting subsequent supported Draw/Dispatch/Copy/Barrier commands.
    void RecordUnsupportedExtensionIgnored(const char* ext_name, int32_t slot);

#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    void ExecuteEnhancedBarriers(
        UINT32 num_barrier_groups,
        const D3D12_BARRIER_GROUP* barrier_groups);
#endif

private:
    friend class CommandListExtendedProxy;

    std::atomic<ULONG> ref_count_{1};
    DeviceX* device_x_ = nullptr;
    ID3D12GraphicsCommandList* host_list_ = nullptr;
    ID3D12GraphicsCommandList1* host_list1_ = nullptr;
    ID3D12GraphicsCommandList2* host_list2_ = nullptr;
    ID3D12GraphicsCommandList3* host_list3_ = nullptr;
    ID3D12GraphicsCommandList4* host_list4_ = nullptr;
    ID3D12GraphicsCommandList5* host_list5_ = nullptr;
    ID3D12GraphicsCommandList6* host_list6_ = nullptr;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    ID3D12GraphicsCommandList7* host_list7_ = nullptr;
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    ID3D12GraphicsCommandList8* host_list8_ = nullptr;
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    ID3D12GraphicsCommandList9* host_list9_ = nullptr;
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    ID3D12GraphicsCommandList10* host_list10_ = nullptr;
#endif
    CommandListExtendedProxy* ext_proxy_ = nullptr;
    D3D12_COMMAND_LIST_TYPE list_type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::atomic<uint64_t> gpu_va_hint_{0};
    std::atomic<uint64_t> kickoff_count_{0};
    std::atomic<float> depth_bias_{0.0f};
    std::atomic<float> depth_bias_clamp_{0.0f};
    std::atomic<float> slope_scaled_depth_bias_{0.0f};
    std::atomic<float> depth_bounds_min_{0.0f};
    std::atomic<float> depth_bounds_max_{1.0f};
    std::atomic<uint32_t> view_instance_mask_{0u};
    std::atomic<uint64_t> write_buffer_immediate_calls_{0ull};
    mutable std::mutex ext_state_mutex_;
    uint32_t sample_positions_per_pixel_ = 0u;
    uint32_t sample_positions_pixels_ = 0u;
    std::vector<D3D12_SAMPLE_POSITION> sample_positions_;

    // Vulkan command-list routing recording state (used only when DeviceX::IsVulkanRoutingActive())
    mutable std::mutex vk_rec_mutex_;
    VulkanRecordedBatch vk_batch_;
    ID3D12PipelineState* vk_cur_pso_ = nullptr;
    bool vk_cur_pso_is_spirv_ = false;
    ID3D12Resource* vk_cur_rtv_ = nullptr;
    ID3D12Resource* vk_cur_dsv_ = nullptr;
    bool vk_has_clear_color_ = false;
    float vk_clear_color_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float vk_clear_depth_ = 1.0f;
    uint32_t vk_vp_width_ = 0u;
    uint32_t vk_vp_height_ = 0u;
    D3D12_VERTEX_BUFFER_VIEW vk_cur_vbv_{};
    bool vk_has_vbv_ = false;
    D3D12_INDEX_BUFFER_VIEW vk_cur_ibv_{};
    bool vk_has_ibv_ = false;
    std::vector<uint32_t> vk_gfx_root_constants_;
    std::vector<uint32_t> vk_cmp_root_constants_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_gfx_cbvs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_gfx_srvs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_gfx_uavs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_cmp_cbvs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_cmp_srvs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vk_cmp_uavs_;

    // Sprint 12 live AAA island state (tracked per command list)
    ID3D12RootSignature* last_host_compute_rs_ = nullptr;
    ID3D12PipelineState* last_host_pso_ = nullptr;
    ID3D12DescriptorHeap* bound_cbv_srv_uav_heap_ = nullptr;
    ID3D12DescriptorHeap* bound_sampler_heap_ = nullptr;
    uint32_t last_raw_topology_ = 0u;
    D3D12_VERTEX_BUFFER_VIEW last_vb0_raw_{};
    D3D12_VERTEX_BUFFER_VIEW last_vb0_host_{};
    D3D12_VERTEX_BUFFER_VIEW last_vb_raw_[8]{};
    D3D12_VERTEX_BUFFER_VIEW last_vb_host_[8]{};
    bool last_vb_need_upload_[8]{};
    uint8_t last_vb_valid_mask_ = 0u;
    D3D12_INDEX_BUFFER_VIEW last_ib_raw_{};
    D3D12_INDEX_BUFFER_VIEW last_ib_host_{};
    bool last_ib_need_upload_ = false;
    D3D12_GPU_VIRTUAL_ADDRESS last_gfx_root_srv_raw_[16]{};
    D3D12_GPU_VIRTUAL_ADDRESS last_gfx_root_srv_host_[16]{};
    mutable std::mutex aaa_rec_mutex_;
    std::vector<uint64_t> written_mirrored_vas_;
    std::vector<DeferredCpuWrite32> deferred_cpu_writes_;
    std::vector<DeferredCpuWait> deferred_cpu_waits_;
    std::vector<DeferredStateTransition> deferred_state_transitions_;
    void RecordMirroredResourceGpuWrite(ID3D12Resource* host_resource);

    // Sprint 13 placed tiled texture transient buffers & bridge helpers (Leonida #1674)
    static constexpr uint32_t kAlignedStagingRingSlots = 4u;
    struct AlignedStagingSlot {
        ID3D12Resource* buffer = nullptr;
        UINT64 capacity = 0ull;
        UINT64 cursor = 0ull;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        std::vector<ID3D12Resource*> retired_buffers;
    };
    AlignedStagingSlot aligned_staging_ring_[kAlignedStagingRingSlots]{};
    uint32_t aligned_staging_slot_idx_ = 0u;
    bool AcquireAlignedStagingSlice(
        UINT64 required_bytes,
        ID3D12Resource*& out_buf,
        UINT64& out_offset,
        D3D12_RESOURCE_STATES& out_current_state);
    void TransitionAlignedStagingState(D3D12_RESOURCE_STATES new_state);
    void ReleaseAlignedStagingRing();

    std::mutex tiled_transient_mutex_;
    std::vector<ID3D12Resource*> tiled_transient_resources_;
    std::vector<ID3D12Resource*> align_transient_ring_[6];
    size_t align_transient_slot_ = 0u;
    void TrackTiledTransientResource(ID3D12Resource* res);
    void ReleaseTiledTransientResources();
    void TrackAlignTransientResource(ID3D12Resource* res);
    void RotateAlignTransientRing();
    void ReleaseAllAlignTransientResources();
    bool TryBridgePlacedTiledCopyTextureRegion(
        const D3D12_TEXTURE_COPY_LOCATION& dst_loc,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION& src_loc,
        const D3D12_BOX* src_box);
    bool TryBridgePlacedTiledCopyResource(
        ID3D12Resource* host_dst,
        ID3D12Resource* host_src);
    bool TryBridgePlacedTiledCopyTiles(
        ID3D12Resource* host_tiled_res,
        const D3D12_TILED_RESOURCE_COORDINATE* start_coord,
        const D3D12_TILE_REGION_SIZE* region_size,
        ID3D12Resource* host_buffer,
        UINT64 buffer_offset_bytes,
        D3D12_TILE_COPY_FLAGS flags);
    void TrackPlacedTiledResourceBarriers(
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER* unwrapped_barriers,
        std::vector<D3D12_RESOURCE_BARRIER>& extra_coherency_barriers);

public:
    static bool IsDiagTraceEnabled() noexcept;
    static const char* DiagTraceLogPath() noexcept;
    void AppendDiagOp(std::string op);
    std::vector<std::string> CopyDiagOps() const;
    void ExtractTransientResources(std::vector<ID3D12Resource*>& out);
    bool LastCloseFailed() const noexcept { return last_close_failed_.load(std::memory_order_acquire); }

private:
    static constexpr uint32_t kRetailPrivateAllocRingSize = 8u;
    ID3D12CommandAllocator* retail_private_allocs_[kRetailPrivateAllocRingSize]{};
    uint32_t retail_private_alloc_idx_ = 0u;
    bool last_pso_is_standin_ = false;
    bool last_gfx_rs_is_empty_ = false;
    bool last_cmp_rs_is_empty_ = false;
    class PipelineStateXProxy* last_pso_proxy_ = nullptr;
    ID3D12RootSignature* last_host_graphics_rs_ = nullptr;
    ID3D12Resource* last_bound_rtv0_res_ = nullptr;
    DXGI_FORMAT last_bound_rtv_formats_[8]{};
    UINT last_bound_rtv_count_ = 0u;
    DXGI_FORMAT last_bound_dsv_format_ = DXGI_FORMAT_UNKNOWN;
    bool guest_set_stencil_ref_ = false;
    UINT guest_stencil_ref_ = 0u;
    ID3D12Resource* ei_sync_dummy_buf_ = nullptr;
    std::atomic<bool> last_close_failed_{false};
    mutable std::mutex diag_ops_mutex_;
    std::vector<std::string> diag_ops_;

    // Sprint 14 Enhanced Barriers telemetry & diagnostics
    std::atomic<uint64_t> eb_barrier_calls_{0ull};
    std::atomic<uint64_t> eb_native_calls_{0ull};
    std::atomic<uint64_t> eb_legacy_fallbacks_{0ull};
    std::atomic<uint64_t> eb_buffer_barriers_{0ull};
    std::atomic<uint64_t> eb_texture_barriers_{0ull};
    std::atomic<uint64_t> eb_global_barriers_{0ull};
    std::atomic<uint64_t> eb_resources_unwrapped_{0ull};
    std::atomic<uint64_t> eb_va_mirror_notifs_{0ull};
    std::atomic<uint64_t> eb_unified_tex_notifs_{0ull};
};

}  // namespace openscarlett::d3d12_xs
