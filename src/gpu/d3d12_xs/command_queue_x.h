#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/vulkan_backend.h"

namespace openscarlett::d3d12_xs {

class DeviceX;
struct WindowCallbackState;
struct VulkanRecordedBatch;

class CommandQueueX final
    : public ID3D12CommandQueueX,
      public IOpenScarlettPresentCapture,
      public IOpenScarlettPresentWindow,
      public IOpenScarlettPresentHdr,
      public IOpenScarlettPresentTiming,
      public IOpenScarlettPresentPacing {
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

    ID3D12CommandAllocator* present_alloc_ = nullptr;
    ID3D12GraphicsCommandList* present_cmd_ = nullptr;
    ID3D12Fence* present_fence_ = nullptr;
    HANDLE present_fence_event_ = nullptr;
    uint64_t present_fence_val_ = 0;

    ID3D12Resource* readback_buf_ = nullptr;
    uint64_t readback_buf_capacity_ = 0;
    ID3D12Resource* tonemap_upload_buf_ = nullptr;
    uint64_t tonemap_upload_capacity_ = 0;

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
};

}  // namespace openscarlett::d3d12_xs
