#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/vulkan_backend.h"

namespace openscarlett::d3d12_xs {

class DeviceX;
struct WindowCallbackState;
struct VulkanRecordedBatch;

// Sprint 14: Lock-safe FenceWaitDispatcher COM interface (Leonida #1933).
// Dispatches fence completion callbacks strictly OUTSIDE the dispatcher mutex
// (preventing re-entrant deadlocks) and wakes all blocked waits cleanly on device removal.
// {4F534431-3258-4657-4400-000000000031}
inline constexpr GUID IID_IOpenScarlettFenceWaitDispatcher = {
    0x4F534431u, 0x3258u, 0x4657u,
    {0x44u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x31u}
};

using FenceCompletionCallbackFn = void (*)(
    ID3D12Fence* fence,
    UINT64 target_value,
    HRESULT completion_status,
    void* user_ctx);

struct IOpenScarlettFenceWaitDispatcher : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterFenceCallback(
        ID3D12Fence* fence,
        UINT64 target_value,
        FenceCompletionCallbackFn callback,
        void* user_ctx,
        uint64_t* out_registration_id) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForFenceValue(
        ID3D12Fence* fence,
        UINT64 target_value,
        UINT timeout_ms) = 0;
    virtual HRESULT STDMETHODCALLTYPE PumpFenceCallbacks(
        uint32_t* out_dispatched_count) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyDeviceRemoved(
        HRESULT removed_reason) = 0;
};

class FenceWaitDispatcher final {
public:
    explicit FenceWaitDispatcher(DeviceX* parent_device);
    ~FenceWaitDispatcher();

    FenceWaitDispatcher(const FenceWaitDispatcher&) = delete;
    FenceWaitDispatcher& operator=(const FenceWaitDispatcher&) = delete;

    HRESULT RegisterCallback(
        ID3D12Fence* fence,
        UINT64 target_value,
        FenceCompletionCallbackFn callback,
        void* user_ctx,
        uint64_t* out_registration_id);
    HRESULT WaitForFenceValue(
        ID3D12Fence* fence,
        UINT64 target_value,
        UINT timeout_ms);
    uint32_t PumpReadyCallbacks();
    void NotifyDeviceRemoved(HRESULT removed_reason);
    bool IsDeviceRemoved() const noexcept {
        return device_removed_.load(std::memory_order_acquire);
    }
    HRESULT RemovedReason() const noexcept {
        return removed_reason_.load(std::memory_order_acquire);
    }
    HANDLE DeviceRemovedEventHandle() const noexcept { return device_removed_event_; }

private:
    struct PendingEntry {
        uint64_t id = 0ull;
        ID3D12Fence* user_fence = nullptr;
        ID3D12Fence* host_fence = nullptr;
        UINT64 target_value = 0ull;
        FenceCompletionCallbackFn callback = nullptr;
        void* user_ctx = nullptr;
        HANDLE event_handle = nullptr;
        HRESULT status = S_OK;
    };

    void EnsureWorkerStartedLocked();
    void WorkerLoop();
    void DispatchOutsideLock(std::vector<PendingEntry>& ready);

    DeviceX* device_x_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<uint64_t> next_id_{1ull};
    std::atomic<bool> device_removed_{false};
    std::atomic<HRESULT> removed_reason_{S_OK};
    bool stopping_ = false;
    bool worker_started_ = false;
    HANDLE stop_event_ = nullptr;
    HANDLE wakeup_event_ = nullptr;
    HANDLE device_removed_event_ = nullptr;
    std::thread worker_thread_;
    std::vector<PendingEntry> pending_;
};

