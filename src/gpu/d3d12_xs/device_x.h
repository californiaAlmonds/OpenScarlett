#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/descriptor_sgpr_bridge.h"
#include "openscarlett/gpu/frame_capture.h"
#include "openscarlett/gpu/query_accumulator_x.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "openscarlett/gpu/sampler_feedback_x.h"
#include "openscarlett/gpu/shader_cache.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/kernel/title_va_mirror.h"

namespace openscarlett {

// Sprint 14 residency/profile/window helpers implemented in src/kernel/vram_residency_manager.cpp.
struct XboxConsoleProfileSelection {
    uint32_t profile_id = 0u;  // 0 = Series S (Lockhart 8 GiB), 1 = Series X (Anaconda 13.5 GiB)
    uint64_t target_console_vram_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    uint64_t host_dedicated_vram_bytes = 0ull;
    bool overridden_by_env = false;
};

XboxConsoleProfileSelection SelectXboxConsoleMemoryProfileFromHostVram(
    uint64_t host_dedicated_vram_bytes,
    const char* explicit_override_or_null = nullptr) noexcept;

#if defined(_WIN32)
struct HostAdapterVideoMemoryInfo {
    bool queried_dxgi_adapter3 = false;
    uint64_t local_budget_bytes = 0ull;
    uint64_t local_current_usage_bytes = 0ull;
    uint64_t local_available_reservation_bytes = 0ull;
    uint64_t non_local_budget_bytes = 0ull;
    uint64_t non_local_current_usage_bytes = 0ull;
};

bool QueryHostAdapterVideoMemoryInfo(
    ID3D12Device* host_device,
    HostAdapterVideoMemoryInfo& out_info) noexcept;
#endif

class SparseVramWindowIsolator;
SparseVramWindowIsolator* CreateSparseVramWindowIsolator();
void DestroySparseVramWindowIsolator(SparseVramWindowIsolator* iso) noexcept;
uint64_t SparseVramAllocateWindow0(SparseVramWindowIsolator* iso, uint64_t size_bytes) noexcept;
uint64_t SparseVramAllocateTitleWindow(SparseVramWindowIsolator* iso, uint64_t size_bytes) noexcept;
void SparseVramFree(SparseVramWindowIsolator* iso, uint64_t va) noexcept;
void SparseVramGetStats(
    const SparseVramWindowIsolator* iso,
    uint64_t& out_w0_allocs,
    uint64_t& out_w0_bytes,
    uint64_t& out_w1_allocs,
    uint64_t& out_w1_bytes) noexcept;

}  // namespace openscarlett

namespace openscarlett::d3d12_xs {

// Sprint 14 COM interface for Live VRAM Residency, Agility SDK status, Sparse Window-0 isolation,
// and Xbox Series PSO Stream normalization diagnostics.
// {4F534431-3258-5652-4D00-000000000033}
inline constexpr GUID IID_IOpenScarlettVramResidencyControl = {
    0x4F534431u, 0x3258u, 0x5652u,
    {0x4Du, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x33u}
};

struct D3D12XBOX_VRAM_RESIDENCY_STATS {
    uint32_t console_profile = 0u;  // 0 = Series S (8 GiB), 1 = Series X (13.5 GiB)
    uint32_t queried_dxgi_adapter3 = 0u;
    uint64_t target_console_vram_bytes = 0ull;
    uint64_t host_dedicated_vram_bytes = 0ull;
    uint64_t dxgi_local_budget_bytes = 0ull;
    uint64_t dxgi_local_current_usage_bytes = 0ull;
    uint64_t dxgi_non_local_budget_bytes = 0ull;
    uint64_t effective_budget_bytes = 0ull;
    uint64_t tracked_resident_bytes = 0ull;
    uint64_t tracked_evicted_bytes = 0ull;
    uint64_t l0_spilled_bytes = 0ull;
    uint64_t tracked_object_count = 0ull;
    uint64_t resident_object_count = 0ull;
    uint64_t evicted_object_count = 0ull;
    uint64_t l0_spill_count = 0ull;
    uint64_t auto_lru_eviction_count = 0ull;
    uint64_t make_resident_calls = 0ull;
    uint64_t evict_calls = 0ull;
    uint64_t set_priority_calls = 0ull;
    uint64_t enqueue_make_resident_calls = 0ull;
    uint64_t window0_descriptor_allocs = 0ull;
    uint64_t window0_descriptor_bytes = 0ull;
    uint64_t window1_title_buffer_allocs = 0ull;
    uint64_t window1_title_buffer_bytes = 0ull;
    uint64_t promoted_oversized_placed_resources = 0ull;
    uint64_t skipped_active_live_heaps = 0ull;
};

struct D3D12XBOX_AGILITY_SDK_INFO {
    uint32_t exported_sdk_version = 0u;
    uint32_t configured_sdk_version = 0u;
    uint32_t core_dll_found = 0u;
    uint32_t agility_active = 0u;
    uint32_t using_inbox_fallback = 1u;
    HRESULT last_config_hr = S_FALSE;
    char exported_sdk_path[64]{};
    char discovered_core_dir[260]{};
};

struct D3D12XBOX_PSO_STREAM_DIAG_STATS {
    uint64_t remapped_as_tags = 0ull;
    uint64_t remapped_ms_tags = 0ull;
    uint64_t remapped_ext_tags = 0ull;
    uint64_t dropped_legacy_stages = 0ull;
    uint64_t inferred_mesh_topologies = 0ull;
    uint64_t f16_stage_links_reconciled = 0ull;
    uint64_t interp_validations_run = 0ull;
    uint64_t interp_attributes_matched = 0ull;
};

struct IOpenScarlettVramResidencyControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVramResidencyStats(
        D3D12XBOX_VRAM_RESIDENCY_STATS* out_stats) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetResidencyBudgetOverride(
        uint64_t budget_bytes_or_zero_for_auto) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetConsoleProfileOverride(
        int32_t profile_or_neg1_for_auto) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAgilitySdkInfo(
        D3D12XBOX_AGILITY_SDK_INFO* out_info) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPsoStreamDiagStats(
        D3D12XBOX_PSO_STREAM_DIAG_STATS* out_stats) = 0;
    virtual HRESULT STDMETHODCALLTYPE AllocateSparseWindowVa(
        uint32_t is_descriptor_window0,
        uint64_t size_bytes,
        uint64_t* out_va) = 0;
    virtual HRESULT STDMETHODCALLTYPE FreeSparseWindowVa(
        uint64_t va) = 0;
};

void QueryAgilitySdkRuntimeSnapshot(D3D12XBOX_AGILITY_SDK_INFO* out_info) noexcept;

// Display VLine & Frame notification registry implemented in src/shims/d3d12_xs_shim.cpp
// and signaled on CommandQueueX::PresentX completion (xvalkyrie a252811).
void SignalRegisteredDisplayNotificationsOnPresent(
    uint64_t* out_vline_signaled = nullptr,
    uint64_t* out_frame_signaled = nullptr) noexcept;
void GetDisplayNotificationCounts(
    uint64_t* out_vline_signaled,
    uint64_t* out_frame_signaled) noexcept;

class DeviceExtendedProxy;
class DxgiDeviceProxy;

struct VulkanPipelineInfo {
    bool is_spirv_direct = false;
    bool is_compute = false;
    bool is_mesh = false;
    std::vector<uint32_t> vs_spirv;
    std::vector<uint32_t> ps_spirv;
    std::vector<uint32_t> cs_spirv;
    std::vector<uint32_t> as_spirv;
    std::vector<uint32_t> ms_spirv;
    std::string vs_entry = "main";
    std::string ps_entry = "main";
    std::string cs_entry = "main";
    std::string as_entry = "main";
    std::string ms_entry = "main";
    std::vector<openscarlett::VulkanVertexAttribute> vertex_attrs;
    uint32_t vertex_stride_hint = 0u;
    bool enable_depth = false;
    bool depth_write = true;
    openscarlett::VulkanImageFormat color_format = openscarlett::VulkanImageFormat::Rgba8Unorm;
};

struct VulkanTrackedResource {
    ID3D12Resource* resource = nullptr;
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0ull;
    std::vector<uint8_t> shadow_bytes;
};

struct UnifiedTextureMapping {
    ID3D12Resource* resource = nullptr;
    uint64_t guest_va = 0ull;
    uint64_t byte_size = 0ull;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth_or_array_size = 1u;
    uint32_t mip_levels = 1u;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t row_pitch_bytes = 0u;
    uint64_t page_generation = 1ull;
    uint64_t uploaded_generation = 0ull;
    bool dirty = false;
    bool allow_cpu_upload = true;
    bool cpu_readable = false;
    bool write_watch_enabled = false;
    bool has_uploaded_once = false;
    bool has_uploaded_nonzero = false;
    bool mirrored_by_title_va = false;
    bool has_pending_hash = false;
    uint64_t last_uploaded_hash = 0ull;
    uint64_t pending_hash = 0ull;
};

struct XMemAllocationRecord {
    uint64_t guest_va = 0ull;
    uint64_t byte_size = 0ull;
    uint64_t aligned_size = 0ull;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* anchor_buffer = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS desktop_gpu_va = 0ull;
};

