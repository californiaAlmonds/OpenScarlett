#pragma once

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#endif

#include "openscarlett/core/status.h"
#include "openscarlett/core/types.h"

namespace openscarlett {

// Resource classification for 64-bit Xbox title virtual-address regions mirrored
// onto discrete PC GPU VRAM.
enum class TitleVaResourceKind : uint8_t {
    Buffer = 0,
    Texture = 1,
};

// Coalesced contiguous dirty CPU byte range within a mirrored Title-VA region.
struct DirtyPageRange {
    uint64_t offset = 0;
    uint64_t size = 0;
};

#if defined(_WIN32)

// Registration descriptor for a 64-bit Title-VA unified-memory mirror region.
struct TitleVaMappingDesc {
    uint64_t title_va = 0;  // Guest 64-bit Title VA (0 => use CPU backing VA)
    uint64_t size_bytes = 0;
    uint64_t alignment = kPageSize64KB;
    TitleVaResourceKind kind = TitleVaResourceKind::Buffer;

    // Optional caller-owned CPU memory allocated with MEM_WRITE_WATCH.
    // When nullptr, TitleVAMirror allocates write-watched CPU memory via VirtualAlloc.
    void* existing_write_watch_cpu_ptr = nullptr;

    // Optional caller-provided PC GPU default-heap shadow resource.
    // When nullptr and a non-null ID3D12Device* is passed to RegisterRegion,
    // TitleVAMirror creates a matching DEFAULT buffer plus UPLOAD/READBACK staging buffers.
    ID3D12Resource* gpu_shadow_resource = nullptr;

    // Optional explicit PC GPU VA (when 0 and gpu_shadow_resource is a buffer,
    // queried via gpu_shadow_resource->GetGPUVirtualAddress()).
    uint64_t pc_gpu_va = 0;

    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    bool allow_uav = true;
};

// Snapshot of a registered Title-VA mirrored region.
struct TitleVaRegionInfo {
    uint64_t title_va = 0;
    uint64_t size_bytes = 0;
    uint64_t alloc_size_bytes = 0;
    uint64_t alignment = 0;
    TitleVaResourceKind kind = TitleVaResourceKind::Buffer;
    void* cpu_backing_ptr = nullptr;
    bool owns_cpu_memory = false;
    bool write_watch_enabled = false;
    ID3D12Resource* gpu_shadow_resource = nullptr;
    uint64_t pc_gpu_va = 0;
    D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
};

// Per-flush report returned by TitleVAMirror::FlushDirtyPagesToGpu.
struct FlushStats {
    uint64_t regions_inspected = 0;
    uint64_t regions_flushed = 0;
    uint64_t dirty_pages_flushed = 0;
    uint64_t clean_pages_skipped = 0;
    uint64_t clean_regions_skipped = 0;
    uint64_t coalesced_ranges = 0;
    uint64_t bytes_uploaded = 0;
    std::vector<DirtyPageRange> flushed_ranges;
};

// Cumulative telemetry counters for TitleVAMirror.
struct TitleVaMirrorStats {
    uint64_t total_registrations = 0;
    uint64_t active_registrations = 0;
    uint64_t dirty_pages_flushed = 0;
    uint64_t bytes_uploaded = 0;
    uint64_t bytes_readback = 0;
    uint64_t clean_skips = 0;
    uint64_t clean_pages_skipped = 0;
    uint64_t pending_readbacks = 0;
    uint64_t completed_readbacks = 0;
};

// 64-bit Xbox Title-VA Unified Memory Mirror for discrete-VRAM PCs.
//
// Tracks placed 64-bit guest virtual address regions backed by Win32 MEM_WRITE_WATCH
// CPU memory and mirrored to discrete PC GPU default-heap shadow resources:
//  - Queries ONLY modified 4 KiB CPU pages via Win32 GetWriteWatch(WRITE_WATCH_FLAG_RESET)
//    and coalesces contiguous dirty pages into minimal CopyBufferRegion uploads.
//  - Schedules asynchronous GPU->CPU readbacks keyed by fence value and clears write-watch
//    on completion so GPU readback writes do not trigger false CPU dirty uploads.
//  - Provides O(log N) thread-safe guest Title-VA -> PC GPU-VA interval resolution.
class TitleVAMirror {
public:
    TitleVAMirror();
    ~TitleVAMirror();

    TitleVAMirror(const TitleVAMirror&) = delete;
    TitleVAMirror& operator=(const TitleVAMirror&) = delete;

    // Allocates a new MEM_WRITE_WATCH CPU backing region (and optional D3D12 default-heap
    // shadow + upload/readback staging buffers when `device != nullptr`) and registers it.
    Status AllocateRegion(uint64_t size_bytes,
                          uint64_t alignment,
                          TitleVaResourceKind kind,
                          ID3D12Device* device,
                          TitleVaRegionInfo& out_info,
                          uint64_t explicit_title_va = 0,
                          bool allow_uav = true);

    // Registers an existing MEM_WRITE_WATCH region or allocates backing memory per `desc`.
    Status RegisterRegion(const TitleVaMappingDesc& desc,
                          ID3D12Device* device,
                          TitleVaRegionInfo& out_info);

    // Unregisters a region by base or interior Title-VA and releases owned resources.
    Status UnregisterRegion(uint64_t title_va);

    // Unregisters all regions and releases all owned CPU/GPU resources.
    void Clear();