class CommandQueueX final
    : public ID3D12CommandQueueX,
      public IOpenScarlettPresentCapture,
      public IOpenScarlettPresentWindow,
      public IOpenScarlettPresentHdr,
      public IOpenScarlettPresentTiming,
      public IOpenScarlettPresentPacing,
      public IOpenScarlettFenceWaitDispatcher {
public:
    CommandQueueX(DeviceX* parent_device,
                  ID3D12CommandQueue* host_queue,
                  HWND initial_hwnd = nullptr);
    ~CommandQueueX();

    CommandQueueX(const CommandQueueX&) = delete;
    CommandQueueX& operator=(const CommandQueueX&) = delete;

    ID3D12CommandQueue* HostQueue() const noexcept { return host_queue_; }
    D3D12_COMMAND_LIST_TYPE QueueType() const noexcept { return queue_type_; }

    // Title-created Win32 console display screen helpers (non-virtual; preserves COM vtable ABI).
    HWND EffectiveOutputWindow() const noexcept;
    bool AdoptTitleWindow(HWND hwnd) noexcept;

    // Zero-overhead C++ integration hook invoked on command list submission.
    using QueueSubmitHookFn = void (*)(
        ID3D12CommandQueue* host_queue,
        UINT num_lists,
        ID3D12CommandList* const* host_lists,
        void* user_ctx) noexcept;
    void SetSubmitHook(QueueSubmitHookFn fn, void* user_ctx) noexcept;

    static ID3D12Pageable* UnwrapHostPageable(ID3D12Pageable* maybe_wrapped) noexcept;
    static ID3D12CommandQueue* UnwrapHostQueue(ID3D12CommandQueue* maybe_wrapped) noexcept;
    static void NotifyDeviceRemovedForDevice(DeviceX* dev, HRESULT removed_reason) noexcept;

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

    // ID3D12CommandQueue (slots 8..18)
    void STDMETHODCALLTYPE UpdateTileMappings(
        ID3D12Resource* pResource,
        UINT NumResourceRegions,
        const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
        const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
        ID3D12Heap* pHeap,
        UINT NumRanges,
        const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
        const UINT* pHeapRangeStartOffsets,
        const UINT* pRangeTileCounts,
        D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE CopyTileMappings(
        ID3D12Resource* pDstResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
        ID3D12Resource* pSrcResource,
        const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
        const D3D12_TILE_REGION_SIZE* pRegionSize,
        D3D12_TILE_MAPPING_FLAGS Flags) override;
    void STDMETHODCALLTYPE ExecuteCommandLists(
        UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* pFence, UINT64 Value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* pFrequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(
        UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    using ID3D12CommandQueue::GetDesc;
    D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* pRet) override;
#else
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override;
#endif

    // ID3D12CommandQueueX (slots 19..24; slots 19..21 match real GDKX ID3D12CommandQueue)
    HRESULT STDMETHODCALLTYPE PresentX(
        UINT PlaneCount,
        const D3D12XBOX_PRESENT_PLANE_PARAMETERS* pPlaneParameters,
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters) override;
    HRESULT STDMETHODCALLTYPE SuspendX(uint32_t Flags) override;
    HRESULT STDMETHODCALLTYPE ResumeX() override;
    HRESULT STDMETHODCALLTYPE KickoffX() override;
    HRESULT STDMETHODCALLTYPE Wait32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    HRESULT STDMETHODCALLTYPE Wait64BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint64_t Value,
        uint64_t Mask,
        D3D12XBOX_WAIT_VALUE_OP Operation) override;
    HRESULT STDMETHODCALLTYPE Write32BitValueX(
        D3D12_GPU_VIRTUAL_ADDRESS Address,
        uint32_t Value,
        uint32_t Flags) override;

    // IOpenScarlettPresentCapture
    HRESULT STDMETHODCALLTYPE SetOutputWindow(HWND hwnd) override;
    HRESULT STDMETHODCALLTYPE SetReadbackCaptureEnabled(BOOL enabled) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentStats(
        uint64_t* out_fnv1a_hash,
        uint32_t* out_width,
        uint32_t* out_height,
        uint64_t* out_present_count) override;
    HRESULT STDMETHODCALLTYPE CopyLastPresentPixels(
        uint8_t* out_rgba8,
        uint32_t buffer_bytes,
        uint32_t* out_copied_bytes) override;

    // IOpenScarlettPresentWindow
    HRESULT STDMETHODCALLTYPE GetPresentWindowState(
        D3D12XBOX_PRESENT_WINDOW_STATE* out_state) override;
    HRESULT STDMETHODCALLTYPE ToggleBorderlessFullscreen() override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetWindowCloseBehavior(BOOL post_quit_on_close) override;

    // IOpenScarlettPresentHdr
    HRESULT STDMETHODCALLTYPE SetPresentConfig(
        const D3D12XBOX_PRESENTX_CONFIG* config) override;
    HRESULT STDMETHODCALLTYPE SetHdrMetaData(
        const D3D12XBOX_HDR10_METADATA* metadata) override;
    HRESULT STDMETHODCALLTYPE GetHdrSwapChainState(
        D3D12XBOX_HDR_SWAPCHAIN_STATE* out_state) override;
    HRESULT STDMETHODCALLTYPE CopyLastPresentRawPixels(
        uint8_t* out_raw,
        uint32_t buffer_bytes,
        uint32_t* out_copied_bytes,
        DXGI_FORMAT* out_format) override;

    // IOpenScarlettPresentTiming
    HRESULT STDMETHODCALLTYPE GetPresentTimingStats(
        D3D12XBOX_PRESENT_TIMING_STATS* out_stats) override;

    // IOpenScarlettPresentPacing
    HRESULT STDMETHODCALLTYPE SetFrameCapFps(
        uint32_t fps_or_zero_for_uncapped,
        BOOL override_active) override;
    HRESULT STDMETHODCALLTYPE GetPresentPacingState(
        D3D12XBOX_PRESENT_PACING_STATE* out_state) override;

    // IOpenScarlettFenceWaitDispatcher
    HRESULT STDMETHODCALLTYPE RegisterFenceCallback(
        ID3D12Fence* fence,
        UINT64 target_value,
        FenceCompletionCallbackFn callback,
        void* user_ctx,
        uint64_t* out_registration_id) override;
    HRESULT STDMETHODCALLTYPE WaitForFenceValue(
        ID3D12Fence* fence,
        UINT64 target_value,
        UINT timeout_ms) override;
    HRESULT STDMETHODCALLTYPE PumpFenceCallbacks(
        uint32_t* out_dispatched_count) override;
    HRESULT STDMETHODCALLTYPE NotifyDeviceRemoved(
        HRESULT removed_reason) override;

private:
    uint32_t ResolveEffectiveFrameIntervalUsLocked(
        bool& out_override_active,
        uint32_t& out_fcap_fps) const noexcept;
    void PumpMirrorReadbackCompletionLocked() noexcept;
    void ExecuteVulkanRecordedBatch(const VulkanRecordedBatch& batch);
    HRESULT EnsureVulkanSwapChainLocked(
        HWND hwnd, uint32_t width, uint32_t height, bool prefer_hdr10, bool headless);
    void ReleaseVulkanSwapChainLocked();
    HRESULT WaitForInternalFenceWithTimingLocked(
        int64_t* out_signal_qpc,
        double* out_wait_us,
        uint64_t* out_completed_value);
    HRESULT EnsurePresentHelperObjectsLocked();
    HRESULT EnsureSwapChainLocked(
        HWND hwnd,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        D3D12XBOX_COLOR_SPACE color_space,
        bool force_resize);
    void ConfigureSwapChainHdrLocked(
        DXGI_FORMAT src_format,
        D3D12XBOX_COLOR_SPACE req_color_space);
    HRESULT EnsureReadbackBufferLocked(uint64_t required_bytes);
    HRESULT EnsureToneMapUploadBufferLocked(uint64_t required_bytes);
    HRESULT EnsureCompositeObjectsLocked(uint32_t width, uint32_t height, DXGI_FORMAT rt_format);
    void ReleaseCompositeObjectsLocked();
    HRESULT WaitForInternalFenceLocked();
    void AttachWindowSubclassLocked(HWND hwnd);
    void DetachWindowSubclassLocked();
    HWND ResolveTargetOutputWindowLocked(
        const D3D12XBOX_PRESENT_PARAMETERS* pPresentParameters,
        bool force_headless);
    void ReleaseSwapChainLocked();
    void ReleasePresentObjectsLocked();
    void PaceFrameLocked(uint32_t frame_interval_us);

    std::atomic<ULONG> ref_count_{1};
    DeviceX* device_x_ = nullptr;
    ID3D12CommandQueue* host_queue_ = nullptr;
    D3D12_COMMAND_LIST_TYPE queue_type_ = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::atomic<bool> suspended_{false};
    std::atomic<uint64_t> kickoff_count_{0ull};
    std::atomic<QueueSubmitHookFn> submit_hook_{nullptr};
    std::atomic<void*> submit_hook_ctx_{nullptr};

    mutable std::mutex present_mutex_;
    HWND attached_hwnd_ = nullptr;
    HWND subclassed_hwnd_ = nullptr;
    bool explicit_window_detached_ = false;
    std::shared_ptr<WindowCallbackState> window_state_;
    bool readback_capture_enabled_ = true;

    static constexpr uint32_t kPresentRingSlots = 3u;
    static constexpr uint32_t kReadbackRingSlots = 4u;
    ID3D12CommandAllocator* present_alloc_ = nullptr;
    ID3D12GraphicsCommandList* present_cmd_ = nullptr;
    ID3D12CommandAllocator* present_ring_allocs_[kPresentRingSlots]{};
    ID3D12GraphicsCommandList* present_ring_cmds_[kPresentRingSlots]{};
    uint64_t present_ring_fences_[kPresentRingSlots]{};
    uint32_t present_ring_idx_ = 0u;
    ID3D12CommandAllocator* rb_ring_allocs_[kReadbackRingSlots]{};
    ID3D12GraphicsCommandList* rb_ring_cmds_[kReadbackRingSlots]{};
    uint64_t rb_ring_fences_[kReadbackRingSlots]{};
    uint32_t rb_ring_idx_ = 0u;
    ID3D12Fence* present_fence_ = nullptr;
    HANDLE present_fence_event_ = nullptr;
    uint64_t present_fence_val_ = 0;

    ID3D12Resource* readback_buf_ = nullptr;
    uint64_t readback_buf_capacity_ = 0;
    ID3D12Resource* tonemap_upload_buf_ = nullptr;
    uint64_t tonemap_upload_capacity_ = 0;

    ID3D12RootSignature* composite_rs_ = nullptr;
    ID3D12PipelineState* composite_pso_ = nullptr;
    DXGI_FORMAT composite_pso_rt_format_ = DXGI_FORMAT_UNKNOWN;
    ID3D12DescriptorHeap* composite_rtv_heap_ = nullptr;
    ID3D12DescriptorHeap* composite_srv_heap_ = nullptr;
    ID3D12Resource* composite_rt_ = nullptr;
    uint32_t composite_rt_width_ = 0u;
    uint32_t composite_rt_height_ = 0u;
    DXGI_FORMAT composite_rt_format_ = DXGI_FORMAT_UNKNOWN;
    bool composite_compile_failed_ = false;

    IDXGISwapChain3* swapchain_ = nullptr;
    IDXGISwapChain4* swapchain4_ = nullptr;
    HWND swapchain_hwnd_ = nullptr;
    uint32_t swapchain_width_ = 0;
    uint32_t swapchain_height_ = 0;
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t swapchain_flags_ = 0;
    uint32_t swapchain_buffer_count_ = 0;
    uint32_t swapchain_max_latency_ = 0;
    bool swapchain_occluded_ = false;
    HANDLE frame_latency_waitable_object_ = nullptr;

    D3D12XBOX_PRESENTX_CONFIG present_config_{};
    D3D12XBOX_HDR10_METADATA hdr_metadata_{};
    bool hdr_metadata_explicit_ = false;
    DXGI_COLOR_SPACE_TYPE active_color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT cs_support_flags_ = 0u;
    bool color_space_applied_ = false;
    bool hdr_metadata_applied_ = false;
    bool tone_map_fallback_active_ = false;
    DXGI_FORMAT last_presented_src_format_ = DXGI_FORMAT_UNKNOWN;
    D3D12XBOX_COLOR_SPACE last_presented_color_space_ =
        D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    int64_t qpc_freq_ = 0;
    int64_t last_paced_qpc_ = 0;
    int64_t last_present_qpc_ = 0;
    HANDLE pacing_timer_ = nullptr;
    bool pacing_timer_high_res_ = false;
    uint32_t cached_monitor_refresh_hz_ = 0u;
    HWND cached_monitor_hwnd_ = nullptr;
    uint32_t fcap_override_fps_ = 0u;
    bool fcap_override_active_ = false;
    uint32_t last_effective_interval_us_ = 0u;
    uint64_t paced_frame_count_ = 0ull;
    std::atomic<uint64_t> cached_gpu_ts_freq_{0ull};

    uint64_t present_count_ = 0;
    uint64_t non_blank_present_count_ = 0;
    bool auto_exit_posted_ = false;
    bool capture_png_written_ = false;
    uint64_t latency_waits_ = 0;
    double last_frame_ms_ = 0.0;
    uint64_t last_present_hash_ = 0;
    uint32_t last_present_width_ = 0;
    uint32_t last_present_height_ = 0;
    std::vector<uint8_t> last_present_pixels_;
    std::vector<uint8_t> last_present_raw_pixels_;
    DXGI_FORMAT last_present_raw_format_ = DXGI_FORMAT_UNKNOWN;

    // Separated PresentX GPU fence-signal vs DXGI/Vulkan presentation timings
    D3D12XBOX_PRESENT_TIMING_STATS last_timing_stats_{};

    // Vulkan swapchain state (used when DeviceX::IsVulkanRoutingActive())
    uint64_t vk_swapchain_handle_ = 0ull;
    openscarlett::VulkanSwapchainInfo vk_swapchain_info_{};
    HWND vk_swapchain_hwnd_ = nullptr;
    uint32_t vk_swapchain_width_ = 0u;
    uint32_t vk_swapchain_height_ = 0u;
    bool vk_swapchain_headless_ = false;

    std::unique_ptr<FenceWaitDispatcher> fence_dispatcher_;

    // GPU-fence-tracked transient resource retirement (keeps staging buffers alive until GPU finishes)
    std::mutex retire_mutex_;
    ID3D12Fence* retire_fence_ = nullptr;
    uint64_t retire_fence_val_ = 0ull;
    std::vector<std::pair<uint64_t, std::vector<ID3D12Resource*>>> pending_retirements_;
    void PumpPendingRetirementsLocked() noexcept;

    // Non-blocking GPU-ordered Write32BitValueX / Write64BitValueX fence completion
    std::mutex deferred_write_mutex_;
    ID3D12Fence* deferred_write_fence_ = nullptr;
    uint64_t deferred_write_fence_val_ = 0ull;
};

}  // namespace openscarlett::d3d12_xs