class DeviceX final
    : public ID3D12DeviceX,
      public IOpenScarlettUnifiedMemory,
      public IOpenScarlettVulkanRouting,
      public IOpenScarlettAaaRuntime,
      public IOpenScarlettVramResidencyControl,
      public IOpenScarlettGraphicsParity,
      public IOpenScarlettGraphicsCompleteness {
public:
    DeviceX(ID3D12Device* host_device,
            const D3D12AdapterInfo& adapter_info,
            HWND default_hwnd = nullptr);

    void SetRetailScarlettAbi(bool enabled) noexcept { retail_scarlett_abi_ = enabled; }
    bool IsRetailScarlettAbi() const noexcept { return retail_scarlett_abi_; }
    void LiftPendingXdatGraphicsPso(class PipelineStateXProxy* proxy);
    ID3D12PipelineState* CreateGraphicsPsoTargetVariant(
        class PipelineStateXProxy* proxy,
        UINT num_rtvs,
        const DXGI_FORMAT* rtv_formats,
        DXGI_FORMAT dsv_format);
    HRESULT CreateDerivedGraphicsPipelineStateX(
        ID3D12PipelineState* pSourcePso,
        UINT numOverrides,
        const void* pOverrides,
        REFIID riid,
        void** ppPipelineState);
    HRESULT DeserializeGraphicsPipelineStateX(
        const void* pDesc,
        REFIID riid,
        void** ppPipelineState);
    HRESULT DeserializeComputePipelineStateX(
        const void* pDesc,
        REFIID riid,
        void** ppPipelineState);
    void CreatePlacedShaderResourceViewX(
        const D3D12_RESOURCE_DESC* pResourceDesc,
        const void* pViewDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor);
    void SetLastDiiRtv(ID3D12Resource* res) noexcept {
        last_dii_rtv_.store(res, std::memory_order_release);
    }
    ID3D12Resource* LastDiiRtv() const noexcept {
        return last_dii_rtv_.load(std::memory_order_acquire);
    }
    uint32_t LazyLiftCount() const noexcept {
        return lazy_lift_count_.load(std::memory_order_acquire);
    }
    void SetRetailResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES st) noexcept {
        if (!res) return;
        std::lock_guard<std::mutex> lk(retail_state_mutex_);
        retail_res_states_[res] = st;
    }
    D3D12_RESOURCE_STATES GetRetailResourceState(
        ID3D12Resource* res, D3D12_RESOURCE_STATES fallback = D3D12_RESOURCE_STATE_COMMON) const noexcept {
        if (!res) return fallback;
        std::lock_guard<std::mutex> lk(retail_state_mutex_);
        auto it = retail_res_states_.find(res);
        return (it != retail_res_states_.end()) ? it->second : fallback;
    }
    D3D12_RESOURCE_STATES TransitionRetailResourceState(
        ID3D12Resource* res, D3D12_RESOURCE_STATES new_state,
        D3D12_RESOURCE_STATES default_before = D3D12_RESOURCE_STATE_COMMON) noexcept {
        if (!res) return new_state;
        std::lock_guard<std::mutex> lk(retail_state_mutex_);
        auto it = retail_res_states_.find(res);
        const D3D12_RESOURCE_STATES old_state =
            (it != retail_res_states_.end()) ? it->second : default_before;
        retail_res_states_[res] = new_state;
        return old_state;
    }
    ID3D12Resource* LookupRetailRtvResource(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto it = valid_rtv_handles_.find(ptr);
        return (it != valid_rtv_handles_.end()) ? it->second : nullptr;
    }
    ID3D12Resource* LookupRetailDsvResource(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto it = valid_dsv_handles_.find(ptr);
        return (it != valid_dsv_handles_.end()) ? it->second : nullptr;
    }
    DXGI_FORMAT LookupRetailRtvFormat(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto it = valid_rtv_formats_.find(ptr);
        return (it != valid_rtv_formats_.end()) ? it->second : DXGI_FORMAT_UNKNOWN;
    }
    DXGI_FORMAT LookupRetailDsvFormat(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        auto it = valid_dsv_formats_.find(ptr);
        return (it != valid_dsv_formats_.end()) ? it->second : DXGI_FORMAT_UNKNOWN;
    }
    bool IsValidRetailRtvHandle(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        return valid_rtv_handles_.find(ptr) != valid_rtv_handles_.end();
    }
    bool IsValidRetailDsvHandle(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        return valid_dsv_handles_.find(ptr) != valid_dsv_handles_.end();
    }
    bool IsReadOnlyRetailDsvHandle(SIZE_T ptr) const noexcept {
        std::lock_guard<std::mutex> lk(valid_view_mutex_);
        return readonly_dsv_handles_.find(ptr) != readonly_dsv_handles_.end();
    }
    bool IsSamplerRootTableParam(void* host_rs, UINT root_param_index) const noexcept;
    bool IsValidRootDescriptorTableParam(
        void* host_rs, UINT root_param_index, bool* out_is_sampler = nullptr) const noexcept;
    ~DeviceX();

    DeviceX(const DeviceX&) = delete;
    DeviceX& operator=(const DeviceX&) = delete;

    ID3D12Device* HostDevice() const noexcept { return host_dev_; }
    const D3D12AdapterInfo& AdapterInfo() const noexcept { return adapter_info_; }
    HWND DefaultWindow() const noexcept;
    void SetDefaultWindow(HWND hwnd) noexcept;
    bool AdoptTitleWindow(HWND hwnd) noexcept;
    HWND ResolveEffectiveOutputWindow(HWND explicit_hwnd = nullptr) const noexcept;
    bool SupportsDepthBoundsTest() const noexcept { return supports_depth_bounds_test_; }
    bool SupportsViewInstancing() const noexcept { return supports_view_instancing_; }
    bool SupportsProgrammableSamplePositions() const noexcept {
        return supports_programmable_sample_positions_;
    }

    // Lifecycle cleanup notifications invoked by proxy destructors to prevent stale pointer reuse.
    void NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept;
    void DeferHostResourceRelease(ID3D12Resource* host_resource) noexcept;
    void OnPresentFrameComplete(uint64_t present_frame) noexcept;
    void NotifyHeapDestroyed(ID3D12Heap* host_heap) noexcept;
    void NotifyPipelineDestroyed(ID3D12PipelineState* pso_key) noexcept;

    // Zero-overhead C++ integration hooks for TitleVAMirror & DescriptorSgprBridge.
    using GuestGpuVaTranslatorFn = D3D12_GPU_VIRTUAL_ADDRESS (*)(
        D3D12_GPU_VIRTUAL_ADDRESS guest_va, void* user_ctx) noexcept;
    using DescriptorCreateHookFn = void (*)(
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
        D3D12_CPU_DESCRIPTOR_HANDLE dest,
        ID3D12Resource* host_resource,
        D3D12_GPU_VIRTUAL_ADDRESS gpu_va,
        void* user_ctx) noexcept;
    void SetGuestGpuVaTranslatorHook(GuestGpuVaTranslatorFn fn, void* user_ctx) noexcept;
    void SetDescriptorCreateHook(DescriptorCreateHookFn fn, void* user_ctx) noexcept;
    openscarlett::TitleVAMirror& GetTitleVaMirror() noexcept { return title_va_mirror_; }
    const openscarlett::TitleVAMirror& GetTitleVaMirror() const noexcept { return title_va_mirror_; }
    openscarlett::DescriptorSgprBridge& GetDescriptorBridge() noexcept { return descriptor_bridge_; }
    const openscarlett::DescriptorSgprBridge& GetDescriptorBridge() const noexcept { return descriptor_bridge_; }
    SamplerFeedbackManager& GetSamplerFeedbackManager() noexcept { return sampler_feedback_mgr_; }
    const SamplerFeedbackManager& GetSamplerFeedbackManager() const noexcept { return sampler_feedback_mgr_; }
    QueryAccumulatorX& GetQueryAccumulator() noexcept { return query_accumulator_; }
    const QueryAccumulatorX& GetQueryAccumulator() const noexcept { return query_accumulator_; }

    bool IsDeviceRemoved() const noexcept {
        return FAILED(device_removed_reason_.load(std::memory_order_acquire));
    }
    bool HasMirroredTitleVaRegions() const noexcept {
        return mirrored_va_count_.load(std::memory_order_acquire) > 0u;
    }
    bool LookupMirroredTitleVaForResource(
        ID3D12Resource* host_resource, uint64_t& out_title_va) const noexcept;
    HRESULT FlushMirroredTitleVaPagesOnQueue(
        ID3D12CommandQueue* host_queue,
        D3D12_COMMAND_LIST_TYPE queue_type,
        uint64_t* out_bytes_uploaded = nullptr,
        bool wait_for_gpu = true);
    ID3D12Resource* ResolveBufferResourceByGpuVa(
        D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes = nullptr) const;
    XG_TILE_MODE GetTrackedResourceTileMode(ID3D12Resource* host_resource) const noexcept;
    void SetTrackedResourceTileMode(ID3D12Resource* host_resource, XG_TILE_MODE mode);
    bool EnsureXgGpuUntilerReady();
    openscarlett::XgGpuUntiler* GetXgGpuUntiler() noexcept { return xg_untiler_.get(); }
    HRESULT RecordUntileUploadOnCommandList(
        ID3D12GraphicsCommandList* host_list,
        D3D12_COMMAND_LIST_TYPE list_type,
        const D3D12_TEXTURE_COPY_LOCATION& dst_loc,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION& src_loc,
        const D3D12_BOX* src_box,
        XG_TILE_MODE tile_mode);
    HRESULT RecordFeedbackFallbackDecode(
        ID3D12GraphicsCommandList* host_list,
        ID3D12Resource* host_feedback_src,
        ID3D12Resource* host_dst_tex,
        UINT dst_subresource,
        UINT dst_x,
        UINT dst_y);
    HRESULT RecordFeedbackFallbackEncode(
        ID3D12GraphicsCommandList* host_list,
        ID3D12Resource* host_src_tex,
        UINT src_subresource,
        ID3D12Resource* host_feedback_dst);

    void RecordCommandListQueryBegin() noexcept { qa_cl_begin_count_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCommandListQueryEnd() noexcept { qa_cl_end_count_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCommandListQueryResolve() noexcept { qa_cl_resolve_count_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordBridgeHeapBind() noexcept { db_heap_binds_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordBridgeTableBind() noexcept { db_table_binds_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordBridgeRootConstUpdate() noexcept { db_root_const_updates_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordBridgeRootCbvUpdate() noexcept { db_root_cbv_updates_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordIndirectMirrorFlush() noexcept { va_indirect_flushes_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordFeedbackDecodeIntercept() noexcept { sf_decode_intercepts_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordFeedbackEncodeIntercept() noexcept { sf_encode_intercepts_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCmdListKickoff() noexcept { cmdlist_kickoffs_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCmdListWrite32() noexcept { cmdlist_write32_calls_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCmdListPredicationX() noexcept { cmdlist_predicationx_calls_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCmdListFlushCache() noexcept { cmdlist_flush_cache_calls_.fetch_add(1ull, std::memory_order_relaxed); }
    void RecordCmdListDispatchMesh(UINT x, UINT y, UINT z) noexcept {
        cmdlist_dispatch_mesh_calls_.fetch_add(1ull, std::memory_order_relaxed);
        last_mesh_x_.store(x, std::memory_order_relaxed);
        last_mesh_y_.store(y, std::memory_order_relaxed);
        last_mesh_z_.store(z, std::memory_order_relaxed);
        const uint64_t groups = static_cast<uint64_t>(x) * static_cast<uint64_t>(y) * static_cast<uint64_t>(z);
        total_mesh_groups_.fetch_add(groups, std::memory_order_relaxed);
    }
    bool IsR9G9B9E5UavFallbackActive() const noexcept;
    static void SetForceR9G9B9E5UavFallback(bool force) noexcept;
    static void SetGlobalBackendMode(uint32_t backend_mode) noexcept;
    static uint32_t ResolveEffectiveBackendMode() noexcept;

    bool IsVulkanRoutingActive() const noexcept { return vulkan_routing_active_; }
    openscarlett::VulkanBackend* GetVulkanBackend() const noexcept { return vulkan_backend_.get(); }
    bool LookupVulkanPipeline(ID3D12PipelineState* pso, VulkanPipelineInfo& out_info) const;
    bool IsSpirvDirectPipeline(ID3D12PipelineState* pso) const;
    ID3D12Resource* ResolveVulkanRtvResource(D3D12_CPU_DESCRIPTOR_HANDLE rtv) const;
    ID3D12Resource* ResolveVulkanDsvResource(D3D12_CPU_DESCRIPTOR_HANDLE dsv) const;
    ID3D12Resource* ResolveVulkanBufferByGpuVa(
        D3D12_GPU_VIRTUAL_ADDRESS va, uint64_t* out_offset_bytes = nullptr) const;
    bool ReadVulkanBufferData(
        ID3D12Resource* res, uint64_t offset, uint64_t size, std::vector<uint8_t>& out_bytes);
    bool WriteVulkanBufferData(
        ID3D12Resource* res, uint64_t offset, const void* data, size_t size);
    void StoreVulkanRenderTargetFramebuffer(
        ID3D12Resource* rt_res, const openscarlett::FrameBufferRgba8& fb);
    bool TryGetVulkanRenderTargetFramebuffer(
        ID3D12Resource* rt_res, openscarlett::FrameBufferRgba8& out_fb) const;
    void RecordVulkanRoutingCounters(
        uint64_t cmd_lists,
        uint64_t draws,
        uint64_t indexed_draws,
        uint64_t dispatches,
        uint64_t mesh_dispatches,
        uint64_t copies,
        uint64_t barriers,
        uint64_t rtv_clears,
        uint64_t dsv_clears,
        uint64_t presents,
        uint64_t icd_execs,
        uint64_t spirv_draws,
        uint64_t spirv_dispatches,
        uint64_t spirv_mesh_dispatches) noexcept;
    D3D12_GPU_VIRTUAL_ADDRESS TranslateGuestGpuVa(D3D12_GPU_VIRTUAL_ADDRESS address) const noexcept;
    // Desktop D3D12 requires 256-byte alignment for CBV GPU VAs, while Xbox Series X allows
    // 16-byte CBV alignment. Copies unaligned slices into a persistently-mapped 256B-aligned ring.
    D3D12_GPU_VIRTUAL_ADDRESS AlignRootCbvGpuVaForDesktop(
        D3D12_GPU_VIRTUAL_ADDRESS translated_va,
        D3D12_GPU_VIRTUAL_ADDRESS guest_va_fallback = 0ull);
    // Uploads an unmapped Xbox unified CPU/TitleVA slice (e.g. Scaleform dynamic VB/IB/CBV
    // allocated directly in CPU heap without CreatePlacedResourceX) into a persistently-mapped
    // desktop UPLOAD ring and returns a real desktop D3D12_GPU_VIRTUAL_ADDRESS.
    D3D12_GPU_VIRTUAL_ADDRESS UploadTransientGuestBufferForDesktop(
        D3D12_GPU_VIRTUAL_ADDRESS guest_va,
        uint64_t size_bytes,
        uint64_t alignment = 256ull,
        bool allow_same_frame_cache = true);
    D3D12_GPU_VIRTUAL_ADDRESS EnsureDummyRootUavBufferGpuVa();
    D3D12_GPU_VIRTUAL_ADDRESS ResolveOrUploadRootCbvGpuVa(D3D12_GPU_VIRTUAL_ADDRESS guest_va);
    D3D12_GPU_VIRTUAL_ADDRESS ResolveOrUploadRootSrvGpuVa(D3D12_GPU_VIRTUAL_ADDRESS guest_va);
    D3D12_GPU_VIRTUAL_ADDRESS ResolveOrDummyRootUavGpuVa(D3D12_GPU_VIRTUAL_ADDRESS guest_va);
    uint64_t ResolveCpuAliasVa(uint64_t guest_va) const noexcept;
    uint32_t FrameIntervalMicroseconds() const noexcept {
        return frame_interval_us_.load(std::memory_order_relaxed);
    }
    uint32_t MaxFrameLatency() const noexcept {
        return max_frame_latency_.load(std::memory_order_relaxed);
    }
    bool HasRegisteredUnifiedTextures() const noexcept {
        return registered_unified_count_.load(std::memory_order_acquire) > 0u;
    }
    bool HasDirtyUnifiedTextures() const noexcept {
        return dirty_unified_count_.load(std::memory_order_acquire) > 0u;
    }
    bool RefreshDirtyUnifiedResources() noexcept;
    HRESULT SyncAllUnifiedResourcesBeforeSubmit(
        ID3D12CommandQueue* host_queue,
        D3D12_COMMAND_LIST_TYPE queue_type);
    void NotifyResourceStateTransition(
        ID3D12Resource* resource, D3D12_RESOURCE_STATES new_state) noexcept;
    void RegisterUmaShadowMapping(
        ID3D12Resource* resource, void* cpu_ptr, const D3D12_RESOURCE_DESC& desc);
    void MarkUmaShadowDirty(ID3D12Resource* resource) noexcept;

    // IUnknown (slots 0..2)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object (slots 3..6)
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12Device (slots 7..43)
    UINT STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(
        const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator,
        ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
        const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(
        UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes,
        REFIID riid, void** ppvRootSignature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(
        const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(
        ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(
        ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(
        ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(
        ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateSampler(
        const D3D12_SAMPLER_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CopyDescriptors(
        UINT NumDestDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes,
        UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
        const UINT* pSrcDescriptorRangeSizes,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(
        UINT NumDescriptors,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
        D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
        D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetResourceAllocationInfo;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet,
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override;
    using ID3D12Device::GetCustomHeapProperties;
    D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
        D3D12_HEAP_PROPERTIES* pRet,
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visibleMask, UINT numResourceDescs,
        const D3D12_RESOURCE_DESC* pResourceDescs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(
        UINT nodeMask, D3D12_HEAP_TYPE heapType) override;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource,
        void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(
        const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(
        ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid,
        void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(
        ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes,
        DWORD Access, LPCWSTR Name, HANDLE* pHandle) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(
        HANDLE NTHandle, REFIID riid, void** ppvObj) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(
        LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) override;
    HRESULT STDMETHODCALLTYPE MakeResident(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE Evict(
        UINT NumObjects, ID3D12Pageable* const* ppObjects) override;
    HRESULT STDMETHODCALLTYPE CreateFence(
        UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void STDMETHODCALLTYPE GetCopyableFootprints(
        const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources,
        UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows,
        UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(
        const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(
        const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature,
        REFIID riid, void** ppvCommandSignature) override;
    void STDMETHODCALLTYPE GetResourceTiling(
        ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource,
        D3D12_PACKED_MIP_INFO* pPackedMipDesc,
        D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips,
        UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet,
        D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device::GetAdapterLuid;
    LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID* pRet) override;
#else
    LUID STDMETHODCALLTYPE GetAdapterLuid() override;
#endif

    // ID3D12Device1 (slots 44..46)
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
        const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid,
        void** ppPipelineLibrary) override;
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
        ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences,
        D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) override;
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(
        UINT NumObjects, ID3D12Pageable* const* ppObjects,
        const D3D12_RESIDENCY_PRIORITY* pPriorities) override;

    // ID3D12Device2 (slot 47)
    HRESULT STDMETHODCALLTYPE CreatePipelineState(
        const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid,
        void** ppPipelineState) override;

    // ID3D12Device3 (slots 48..50)
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
        const void* pAddress, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
        HANDLE hFileMapping, REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
        D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects,
        ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) override;

    // ID3D12Device4 (slots 51..56)
    HRESULT STDMETHODCALLTYPE CreateCommandList1(
        UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
        REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid,
        void** ppSession) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
        const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap1(
        const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvHeap) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(
        const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riid, void** ppvResource) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device4::GetResourceAllocationInfo1;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo1(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet,
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(
        UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#endif

    // ID3D12Device5 (slots 57..64)
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
        ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) override;
    void STDMETHODCALLTYPE RemoveDevice() override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
        UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
        REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage,
        UINT* pTotalStructureSizeInBytes, UINT* pParameterCount,
        D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) override;
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(
        REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData,
        SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) override;
    HRESULT STDMETHODCALLTYPE CreateStateObject(
        const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) override;
    void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) override;
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
        D3D12_SERIALIZED_DATA_TYPE SerializedDataType,
        const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) override;

    // ID3D12Device6 (slot 65)
    HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(
        D3D12_BACKGROUND_PROCESSING_MODE Mode,
        D3D12_MEASUREMENTS_ACTION MeasurementsAction,
        HANDLE hEventToSignalUponCompletion,
        BOOL* pbFurtherMeasurementsDesired) override;

    // ID3D12Device7 (slots 66..67)
    HRESULT STDMETHODCALLTYPE AddToStateObject(
        const D3D12_STATE_OBJECT_DESC* pAddition,
        ID3D12StateObject* pStateObjectToGrowFrom,
        REFIID riid,
        void** ppNewStateObject) override;
    HRESULT DeserializeStateObjectX(
        const void* pBlob,
        SIZE_T blobSize,
        ID3D12RootSignature* pRootSignature,
        UINT flags,
        REFIID riid,
        void** ppStateObject);
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(
        const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc,
        REFIID riid,
        void** ppSession) override;

    // ID3D12Device8 (slots 68..72)
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12Device8::GetResourceAllocationInfo2;
    D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo2(
        D3D12_RESOURCE_ALLOCATION_INFO* pRet,
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC1* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#else
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo2(
        UINT visibleMask,
        UINT numResourceDescs,
        const D3D12_RESOURCE_DESC1* pResourceDescs,
        D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) override;
#endif
    HRESULT STDMETHODCALLTYPE CreateCommittedResource2(
        const D3D12_HEAP_PROPERTIES* pHeapProperties,
        D3D12_HEAP_FLAGS HeapFlags,
        const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        ID3D12ProtectedResourceSession* pProtectedSession,
        REFIID riidResource,
        void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource1(
        ID3D12Heap* pHeap,
        UINT64 HeapOffset,
        const D3D12_RESOURCE_DESC1* pDesc,
        D3D12_RESOURCE_STATES InitialState,
        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid,
        void** ppvResource) override;
    void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(
        ID3D12Resource* pTargetedResource,
        ID3D12Resource* pFeedbackResource,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE GetCopyableFootprints1(
        const D3D12_RESOURCE_DESC1* pResourceDesc,
        UINT FirstSubresource,
        UINT NumSubresources,
        UINT64 BaseOffset,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
        UINT* pNumRows,
        UINT64* pRowSizeInBytes,
        UINT64* pTotalBytes) override;

    // ID3D12DeviceX (slots 73..79; slots 73..75 match real GDKX ID3D12Device8 Xbox methods)
    HRESULT STDMETHODCALLTYPE SetFrameIntervalX(
        IDXGIOutput* pOutput, uint32_t PeriodMicroseconds,
        uint32_t MaxFrameLatency, uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint64_t DelayMicroseconds,
        HANDLE hEvent, uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT Event, uint32_t TimeoutMilliseconds,
        void* pReserved, uint32_t Flags,
        D3D12XBOX_FRAME_PIPELINE_TOKEN* pOutToken) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResourceX(
        D3D12_GPU_VIRTUAL_ADDRESS VirtualAddress, const D3D12_RESOURCE_DESC* pDesc,
        D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue,
        REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ResumeX() override;
    HRESULT STDMETHODCALLTYPE GetGpuHardwareInfoX(D3D12XBOX_GPU_HARDWARE_INFO* pOutInfo) override;
    HRESULT STDMETHODCALLTYPE GetGpuHardwareConfigurationX(
        D3D12XBOX_GPU_HARDWARE_CONFIGURATION* pOutConfig) override;

    // IOpenScarlettUnifiedMemory
    HRESULT STDMETHODCALLTYPE RegisterUnifiedTexture(
        const D3D12XBOX_UNIFIED_TEXTURE_DESC* desc) override;
    HRESULT STDMETHODCALLTYPE UnregisterUnifiedTexture(
        ID3D12Resource* resource) override;
    HRESULT STDMETHODCALLTYPE NotifyVirtualRemap(
        uint64_t guest_va_base,
        uint64_t byte_size,
        uint64_t new_backing_va,
        uint32_t* out_flagged_count) override;
    HRESULT STDMETHODCALLTYPE FlushDirtyUnifiedTextures(
        ID3D12CommandQueue* queue,
        uint32_t* out_flushed_count) override;
    HRESULT STDMETHODCALLTYPE GetUnifiedTextureStats(
        D3D12XBOX_UNIFIED_TEXTURE_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE AttachXMemAllocation(
        uint64_t guest_va,
        uint64_t byte_size,
        uint64_t* out_desktop_gpu_va) override;
    HRESULT STDMETHODCALLTYPE DetachXMemAllocation(
        uint64_t guest_va) override;
    HRESULT STDMETHODCALLTYPE ResolveXMemGpuVa(
        uint64_t guest_va,
        uint64_t* out_desktop_gpu_va) override;
    HRESULT STDMETHODCALLTYPE GetR9G9B9E5UavFallbackActive(
        int* out_active) override;

    // IOpenScarlettVulkanRouting
    uint32_t STDMETHODCALLTYPE GetActiveBackendMode() override;
    HRESULT STDMETHODCALLTYPE GetVulkanRoutingStats(
        D3D12XBOX_VULKAN_ROUTING_STATS* out_stats) override;

    // IOpenScarlettAaaRuntime
    HRESULT STDMETHODCALLTYPE GetAaaWiringStats(
        D3D12XBOX_AAA_WIRING_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE SetResourceTileMode(
        ID3D12Resource* resource,
        uint32_t xg_tile_mode) override;
    HRESULT STDMETHODCALLTYPE GetResourceTileMode(
        ID3D12Resource* resource,
        uint32_t* out_xg_tile_mode) override;
    HRESULT STDMETHODCALLTYPE GetQueryAccumulatorHeap(
        uint32_t query_kind,
        ID3D12QueryHeap** out_heap) override;
    HRESULT STDMETHODCALLTYPE FlushTitleVaMirrorOnQueue(
        ID3D12CommandQueue* queue,
        uint64_t* out_bytes_uploaded) override;
    HRESULT STDMETHODCALLTYPE ScheduleTitleVaWriteback(
        ID3D12GraphicsCommandList* cmd_list,
        uint64_t title_va,
        uint64_t fence_value,
        uint64_t offset,
        uint64_t size_bytes) override;
    HRESULT STDMETHODCALLTYPE CompleteTitleVaWritebacks(
        uint64_t completed_fence_value,
        uint64_t* out_completed_count) override;
    HRESULT STDMETHODCALLTYPE ClearFeedbackMap(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* feedback_resource) override;
    HRESULT STDMETHODCALLTYPE WriteFeedbackSamples(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* feedback_resource,
        const uint32_t* packed_xy_slice_mip_quads,
        uint32_t sample_count) override;
    HRESULT STDMETHODCALLTYPE ReadbackFeedbackMinMip(
        ID3D12CommandQueue* queue,
        ID3D12Resource* feedback_resource,
        uint8_t* out_tiles,
        uint32_t max_tiles,
        uint32_t* out_tiles_x,
        uint32_t* out_tiles_y) override;
    HRESULT STDMETHODCALLTYPE CalibrateQueueClock(
        ID3D12CommandQueue* queue,
        D3D12XBOX_CLOCK_CALIBRATION_SUMMARY* out_summary) override;
    HRESULT STDMETHODCALLTYPE BindBridgeBufferSrv(
        ID3D12Resource* resource,
        uint32_t num_elements,
        uint32_t stride_bytes,
        uint32_t* out_bindless_index) override;
    HRESULT STDMETHODCALLTYPE BindBridgeHeaps(
        ID3D12GraphicsCommandList* cmd_list) override;

    // IOpenScarlettVramResidencyControl
    HRESULT STDMETHODCALLTYPE GetVramResidencyStats(
        D3D12XBOX_VRAM_RESIDENCY_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE SetResidencyBudgetOverride(
        uint64_t budget_bytes_or_zero_for_auto) override;
    HRESULT STDMETHODCALLTYPE SetConsoleProfileOverride(
        int32_t profile_or_neg1_for_auto) override;
    HRESULT STDMETHODCALLTYPE GetAgilitySdkInfo(
        D3D12XBOX_AGILITY_SDK_INFO* out_info) override;
    HRESULT STDMETHODCALLTYPE GetPsoStreamDiagStats(
        D3D12XBOX_PSO_STREAM_DIAG_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE AllocateSparseWindowVa(
        uint32_t is_descriptor_window0,
        uint64_t size_bytes,
        uint64_t* out_va) override;
    HRESULT STDMETHODCALLTYPE FreeSparseWindowVa(
        uint64_t va) override;

    // IOpenScarlettGraphicsParity
    HRESULT STDMETHODCALLTYPE GetGraphicsParityStats(
        D3D12XBOX_GRAPHICS_PARITY_STATS* out_stats) override;

    // IOpenScarlettGraphicsCompleteness (Sprint 16)
    HRESULT STDMETHODCALLTYPE GetGraphicsCompletenessStats(
        D3D12XBOX_GRAPHICS_COMPLETENESS_STATS* out_stats) override;
    HRESULT STDMETHODCALLTYPE AllocateDescriptorHeapPage(
        ID3D12DescriptorHeap* heap,
        uint32_t* out_page_index,
        uint64_t* out_generation,
        uint64_t* out_page_gpu_base) override;
    HRESULT STDMETHODCALLTYPE RetireDescriptorHeapPage(
        ID3D12DescriptorHeap* heap,
        uint32_t page_index) override;
    HRESULT STDMETHODCALLTYPE ExecuteGpuResolveFmask(
        uint32_t width,
        uint32_t height,
        uint32_t sample_count,
        uint32_t resolve_mode,
        uint32_t clear_rgba8,
        const uint8_t* msaa_color_bytes,
        uint32_t msaa_color_size,
        const uint8_t* fmask_bytes,
        uint32_t fmask_size,
        uint8_t* out_rgba8,
        uint32_t out_size,
        uint32_t* out_used_gpu) override;
    HRESULT STDMETHODCALLTYPE ExecuteGpuDecompressDcc(
        uint32_t width,
        uint32_t height,
        uint32_t pipe_aligned,
        uint32_t num_pipes,
        uint32_t clear_rgba8,
        const uint8_t* src_blocks,
        uint32_t src_size,
        const uint8_t* dcc_keys,
        uint32_t dcc_size,
        uint8_t* out_rgba8,
        uint32_t out_size,
        uint32_t* out_used_gpu) override;
    HRESULT STDMETHODCALLTYPE ExecuteGpuDecompressHtile(
        uint32_t width,
        uint32_t height,
        uint32_t mode_flags,
        uint32_t clear_reg0,
        uint32_t clear_reg1,
        const uint8_t* src_tiles,
        uint32_t src_size,
        const uint8_t* htile_words,
        uint32_t htile_size,
        uint8_t* out_depth_and_hiz,
        uint32_t out_size,
        uint32_t* out_used_gpu) override;
    HRESULT STDMETHODCALLTYPE MarkResidencyObjectActive(
        ID3D12Pageable* obj,
        int32_t delta_ref) override;
    HRESULT STDMETHODCALLTYPE MarkResidencyObjectInFlight(
        ID3D12Pageable* obj,
        ID3D12Fence* fence,
        uint64_t fence_value) override;

    // Sprint 16 helpers for CommandListX / CommandQueueX
    D3D12_GPU_DESCRIPTOR_HANDLE TranslateGpuDescriptorHandle(
        D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept;
    void NotifyDescriptorHeapBoundOnCommandList(ID3D12DescriptorHeap* host_heap) noexcept;
    void RecordCopyQueueBarrierSanitized(bool was_uav_suppressed = false) noexcept {
        copy_queue_barriers_sanitized_.fetch_add(1ull, std::memory_order_relaxed);
        if (was_uav_suppressed) {
            copy_queue_uav_barriers_suppressed_.fetch_add(1ull, std::memory_order_relaxed);
        }
    }
    void RecordExecuteIndirectStats(
        bool has_count,
        bool has_root_consts,
        bool is_dispatch_mesh,
        bool bounds_clamped,
        bool rejected,
        bool va_translated,
        bool vulkan_routed) noexcept {
        execute_indirect_calls_.fetch_add(1ull, std::memory_order_relaxed);
        if (has_count) execute_indirect_with_count_buffer_.fetch_add(1ull, std::memory_order_relaxed);
        if (has_root_consts) execute_indirect_with_root_constants_.fetch_add(1ull, std::memory_order_relaxed);
        if (is_dispatch_mesh) execute_indirect_dispatch_mesh_.fetch_add(1ull, std::memory_order_relaxed);
        if (bounds_clamped) execute_indirect_bounds_clamped_.fetch_add(1ull, std::memory_order_relaxed);
        if (rejected) execute_indirect_rejected_.fetch_add(1ull, std::memory_order_relaxed);
        if (va_translated) execute_indirect_va_translated_.fetch_add(1ull, std::memory_order_relaxed);
        if (vulkan_routed) execute_indirect_vulkan_routed_.fetch_add(1ull, std::memory_order_relaxed);
    }
    bool HasActiveGuestVaTranslation() const noexcept {
        return va_translator_hook_.load(std::memory_order_acquire) != nullptr ||
               mirrored_va_count_.load(std::memory_order_acquire) > 0u ||
               xmem_allocation_count_.load(std::memory_order_acquire) > 0u;
    }
    const openscarlett::SanitizedIhvCapabilities& SanitizedIhvCaps() const noexcept {
        return sanitized_ihv_caps_;
    }
    void TrackCommandListTransientResource(ID3D12Resource* res) { TrackTransientGpuResource(res); }

    // Sprint 15: Live MSAA quality clamping, console flag stripping, and parity telemetry hooks.
    bool ClampMsaaSampleDescForFormat(DXGI_FORMAT format, DXGI_SAMPLE_DESC& sample_desc) noexcept;
    D3D12_RESOURCE_DESC SanitizeAndClampResourceDesc(const D3D12_RESOURCE_DESC& desc) noexcept;
    D3D12_RESOURCE_DESC1 SanitizeAndClampResourceDesc1(const D3D12_RESOURCE_DESC1& desc) noexcept;
    void RecordGlobalUavBarrier() noexcept {
        global_uav_barriers_recorded_.fetch_add(1ull, std::memory_order_relaxed);
    }
    void RecordUnsupportedCmdListExtensionIgnored() noexcept {
        unsupported_cmdlist_extensions_ignored_.fetch_add(1ull, std::memory_order_relaxed);
    }
    void RecordZeroCountNoop() noexcept {
        zero_count_noop_calls_.fetch_add(1ull, std::memory_order_relaxed);
    }
    void RecordDepthStencilPlaneCopy() noexcept {
        depth_stencil_plane_copies_.fetch_add(1ull, std::memory_order_relaxed);
    }
    void RecordFootprintOffsetCopy() noexcept {
        footprint_offset_copies_.fetch_add(1ull, std::memory_order_relaxed);
    }
    void RecordDisplayNotificationsSignaled(uint64_t vlines, uint64_t frames) noexcept {
        if (vlines > 0ull) vline_notifications_signaled_.fetch_add(vlines, std::memory_order_relaxed);
        if (frames > 0ull) frame_notifications_signaled_.fetch_add(frames, std::memory_order_relaxed);
    }

    void NotifyDescriptorHeapDestroyed(ID3D12DescriptorHeap* host_heap) noexcept;
    void NotifyRootSignatureDestroyed(ID3D12RootSignature* host_rs) noexcept;
    void NotifyGpuWrittenTexture(ID3D12Resource* host_resource) noexcept;
    void NotifyGpuWrittenBufferRange(
        ID3D12Resource* host_resource, uint64_t offset, uint64_t size_bytes) noexcept;
    void NotifyGpuWrittenBuffer(ID3D12Resource* host_resource) noexcept;
    bool IsGpuWrittenBufferRange(
        ID3D12Resource* host_resource, uint64_t offset, uint64_t size_bytes) const noexcept;
    bool IsGpuWrittenBuffer(ID3D12Resource* host_resource) const noexcept;
    void RecordPsoVertexLayout(
        ID3D12PipelineState* host_pso,
        const D3D12_INPUT_ELEMENT_DESC* elems,
        UINT num_elems) noexcept;
    bool LookupPsoVertexLayout(
        ID3D12PipelineState* host_pso,
        uint32_t& out_pos_off,
        uint32_t& out_pos_comps,
        uint32_t& out_non_float_dword_mask) const noexcept;
    bool PsoUsesZeroIaInputs(ID3D12PipelineState* host_pso) const noexcept;
    void RegisterGuestBackedDescHeap(
        ID3D12DescriptorHeap* host_heap,
        uint64_t host_gpu_start, uint64_t host_cpu_start, uint32_t host_num_descs,
        uint32_t inc_size, uint64_t guest_cpu_start, uint64_t guest_byte_size,
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE MaterializeGuestDescriptorTable(
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
        ID3D12RootSignature* bound_host_rs,
        UINT root_param_index,
        uint32_t* out_copied_hits = nullptr,
        uint32_t* out_slot_count = nullptr,
        bool is_compute = false);
    void RecordDiagDescriptor(uint64_t cpu_ptr, std::string desc);
    void CopyDiagDescriptor(uint64_t dst_cpu_ptr, uint64_t src_cpu_ptr);
    std::string DescribeDiagDescriptorTable(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle, uint32_t max_slots);
    std::string DescribeDiagRtv(uint64_t cpu_ptr);
    void RecordDiagRootSig(void* host_rs, std::string desc);
    std::string DescribeDiagRootSig(void* host_rs);
    void CheckCpuDeviceRemoved(
        const char* api_name, uint64_t detail0 = 0ull, uint64_t detail1 = 0ull) noexcept;

private:
    struct TrackedBufferVa {
        uint64_t gpu_va = 0ull;
        uint64_t size_bytes = 0ull;
        uint64_t guest_va = 0ull;
        const uint8_t* mapped_cpu_ptr = nullptr;
        bool owns_persistent_map = false;
    };

    void TrackBufferVaResource(
        ID3D12Resource* host_res,
        const D3D12_RESOURCE_DESC& desc,
        uint64_t guest_va = 0ull);
    void TrackPlacedTextureVa(
        ID3D12Resource* host_res,
        uint64_t guest_va);
    ID3D12Resource* ResolvePlacedTextureByGpuVa(
        uint64_t guest_va,
        const D3D12_RESOURCE_DESC* expected_desc) const;
    HRESULT EnsureFeedbackCodecPipelinesLocked();
    void TrackTransientGpuResource(ID3D12Resource* res);
    void ReleaseAaaRuntimeObjects();
    void TrackVulkanResource(
        IUnknown* obj, D3D12_HEAP_TYPE heap_type, const D3D12_RESOURCE_DESC& desc);
    bool ShouldRewriteR9G9B9E5Uav(const D3D12_RESOURCE_DESC& desc) const noexcept;
    void TrackFallbackResource(IUnknown* obj, bool is_fallback);
    bool IsTrackedFallbackResource(ID3D12Resource* resource) const noexcept;
    HRESULT EnsureUnifiedUploadObjectsLocked();
    HRESULT AcquireUnifiedUploadRingSlotLocked(
        uint64_t required_bytes,
        uint32_t& out_slot_index);
    HRESULT FlushDirtyUnifiedTexturesInternal(
        ID3D12CommandQueue* queue,
        uint32_t* out_flushed_count,
        bool wait_for_gpu);
    HRESULT EnsureInternalDirectSyncQueueLocked();
    void ReleaseUnifiedMemoryObjects();
    void AutoRegisterUnifiedResource(
        ID3D12Resource* resource,
        uint64_t guest_va,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initial_state,
        bool mirrored_by_title_va = false,
        bool use_footprint_row_pitch = false);
    ID3D12RootSignature* SynthesizeHostRootSignatureFromXboxDesc(
        const openscarlett::XboxRootSignatureDesc& desc);
    void SyncXboxContainerSgprMapToBridge(const openscarlett::XboxParsedShaderContainer& parsed);
    bool LiftXboxStageToHostBytecode(
        const void* data,
        size_t size,
        bool mesh_has_payload,
        bool prefer_dxil_standin,
        const openscarlett::XboxParsedShaderContainer* paired_other_stage,
        std::vector<uint8_t>& out_bytecode,
        openscarlett::XboxParsedShaderContainer* out_parsed = nullptr);
    bool LiftXboxStageToSpirv(
        const void* data,
        size_t size,
        bool mesh_has_payload,
        const openscarlett::XboxParsedShaderContainer* paired_other_stage,
        std::vector<uint32_t>& out_spirv,
        openscarlett::XboxParsedShaderContainer* out_parsed = nullptr);

    friend class DeviceExtendedProxy;
    friend class DxgiDeviceProxy;

    std::atomic<ULONG> ref_count_{1};
    ID3D12Device* host_dev_ = nullptr;
    ID3D12Device1* host_dev1_ = nullptr;
    ID3D12Device2* host_dev2_ = nullptr;
    ID3D12Device3* host_dev3_ = nullptr;
    ID3D12Device4* host_dev4_ = nullptr;
    ID3D12Device5* host_dev5_ = nullptr;
    ID3D12Device6* host_dev6_ = nullptr;
    ID3D12Device7* host_dev7_ = nullptr;
    ID3D12Device8* host_dev8_ = nullptr;
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    ID3D12Device9* host_dev9_ = nullptr;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    ID3D12Device10* host_dev10_ = nullptr;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    ID3D12Device11* host_dev11_ = nullptr;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    ID3D12Device12* host_dev12_ = nullptr;
#endif
    DeviceExtendedProxy* ext_proxy_ = nullptr;
    DxgiDeviceProxy* dxgi_proxy_ = nullptr;
    D3D12AdapterInfo adapter_info_{};
    HWND default_hwnd_ = nullptr;
    bool supports_r9g9b9e5_uav_ = false;
    bool supports_depth_bounds_test_ = false;
    bool supports_view_instancing_ = false;
    bool supports_programmable_sample_positions_ = false;
    std::atomic<uint32_t> frame_interval_us_{16667u};
    std::atomic<uint32_t> max_frame_latency_{2u};
    std::atomic<uint64_t> next_frame_token_{1ull};
    std::atomic<bool> suspended_{false};
    static constexpr size_t kShaderLiftLockStripes = 64u;
    std::array<std::mutex, kShaderLiftLockStripes> shader_lift_stripes_;
    openscarlett::ShaderCache shader_cache_;
    mutable std::shared_mutex xbox_container_cache_mutex_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> xbox_container_dxil_cache_;
    std::mutex synth_rs_mutex_;
    std::vector<ID3D12RootSignature*> synthesized_root_sigs_;
    std::atomic<uint64_t> xbox_container_lifts_{0ull};
    std::atomic<uint64_t> rayquery_bridge_lifts_{0ull};
    std::atomic<uint64_t> mesh_inferred_root_lifts_{0ull};
    mutable std::mutex window_mutex_;

    std::atomic<GuestGpuVaTranslatorFn> va_translator_hook_{nullptr};
    std::atomic<void*> va_translator_ctx_{nullptr};
    std::atomic<DescriptorCreateHookFn> descriptor_create_hook_{nullptr};
    std::atomic<void*> descriptor_create_ctx_{nullptr};
    openscarlett::TitleVAMirror title_va_mirror_;
    openscarlett::DescriptorSgprBridge descriptor_bridge_;
    SamplerFeedbackManager sampler_feedback_mgr_;
    QueryAccumulatorX query_accumulator_;
    std::unique_ptr<openscarlett::XgGpuUntiler> xg_untiler_;
    std::mutex xg_untiler_mutex_;
    bool xg_untiler_attempted_ = false;

    mutable std::shared_mutex buffer_va_mutex_;
    std::unordered_map<ID3D12Resource*, TrackedBufferVa> buffer_va_map_;
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, ID3D12Resource*> buffer_va_by_gpu_;
    std::atomic<uint64_t> buffer_va_gen_{1ull};
    mutable std::mutex tile_mode_mutex_;
    std::unordered_map<ID3D12Resource*, XG_TILE_MODE> resource_tile_modes_;
    std::atomic<uint32_t> tile_mode_count_{0u};
    mutable std::mutex mirrored_res_mutex_;
    std::unordered_map<ID3D12Resource*, uint64_t> mirrored_res_to_va_;
    std::atomic<uint32_t> mirrored_va_count_{0u};
    std::atomic<HRESULT> device_removed_reason_{S_OK};

    std::mutex feedback_codec_mutex_;
    ID3D12RootSignature* fb_codec_rs_ = nullptr;
    ID3D12PipelineState* fb_decode_pso_ = nullptr;
    ID3D12PipelineState* fb_encode_pso_ = nullptr;

    std::mutex transient_res_mutex_;
    std::vector<ID3D12Resource*> transient_gpu_resources_;

    // Sprint 12 live wiring counters
    std::atomic<uint64_t> sf_decode_intercepts_{0ull};
    std::atomic<uint64_t> sf_encode_intercepts_{0ull};
    std::atomic<uint64_t> qa_cl_begin_count_{0ull};
    std::atomic<uint64_t> qa_cl_end_count_{0ull};
    std::atomic<uint64_t> qa_cl_resolve_count_{0ull};
    std::atomic<uint64_t> va_queue_flushes_{0ull};
    std::atomic<uint64_t> va_indirect_flushes_{0ull};
    std::atomic<uint64_t> db_heap_binds_{0ull};
    std::atomic<uint64_t> db_table_binds_{0ull};
    std::atomic<uint64_t> db_root_const_updates_{0ull};
    std::atomic<uint64_t> db_root_cbv_updates_{0ull};
    std::atomic<uint64_t> xg_gpu_untile_dispatches_{0ull};
    std::atomic<uint64_t> xg_gpu_retile_dispatches_{0ull};
    std::atomic<uint64_t> xg_cpu_fallbacks_{0ull};
    std::atomic<uint64_t> cmdlist_kickoffs_{0ull};
    std::atomic<uint64_t> cmdlist_write32_calls_{0ull};
    std::atomic<uint64_t> cmdlist_predicationx_calls_{0ull};
    std::atomic<uint64_t> cmdlist_flush_cache_calls_{0ull};
    std::atomic<uint64_t> cmdlist_dispatch_mesh_calls_{0ull};
    std::atomic<uint32_t> last_mesh_x_{0u};
    std::atomic<uint32_t> last_mesh_y_{0u};
    std::atomic<uint32_t> last_mesh_z_{0u};
    std::atomic<uint64_t> total_mesh_groups_{0ull};

    mutable std::mutex fallback_mutex_;
    std::unordered_set<ID3D12Resource*> r9g9b9e5_fallback_resources_;

    // Title unified-memory texture remap tracking (ivall/xcl fd74662/bdae5a4)
    mutable std::mutex unified_mutex_;
    std::unordered_map<ID3D12Resource*, UnifiedTextureMapping> unified_textures_;
    std::unordered_map<ID3D12Heap*, uint64_t> address_heaps_;
    std::vector<XMemAllocationRecord> xmem_allocations_;
    std::atomic<uint32_t> xmem_allocation_count_{0u};
    std::atomic<uint32_t> registered_unified_count_{0u};
    std::atomic<uint32_t> dirty_unified_count_{0u};
    uint64_t total_remap_notifications_{0ull};
    uint64_t total_unified_flushes_{0ull};
    uint64_t total_unified_bytes_uploaded_{0ull};

    static constexpr uint32_t kUnifiedUploadRingSlots = 8u;
    struct UnifiedUploadRingSlot {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        ID3D12Resource* upload_buf = nullptr;
        uint8_t* mapped_ptr = nullptr;
        uint64_t capacity = 0ull;
        uint64_t fence_val = 0ull;
    };
    std::mutex unified_upload_mutex_;
    std::array<UnifiedUploadRingSlot, kUnifiedUploadRingSlots> unified_upload_ring_{};
    uint32_t unified_upload_ring_cursor_ = 0u;
    ID3D12Fence* unified_fence_ = nullptr;
    HANDLE unified_fence_event_ = nullptr;
    uint64_t unified_fence_val_ = 0ull;
    ID3D12CommandQueue* unified_sync_direct_queue_ = nullptr;
    std::atomic<uint64_t> committed_non_rt_vram_bytes_{0ull};
    std::atomic<uint64_t> committed_rt_ds_vram_bytes_{0ull};

    // Direct d3d12_xs -> Vulkan 1.4 routing state (active when OPENSCARLETT_BACKEND=vulkan)
    bool vulkan_routing_active_ = false;
    std::unique_ptr<openscarlett::VulkanBackend> vulkan_backend_;
    mutable std::mutex vk_state_mutex_;
    std::unordered_map<ID3D12PipelineState*, VulkanPipelineInfo> vk_pipelines_;
    std::unordered_map<SIZE_T, ID3D12Resource*> vk_rtv_map_;
    mutable std::mutex valid_view_mutex_;
    std::unordered_map<SIZE_T, ID3D12Resource*> valid_rtv_handles_;
    std::unordered_map<SIZE_T, ID3D12Resource*> valid_dsv_handles_;
    std::unordered_map<SIZE_T, DXGI_FORMAT> valid_rtv_formats_;
    std::unordered_map<SIZE_T, DXGI_FORMAT> valid_dsv_formats_;
    std::unordered_set<SIZE_T> readonly_dsv_handles_;
    std::unordered_map<SIZE_T, ID3D12Resource*> vk_dsv_map_;
    std::unordered_map<ID3D12Resource*, VulkanTrackedResource> vk_resources_;
    std::unordered_map<ID3D12Resource*, openscarlett::FrameBufferRgba8> vk_rt_framebuffers_;
    std::atomic<uint64_t> vk_routed_cmd_lists_{0ull};
    std::atomic<uint64_t> vk_routed_draws_{0ull};
    std::atomic<uint64_t> vk_routed_indexed_draws_{0ull};
    std::atomic<uint64_t> vk_routed_dispatches_{0ull};
    std::atomic<uint64_t> vk_routed_mesh_dispatches_{0ull};
    std::atomic<uint64_t> vk_routed_copies_{0ull};
    std::atomic<uint64_t> vk_routed_barriers_{0ull};
    std::atomic<uint64_t> vk_routed_rtv_clears_{0ull};
    std::atomic<uint64_t> vk_routed_dsv_clears_{0ull};
    std::atomic<uint64_t> vk_routed_presents_{0ull};
    std::atomic<uint64_t> vk_icd_executions_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_draws_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_dispatches_{0ull};
    std::atomic<uint64_t> vk_spirv_direct_mesh_dispatches_{0ull};

    // Sprint 14: Live VRAM Residency Manager + Sparse VRAM Window-0 Isolation + PSO Diagnostics
    struct LiveResidencyEntry {
        ID3D12Pageable* host_obj = nullptr;
        uint64_t size_bytes = 0ull;
        uint64_t sparse_window_va = 0ull;
        D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_NORMAL;
        uint32_t residency_ref_count = 1u;
        uint32_t active_pin_count = 0u;
        uint32_t placed_child_count = 0u;
        ID3D12Pageable* parent_heap = nullptr;
        ID3D12Fence* in_flight_fence = nullptr;
        uint64_t in_flight_fence_value = 0ull;
        bool is_heap = false;
        bool is_resident = true;
        bool is_placed_suballocation = false;
        bool spilled_to_l0 = false;
        bool in_lru = false;
        std::list<ID3D12Pageable*>::iterator lru_it;
    };

    uint64_t EstimateResourceVramBytes(const D3D12_RESOURCE_DESC& desc) const noexcept;
    void PrepareResidencyBudgetForAllocation(
        uint64_t requested_bytes,
        bool allow_l0_spill,
        bool& out_should_spill_l0);
    void TrackLiveResidencyObject(
        ID3D12Pageable* host_obj,
        uint64_t size_bytes,
        D3D12_HEAP_TYPE heap_type,
        bool is_placed_suballocation,
        bool spilled_to_l0,
        uint64_t sparse_window_va = 0ull,
        bool is_heap = false,
        ID3D12Pageable* parent_heap = nullptr);
    void UntrackLiveResidencyObject(ID3D12Pageable* host_obj) noexcept;
    void RefreshDxgiVideoMemoryInfoLocked() noexcept;
    uint64_t EffectiveResidencyBudgetBytesLocked() const noexcept;
    uint64_t EvictLruVictimsLocked(uint64_t bytes_needed, ID3D12Pageable* protect_obj) noexcept;
    bool IsResidencyEntryLiveProtectedLocked(const LiveResidencyEntry& e) const noexcept;

    // Sprint 16: Page-at-a-Time Console Descriptor Heap Manager (xcl #84)
    struct DescriptorPageSlot {
        uint32_t page_index = 0u;
        uint64_t generation = 1ull;
        bool in_flight = false;
        ID3D12DescriptorHeap* isolated_shadow_heap = nullptr;
    };
    struct PagedDescriptorHeapState {
        ID3D12DescriptorHeap* primary_host_heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{0u};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{0ull};
        UINT inc_size = 32u;
        UINT num_descriptors = 0u;
        UINT descriptors_per_page = 2048u;
        std::vector<DescriptorPageSlot> pages;
    };

    // Sprint 16: Unbounded Sampler Heap Virtualization (> 2048 -> 2048 deduplicated host heap)
    struct SamplerDescKey {
        uint8_t bytes[sizeof(D3D12_SAMPLER_DESC)]{};
        bool operator==(const SamplerDescKey& o) const noexcept {
            return std::memcmp(bytes, o.bytes, sizeof(bytes)) == 0;
        }
    };
    struct SamplerDescKeyHash {
        size_t operator()(const SamplerDescKey& k) const noexcept {
            uint64_t h = 14695981039346656037ull;
            for (uint8_t b : k.bytes) {
                h ^= static_cast<uint64_t>(b);
                h *= 1099511628211ull;
            }
            return static_cast<size_t>(h);
        }
    };
    struct VirtualSamplerHeapState {
        ID3D12DescriptorHeap* host_heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC virtual_desc{};
        D3D12_CPU_DESCRIPTOR_HANDLE virt_cpu_start{0u};
        D3D12_GPU_DESCRIPTOR_HANDLE virt_gpu_start{0ull};
        D3D12_CPU_DESCRIPTOR_HANDLE host_cpu_start{0u};
        D3D12_GPU_DESCRIPTOR_HANDLE host_gpu_start{0ull};
        UINT inc_size = 32u;
        UINT host_capacity = 2048u;
        UINT next_host_slot = 0u;
        std::vector<uint16_t> virt_to_host_slot;
        std::vector<SamplerDescKey> virt_slot_keys;
        std::vector<uint8_t> virt_slot_valid;
        std::unordered_map<SamplerDescKey, uint16_t, SamplerDescKeyHash> dedup_map;
    };

    static SamplerDescKey CanonicalizeSamplerDesc(const D3D12_SAMPLER_DESC& desc) noexcept;
    bool TryHandleVirtualSamplerCreate(
        const D3D12_SAMPLER_DESC* pDesc,
        D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) noexcept;
    bool TryHandleVirtualSamplerCopy(
        UINT NumDestDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
        const UINT* pDestDescriptorRangeSizes,
        UINT NumSrcDescriptorRanges,
        const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts,
        const UINT* pSrcDescriptorRangeSizes) noexcept;
    void TrackPagedDescriptorWrite(D3D12_CPU_DESCRIPTOR_HANDLE dest_cpu) noexcept;

    openscarlett::SanitizedIhvCapabilities sanitized_ihv_caps_{};
    mutable std::mutex paged_desc_mutex_;
    std::unordered_map<ID3D12DescriptorHeap*, PagedDescriptorHeapState> paged_desc_heaps_;
    mutable std::mutex virt_sampler_mutex_;
    std::unordered_map<ID3D12DescriptorHeap*, VirtualSamplerHeapState> virtual_sampler_heaps_;
    uint64_t next_virtual_sampler_cpu_base_ = 0x7F00000000000000ull;
    uint64_t next_virtual_sampler_gpu_base_ = 0x7E00000000000000ull;

    std::atomic<uint64_t> paged_descriptor_heaps_created_{0ull};
    std::atomic<uint64_t> descriptor_pages_allocated_{0ull};
    std::atomic<uint64_t> descriptor_page_rotations_{0ull};
    std::atomic<uint64_t> descriptor_page_generations_advanced_{0ull};
    std::atomic<uint64_t> unbounded_sampler_heaps_created_{0ull};
    std::atomic<uint64_t> unbounded_sampler_writes_{0ull};
    std::atomic<uint64_t> unbounded_sampler_dedup_hits_{0ull};
    std::atomic<uint64_t> unbounded_sampler_unique_host_slots_{0ull};
    std::atomic<uint64_t> unbounded_sampler_table_translations_{0ull};
    std::atomic<uint64_t> dedicated_copy_queues_created_{0ull};
    std::atomic<uint64_t> copy_queue_barriers_sanitized_{0ull};
    std::atomic<uint64_t> copy_queue_uav_barriers_suppressed_{0ull};
    std::atomic<uint64_t> execute_indirect_calls_{0ull};
    std::atomic<uint64_t> execute_indirect_with_count_buffer_{0ull};
    std::atomic<uint64_t> execute_indirect_with_root_constants_{0ull};
    std::atomic<uint64_t> execute_indirect_dispatch_mesh_{0ull};
    std::atomic<uint64_t> execute_indirect_bounds_clamped_{0ull};
    std::atomic<uint64_t> execute_indirect_rejected_{0ull};
    std::atomic<uint64_t> execute_indirect_va_translated_{0ull};
    std::atomic<uint64_t> execute_indirect_vulkan_routed_{0ull};
    std::atomic<uint64_t> gpu_fmask_resolves_{0ull};
    std::atomic<uint64_t> gpu_dcc_decompresses_{0ull};
    std::atomic<uint64_t> gpu_htile_decompresses_{0ull};
    uint64_t skipped_active_live_heaps_ = 0ull;

    mutable std::mutex residency_mutex_;
    openscarlett::XboxConsoleProfileSelection console_profile_{};
    openscarlett::HostAdapterVideoMemoryInfo dxgi_mem_info_{};
    openscarlett::SparseVramWindowIsolator* sparse_window_iso_ = nullptr;
    std::unordered_map<ID3D12Pageable*, LiveResidencyEntry> residency_entries_;
    std::list<ID3D12Pageable*> residency_lru_;  // Front = MRU, Back = LRU
    std::unordered_map<ID3D12DescriptorHeap*, uint64_t> desc_heap_window0_vas_;
    uint64_t residency_budget_override_bytes_ = 0ull;
    uint64_t tracked_resident_bytes_ = 0ull;
    uint64_t tracked_evicted_bytes_ = 0ull;
    uint64_t l0_spilled_bytes_ = 0ull;
    uint64_t l0_spill_count_ = 0ull;
    uint64_t auto_lru_evict_count_ = 0ull;
    uint64_t make_resident_calls_ = 0ull;
    uint64_t evict_calls_ = 0ull;
    uint64_t set_priority_calls_ = 0ull;
    uint64_t enqueue_make_resident_calls_ = 0ull;

    // Sprint 14: Xbox Series PSO Stream & Linkage diagnostic counters
    std::atomic<uint64_t> pso_remapped_as_tags_{0ull};
    std::atomic<uint64_t> pso_remapped_ms_tags_{0ull};
    std::atomic<uint64_t> pso_remapped_ext_tags_{0ull};
    std::atomic<uint64_t> pso_dropped_legacy_stages_{0ull};
    std::atomic<uint64_t> pso_inferred_mesh_topologies_{0ull};
    std::atomic<uint64_t> pso_f16_links_reconciled_{0ull};
    std::atomic<uint64_t> pso_interp_validations_{0ull};
    std::atomic<uint64_t> pso_interp_matches_{0ull};
    bool retail_scarlett_abi_ = false;

    // Sprint 15: D3D12X / Xbox Graphics Parity diagnostic counters
    std::atomic<uint64_t> promoted_oversized_placed_resources_{0ull};
    std::atomic<uint64_t> msaa_quality_clamps_{0ull};
    std::atomic<uint64_t> console_flags_stripped_{0ull};
    std::atomic<uint64_t> reserved_cross_adapter_preserved_{0ull};
    std::atomic<uint64_t> reserved_cross_adapter_stripped_fallback_{0ull};
    std::atomic<uint64_t> global_uav_barriers_recorded_{0ull};
    std::atomic<uint64_t> unsupported_cmdlist_extensions_ignored_{0ull};
    std::atomic<uint64_t> zero_count_noop_calls_{0ull};
    std::atomic<uint64_t> readonly_dsv_creations_{0ull};
    std::atomic<uint64_t> depth_stencil_plane_copies_{0ull};
    std::atomic<uint64_t> footprint_offset_copies_{0ull};
    std::atomic<uint64_t> vline_notifications_signaled_{0ull};
    std::atomic<uint64_t> frame_notifications_signaled_{0ull};

    // Lock-free 256-byte CBV alignment ring buffer for Xbox 16-byte-aligned root CBVs on desktop D3D12.
    std::mutex cbv_align_mutex_;
    ID3D12Resource* cbv_align_ring_ = nullptr;
    std::atomic<uint8_t*> cbv_align_ring_cpu_{nullptr};
    std::atomic<D3D12_GPU_VIRTUAL_ADDRESS> cbv_align_ring_gpu_{0ull};
    std::atomic<uint32_t> cbv_align_ring_cursor_{0u};

    // 64 MiB persistently-mapped UPLOAD ring for unmapped retail Xbox CPU/TitleVA VB/IB/CBV buffers
    // (e.g. Scaleform UI dynamic vertex/index/constant buffers allocated directly in CPU heap).
    struct TransientRingSlice {
        uint64_t raw_start = 0ull;
        uint64_t byte_len = 0ull;
        uint64_t content_hash = 0ull;
        D3D12_GPU_VIRTUAL_ADDRESS host_gpu_start = 0ull;
        uint64_t frame_token = 0ull;
    };
    std::mutex transient_ring_mutex_;
    ID3D12Resource* transient_ring_ = nullptr;
    uint8_t* transient_ring_cpu_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS transient_ring_gpu_ = 0ull;
    uint64_t transient_ring_cursor_ = 0ull;
    std::array<TransientRingSlice, 64u> transient_ring_cache_{};
    uint32_t transient_ring_cache_next_ = 0u;

    // VA-bucketed aliasing heaps for CreatePlacedResourceX textures so Xbox transient
    // render-graph resources placed at overlapping physical VAs share host VRAM (zero bloat).
    std::mutex placed_alias_mutex_;
    std::unordered_map<uint64_t, ID3D12Heap*> placed_alias_heaps_;

    // Guest VA -> placed ID3D12Resource* map for CreatePlacedShaderResourceViewX (Slot 70)
    // so retail single-plane 4K UI / render-target SRVs resolve to live GPU textures.
    mutable std::mutex placed_tex_va_mutex_;
    std::unordered_map<ID3D12Resource*, uint64_t> placed_tex_res_to_va_;
    std::unordered_map<uint64_t, std::vector<ID3D12Resource*>> placed_tex_by_va_;

    // Guest-backed Scarlett shader-visible descriptor heap mirror (for oversized >1M console heaps).
    // Retail titles memcpy 32-byte descriptors from CPU staging heaps directly into guest_cpu_start
    // instead of calling ID3D12Device::CopyDescriptors; we shadow created staging descriptors and
    // materialize each bound descriptor table into the real host shader-visible heap via
    // spec-legal CopyDescriptorsSimple (also folding virtual slots >= host_num_descs cleanly in-bounds).
    struct Desc32Key {
        uint8_t bytes[32]{};
        bool operator==(const Desc32Key& o) const noexcept {
            return std::memcmp(bytes, o.bytes, 32u) == 0;
        }
    };
    struct Desc32KeyHash {
        size_t operator()(const Desc32Key& k) const noexcept {
            uint64_t h = 14695981039346656037ull;
            for (size_t i = 0u; i < 4u; ++i) {
                uint64_t w = 0ull;
                std::memcpy(&w, k.bytes + i * 8u, 8u);
                h ^= w;
                h *= 1099511628211ull;
            }
            return static_cast<size_t>(h);
        }
    };
    struct GuestBackedDescHeap {
        ID3D12DescriptorHeap* host_heap = nullptr;
        uint64_t host_gpu_start = 0ull;
        uint64_t host_cpu_start = 0ull;
        uint32_t host_num_descs = 0u;
        uint32_t inc_size = 32u;
        uint64_t guest_cpu_start = 0ull;
        uint64_t guest_byte_size = 0ull;
        uint32_t ring_cursor = 0u;
        bool ring_wrapped = false;
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    };
    struct ShadowCbvSource {
        uint64_t guest_va = 0ull;
        uint32_t aligned_size = 0u;
    };
    struct ShadowBufSrvSource {
        ID3D12Resource* host_res = nullptr;
        uint64_t guest_cpu_va = 0ull;
        uint64_t host_byte_off = 0ull;
        uint64_t byte_size = 0ull;
        uint32_t stride = 0u;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        D3D12_BUFFER_SRV_FLAGS flags = D3D12_BUFFER_SRV_FLAG_NONE;
    };
    struct RsS2MirrorInfo {
        uint32_t cbv_off = 0u;
        uint32_t uav_off = 0u;
        uint32_t native_slots = 16u;
    };
    bool EnsureShadowDescHeapLocked() noexcept;
    uint32_t AllocShadowSlotLocked(bool prefer_static_partition) noexcept;
    bool RedirectGuestDescDestIfNeeded(
        D3D12_CPU_DESCRIPTOR_HANDLE& dst, uint64_t& out_guest_writeback_addr,
        bool prefer_static_partition = false);
    void SnapshotCreatedDescriptorToShadow(
        D3D12_CPU_DESCRIPTOR_HANDLE created_cpu_handle, uint64_t guest_writeback_addr = 0ull,
        bool prefer_static_partition = false, uint64_t* out_shadow_ptr = nullptr);
    void RegisterShadowSrvUavPair(
        uint64_t shadow_ptr, ID3D12Resource* host_res, bool created_is_uav, DXGI_FORMAT view_fmt);
    bool TryCopyGuestBackedDescriptorsSimple(
        UINT num_descs,
        D3D12_CPU_DESCRIPTOR_HANDLE dst_start,
        D3D12_CPU_DESCRIPTOR_HANDLE src_start,
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type);
    D3D12_CPU_DESCRIPTOR_HANDLE TranslateGuestSamplerCpuHandle(
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        ID3D12DescriptorHeap** out_shadow_src = nullptr,
        D3D12_CPU_DESCRIPTOR_HANDLE* out_shadow_slot = nullptr) noexcept;
    bool HostFormatSupportsTypedUav(DXGI_FORMAT fmt) const noexcept;
    void RecordRootSigTableSizes(void* host_rs, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc);

    std::atomic<bool> has_guest_backed_desc_heaps_{false};
    mutable std::mutex guest_dh_mutex_;
    std::vector<GuestBackedDescHeap> guest_backed_desc_heaps_;
    ID3D12DescriptorHeap* shadow_cbv_srv_uav_heap_ = nullptr;
    ID3D12DescriptorHeap* shadow_sampler_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_sampler_cpu_start_{0u};
    uint32_t shadow_sampler_inc_ = 32u;
    uint32_t shadow_sampler_cap_ = 0u;
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_cbv_srv_uav_cpu_start_{0u};
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_null_srv_cpu_{0u};
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_null_cbv_cpu_{0u};
    D3D12_CPU_DESCRIPTOR_HANDLE shadow_null_uav_cpu_{0u};
    uint32_t shadow_cbv_srv_uav_inc_ = 32u;
    uint32_t shadow_cbv_srv_uav_cap_ = 0u;
    uint32_t shadow_static_cursor_ = 3u;   // slots 0..2 reserved for null SRV / CBV / UAV
    uint32_t shadow_dynamic_cursor_ = 0u;
    bool IsShaderVisibleHostDescHandle(
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        D3D12_DESCRIPTOR_HEAP_TYPE heap_type) const noexcept;
    std::vector<Desc32Key> shadow_slot_keys_;
    std::vector<uint8_t> shadow_slot_valid_;
    std::unordered_map<Desc32Key, uint64_t, Desc32KeyHash> desc32_to_shadow_cpu_;
    std::unordered_map<uint64_t, uint64_t> sv_cpu_to_shadow_cpu_;
    std::unordered_map<uint64_t, ShadowCbvSource> shadow_cbv_sources_;
    std::unordered_map<uint64_t, ShadowCbvSource> staging_cbv_sources_;
    std::unordered_set<uint64_t> cbv_shadow_ptrs_;
    std::unordered_map<uint64_t, ShadowBufSrvSource> shadow_buf_srv_sources_;
    std::unordered_map<uint64_t, ShadowBufSrvSource> staging_buf_srv_sources_;
    std::unordered_set<uint64_t> uav_shadow_ptrs_;
    std::unordered_map<uint64_t, uint64_t> srv_to_uav_shadow_;
    std::unordered_map<uint64_t, uint64_t> uav_to_srv_shadow_;
    std::unordered_map<uint64_t, ID3D12Resource*> shadow_to_tex_res_;
    std::unordered_map<void*, std::vector<uint32_t>> rs_table_slot_counts_;
    std::unordered_map<void*, std::vector<RsS2MirrorInfo>> rs_s2_mirrors_;

    // Diagnostic-only (OPENSCARLETT_TRACE_D3D12_LOG) descriptor & root signature inspector.
    mutable std::mutex diag_meta_mutex_;
    std::unordered_map<uint64_t, std::string> diag_desc_map_;
    std::unordered_map<void*, std::string> diag_rs_map_;
    std::atomic<ID3D12Resource*> last_dii_rtv_{nullptr};
    std::atomic<uint32_t> lazy_lift_count_{0u};
    mutable std::mutex retail_state_mutex_;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> retail_res_states_;
    struct DeferredResourceRelease {
        ID3D12Resource* resource = nullptr;
        uint64_t retire_frame = 0ull;
    };
    std::atomic<uint64_t> present_frame_count_{0ull};
    std::mutex deferred_release_mutex_;
    std::deque<DeferredResourceRelease> deferred_release_queue_;
    std::mutex dummy_root_uav_mutex_;
    ID3D12Resource* dummy_root_uav_buf_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS dummy_root_uav_gpu_ = 0ull;
    mutable std::mutex gpu_written_buf_mutex_;
    std::unordered_map<ID3D12Resource*, std::vector<std::pair<uint64_t, uint64_t>>> gpu_written_buffer_ranges_;
};

}  // namespace openscarlett::d3d12_xs