    // Queries dirty 4 KiB CPU pages for the region containing `title_va` and returns
    // coalesced byte ranges. When `reset_watch` is true, atomically resets dirty bits.
    Status QueryDirtyPageRanges(uint64_t title_va,
                                bool reset_watch,
                                std::vector<DirtyPageRange>& out_ranges,
                                uint64_t* out_dirty_page_count = nullptr,
                                uint64_t* out_clean_page_count = nullptr);

    // Explicitly resets Win32 write-watch tracking for the region containing `title_va`.
    Status ResetDirtyTracking(uint64_t title_va);

    // Flushes ONLY dirty 4 KiB CPU page ranges to the PC GPU default-heap shadow resource
    // via staging upload + CopyBufferRegion with automatic resource-state transitions.
    // Pass `title_va_or_zero_for_all == 0` to flush all registered regions.
    Status FlushDirtyPagesToGpu(
        ID3D12GraphicsCommandList* cmd_list,
        uint64_t title_va_or_zero_for_all = 0,
        FlushStats* out_stats = nullptr,
        D3D12_RESOURCE_STATES after_state = D3D12_RESOURCE_STATE_COMMON);

    // Records a GPU-to-readback copy for the region containing `title_va` and tags it with
    // `fence_value`. Call CompleteReadbacksOnFence() once the GPU fence reaches `fence_value`.
    // When `size_bytes == 0`, copies `[offset, region.size_bytes)`.
    Status ScheduleGpuReadback(
        ID3D12GraphicsCommandList* cmd_list,
        uint64_t title_va,
        uint64_t fence_value,
        uint64_t offset = 0,
        uint64_t size_bytes = 0,
        D3D12_RESOURCE_STATES restore_state = D3D12_RESOURCE_STATE_COMMON);

    // Copies all pending GPU readbacks with `fence_value <= completed_fence_value` back into
    // CPU Title-VA backing memory and resets Win32 write-watch on the overwritten pages so
    // the readback write itself is not falsely treated as a fresh CPU dirty modification.
    Status CompleteReadbacksOnFence(uint64_t completed_fence_value,
                                    uint64_t* out_completed_count = nullptr,
                                    uint64_t* out_bytes_copied = nullptr);

    // Updates tracked D3D12 resource state when external command recording transitions a
    // mirrored shadow resource directly.
    Status NotifyGpuResourceState(uint64_t title_va, D3D12_RESOURCE_STATES new_state);

    // O(log N) binary-search lookup mapping a guest 64-bit Title-VA (base or interior)
    // to its PC GPU virtual address and byte offset from the region base.
    bool ResolveTitleVa(uint64_t title_va,
                        uint64_t& out_pc_gpu_va,
                        uint64_t& out_offset) const noexcept;

    // Resolves a guest 64-bit Title-VA (base or interior) to its CPU backing pointer and
    // remaining accessible byte count within the registered region.
    bool ResolveTitleVaToCpu(uint64_t title_va,
                             void*& out_cpu_ptr,
                             uint64_t& out_remaining_bytes) const noexcept;

    // Retrieves full metadata for the region containing `title_va`.
    bool LookupRegion(uint64_t title_va, TitleVaRegionInfo& out_info) const;

    // Returns cumulative mirror telemetry.
    TitleVaMirrorStats GetStats() const;

    // Returns the OS page size used for write-watch granularity (4096 bytes on x86-64 Windows).
    uint64_t PageSizeBytes() const noexcept { return page_size_bytes_; }

private:
    struct RegionEntry {
        uint64_t title_va = 0;
        uint64_t size_bytes = 0;
        uint64_t alloc_size_bytes = 0;
        uint64_t alignment = kPageSize64KB;
        TitleVaResourceKind kind = TitleVaResourceKind::Buffer;

        void* cpu_backing_ptr = nullptr;
        bool owns_cpu_memory = false;
        bool write_watch_enabled = false;

        ID3D12Resource* gpu_shadow = nullptr;
        ID3D12Resource* upload_staging = nullptr;
        ID3D12Resource* readback_staging = nullptr;
        uint64_t pc_gpu_va = 0;
        D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct PendingReadback {
        uint64_t title_va = 0;
        uint64_t fence_value = 0;
        uint64_t offset = 0;
        uint64_t size_bytes = 0;
    };

    ptrdiff_t FindRegionIndexLocked(uint64_t title_va) const noexcept;
    void ReleaseEntryResources(RegionEntry& entry) noexcept;
    Status QueryDirtyLocked(RegionEntry& entry,
                            bool reset_watch,
                            std::vector<DirtyPageRange>& out_ranges,
                            uint64_t& out_dirty_pages,
                            uint64_t& out_clean_pages) const;

    uint64_t page_size_bytes_ = kPageSize4KB;
    mutable std::shared_mutex mutex_;
    // Sorted by RegionEntry::title_va ascending; non-overlapping intervals.
    std::vector<RegionEntry> regions_;
    std::vector<PendingReadback> pending_readbacks_;

    uint64_t total_registrations_ = 0;
    uint64_t dirty_pages_flushed_ = 0;
    uint64_t bytes_uploaded_ = 0;
    uint64_t bytes_readback_ = 0;
    uint64_t clean_skips_ = 0;
    uint64_t clean_pages_skipped_ = 0;
    uint64_t completed_readbacks_ = 0;
};

#endif  // defined(_WIN32)

}  // namespace openscarlett
