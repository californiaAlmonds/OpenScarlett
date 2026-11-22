#include "openscarlett/kernel/title_va_mirror.h"
#include "openscarlett/kernel/vram_residency_manager.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>

#if defined(_MSC_VER)
#define OS_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_UUIDOF(T) IID_##T
#endif
#endif

namespace openscarlett {

#if defined(_WIN32)

namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1ull) return value;
    const uint64_t rem = value % alignment;
    return rem == 0ull ? value : (value + (alignment - rem));
}

uint64_t AlignDown(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1ull) return value;
    return value - (value % alignment);
}

uint64_t QueryOsPageSize() noexcept {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize ? static_cast<uint64_t>(si.dwPageSize) : kPageSize4KB;
}

HRESULT CreateCommittedBuffer(ID3D12Device* device,
                              D3D12_HEAP_TYPE heap_type,
                              uint64_t size_bytes,
                              D3D12_RESOURCE_FLAGS flags,
                              D3D12_RESOURCE_STATES initial_state,
                              ID3D12Resource** out_resource) {
    if (!device || !out_resource || size_bytes == 0ull) return E_INVALIDARG;
    *out_resource = nullptr;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = heap_type;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_props.CreationNodeMask = 1u;
    heap_props.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0ull;
    desc.Width = size_bytes;
    desc.Height = 1u;
    desc.DepthOrArraySize = 1u;
    desc.MipLevels = 1u;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1u;
    desc.SampleDesc.Quality = 0u;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    return device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initial_state,
        nullptr,
        OS_UUIDOF(ID3D12Resource),
        reinterpret_cast<void**>(out_resource));
}

void EmitTransitionBarrier(ID3D12GraphicsCommandList* cmd_list,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before_state,
                           D3D12_RESOURCE_STATES after_state) noexcept {
    if (!cmd_list || !resource || before_state == after_state) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before_state;
    barrier.Transition.StateAfter = after_state;
    cmd_list->ResourceBarrier(1u, &barrier);
}

}  // namespace

TitleVAMirror::TitleVAMirror() : page_size_bytes_(QueryOsPageSize()) {}

TitleVAMirror::~TitleVAMirror() {
    Clear();
}

void TitleVAMirror::ReleaseEntryResources(RegionEntry& entry) noexcept {
    if (entry.readback_staging) {
        entry.readback_staging->Release();
        entry.readback_staging = nullptr;
    }
    if (entry.upload_staging) {
        entry.upload_staging->Release();
        entry.upload_staging = nullptr;
    }
    if (entry.gpu_shadow) {
        entry.gpu_shadow->Release();
        entry.gpu_shadow = nullptr;
    }
    if (entry.owns_cpu_memory && entry.cpu_backing_ptr) {
        VirtualFree(entry.cpu_backing_ptr, 0, MEM_RELEASE);
        entry.cpu_backing_ptr = nullptr;
        entry.owns_cpu_memory = false;
    }
}

ptrdiff_t TitleVAMirror::FindRegionIndexLocked(uint64_t title_va) const noexcept {
    if (regions_.empty() || title_va == 0ull) return -1;
    // Upper bound finds the first region with region.title_va > title_va.
    const auto it = std::upper_bound(
        regions_.begin(),
        regions_.end(),
        title_va,
        [](uint64_t addr, const RegionEntry& entry) noexcept {
            return addr < entry.title_va;
        });
    if (it == regions_.begin()) return -1;
    const auto prev = std::prev(it);
    if (title_va >= prev->title_va && title_va < prev->title_va + prev->size_bytes) {
        return static_cast<ptrdiff_t>(std::distance(regions_.begin(), prev));
    }
    return -1;
}

Status TitleVAMirror::AllocateRegion(uint64_t size_bytes,
                                     uint64_t alignment,
                                     TitleVaResourceKind kind,
                                     ID3D12Device* device,
                                     TitleVaRegionInfo& out_info,
                                     uint64_t explicit_title_va,
                                     bool allow_uav) {
    TitleVaMappingDesc desc{};
    desc.title_va = explicit_title_va;
    desc.size_bytes = size_bytes;
    desc.alignment = alignment ? alignment : kPageSize64KB;
    desc.kind = kind;
    desc.existing_write_watch_cpu_ptr = nullptr;
    desc.gpu_shadow_resource = nullptr;
    desc.pc_gpu_va = 0ull;
    desc.initial_state = D3D12_RESOURCE_STATE_COMMON;
    desc.allow_uav = allow_uav;
    return RegisterRegion(desc, device, out_info);
}

Status TitleVAMirror::RegisterRegion(const TitleVaMappingDesc& desc,
                                     ID3D12Device* device,
                                     TitleVaRegionInfo& out_info) {
    if (desc.size_bytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "TitleVAMirror::RegisterRegion requires size_bytes > 0");
    }

    const uint64_t align = std::max<uint64_t>(desc.alignment, page_size_bytes_);
    const uint64_t alloc_size = AlignUp(desc.size_bytes, page_size_bytes_);

    RegionEntry entry{};
    entry.size_bytes = desc.size_bytes;
    entry.alloc_size_bytes = alloc_size;
    entry.alignment = align;
    entry.kind = desc.kind;
    entry.current_state = desc.initial_state;

    // Step 1: Allocate or attach MEM_WRITE_WATCH CPU backing memory.
    if (desc.existing_write_watch_cpu_ptr != nullptr) {
        entry.cpu_backing_ptr = desc.existing_write_watch_cpu_ptr;
        entry.owns_cpu_memory = false;
        // Probe write-watch capability on the caller-supplied pointer.
        const UINT reset_rc = ResetWriteWatch(
            entry.cpu_backing_ptr, static_cast<SIZE_T>(entry.alloc_size_bytes));
        entry.write_watch_enabled = (reset_rc == 0u);
    } else {
        void* mem = VirtualAlloc(
            nullptr,
            static_cast<SIZE_T>(entry.alloc_size_bytes),
            MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
            PAGE_READWRITE);
        if (!mem) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "VirtualAlloc(MEM_WRITE_WATCH) failed for TitleVAMirror region");
        }
        entry.cpu_backing_ptr = mem;
        entry.owns_cpu_memory = true;
        entry.write_watch_enabled = (ResetWriteWatch(
            entry.cpu_backing_ptr, static_cast<SIZE_T>(entry.alloc_size_bytes)) == 0u);
    }

    entry.title_va = desc.title_va != 0ull
                         ? desc.title_va
                         : reinterpret_cast<uint64_t>(entry.cpu_backing_ptr);

    // Step 2: Attach or create PC GPU shadow + staging resources.
    if (desc.gpu_shadow_resource != nullptr) {
        entry.gpu_shadow = desc.gpu_shadow_resource;
        entry.gpu_shadow->AddRef();
        const D3D12_RESOURCE_DESC rdesc = entry.gpu_shadow->GetDesc();
        if (desc.pc_gpu_va != 0ull) {
            entry.pc_gpu_va = desc.pc_gpu_va;
        } else if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            entry.pc_gpu_va = entry.gpu_shadow->GetGPUVirtualAddress();
        }
    } else if (device != nullptr && desc.kind == TitleVaResourceKind::Buffer) {
        const D3D12_RESOURCE_FLAGS shadow_flags =
            desc.allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                           : D3D12_RESOURCE_FLAG_NONE;
        const HRESULT hr = CreateCommittedBuffer(
            device,
            D3D12_HEAP_TYPE_DEFAULT,
            entry.alloc_size_bytes,
            shadow_flags,
            desc.initial_state,
            &entry.gpu_shadow);
        if (FAILED(hr)) {
            ReleaseEntryResources(entry);
            return Status::Error(StatusCode::DeviceError,
                                 "Failed to create PC GPU default-heap shadow buffer");
        }
        entry.pc_gpu_va = entry.gpu_shadow->GetGPUVirtualAddress();
    } else {
        entry.pc_gpu_va = desc.pc_gpu_va;
    }

    if (device != nullptr && entry.gpu_shadow != nullptr &&
        entry.kind == TitleVaResourceKind::Buffer) {
        HRESULT hr = CreateCommittedBuffer(
            device,
            D3D12_HEAP_TYPE_UPLOAD,
            entry.alloc_size_bytes,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            &entry.upload_staging);
        if (FAILED(hr)) {
            ReleaseEntryResources(entry);
            return Status::Error(StatusCode::DeviceError,
                                 "Failed to create upload staging buffer for TitleVAMirror");
        }
        hr = CreateCommittedBuffer(
            device,
            D3D12_HEAP_TYPE_READBACK,
            entry.alloc_size_bytes,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &entry.readback_staging);
        if (FAILED(hr)) {
            ReleaseEntryResources(entry);
            return Status::Error(StatusCode::DeviceError,
                                 "Failed to create readback staging buffer for TitleVAMirror");
        }
    }

    // Step 3: Insert into sorted non-overlapping interval vector.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const uint64_t new_start = entry.title_va;
    const uint64_t new_end = entry.title_va + entry.size_bytes;
    if (new_end < new_start) {
        ReleaseEntryResources(entry);
        return Status::Error(StatusCode::InvalidArgument,
                             "Title-VA interval overflow in TitleVAMirror::RegisterRegion");
    }

    const auto insert_pos = std::lower_bound(
        regions_.begin(),
        regions_.end(),
        new_start,
        [](const RegionEntry& existing, uint64_t addr) noexcept {
            return existing.title_va < addr;
        });

    if (insert_pos != regions_.end() && new_end > insert_pos->title_va) {
        ReleaseEntryResources(entry);
        return Status::Error(StatusCode::InvalidArgument,
                             "Title-VA interval overlaps an existing registered region");
    }
    if (insert_pos != regions_.begin()) {
        const auto prev = std::prev(insert_pos);
        if (prev->title_va + prev->size_bytes > new_start) {
            ReleaseEntryResources(entry);
            return Status::Error(StatusCode::InvalidArgument,
                                 "Title-VA interval overlaps a preceding registered region");
        }
    }

    out_info.title_va = entry.title_va;
    out_info.size_bytes = entry.size_bytes;
    out_info.alloc_size_bytes = entry.alloc_size_bytes;
    out_info.alignment = entry.alignment;
    out_info.kind = entry.kind;
    out_info.cpu_backing_ptr = entry.cpu_backing_ptr;
    out_info.owns_cpu_memory = entry.owns_cpu_memory;
    out_info.write_watch_enabled = entry.write_watch_enabled;
    out_info.gpu_shadow_resource = entry.gpu_shadow;
    out_info.pc_gpu_va = entry.pc_gpu_va;
    out_info.current_state = entry.current_state;

    regions_.insert(insert_pos, entry);
    ++total_registrations_;
    return Status::Ok();
}

Status TitleVAMirror::UnregisterRegion(uint64_t title_va) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) {
        return Status::Error(StatusCode::NotFound, "Title-VA region not registered");
    }
    const uint64_t base_va = regions_[static_cast<size_t>(idx)].title_va;
    pending_readbacks_.erase(
        std::remove_if(pending_readbacks_.begin(),
                       pending_readbacks_.end(),
                       [base_va](const PendingReadback& rb) noexcept {
                           return rb.title_va == base_va;
                       }),
        pending_readbacks_.end());

    ReleaseEntryResources(regions_[static_cast<size_t>(idx)]);
    regions_.erase(regions_.begin() + idx);
    return Status::Ok();
}

void TitleVAMirror::Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pending_readbacks_.clear();
    for (auto& entry : regions_) {
        ReleaseEntryResources(entry);
    }
    regions_.clear();
}

Status TitleVAMirror::QueryDirtyLocked(RegionEntry& entry,
                                       bool reset_watch,
                                       std::vector<DirtyPageRange>& out_ranges,
                                       uint64_t& out_dirty_pages,
                                       uint64_t& out_clean_pages) const {
    out_ranges.clear();
    out_dirty_pages = 0ull;
    out_clean_pages = 0ull;

    const uint64_t total_pages =
        (entry.alloc_size_bytes + page_size_bytes_ - 1ull) / page_size_bytes_;

    if (!entry.cpu_backing_ptr || !entry.write_watch_enabled) {
        // Fallback when write-watch isn't active: treat whole region as dirty.
        out_ranges.push_back(DirtyPageRange{0ull, entry.size_bytes});
        out_dirty_pages = total_pages;
        out_clean_pages = 0ull;
        return Status::Ok();
    }

    const DWORD flags = reset_watch ? WRITE_WATCH_FLAG_RESET : 0u;
    std::vector<void*> dirty_addrs;
    std::vector<void*> batch(static_cast<size_t>(std::max<uint64_t>(total_pages + 16ull, 64ull)));

    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(entry.cpu_backing_ptr);
    const uintptr_t end_addr = base_addr + static_cast<uintptr_t>(entry.alloc_size_bytes);
    DWORD granularity = static_cast<DWORD>(page_size_bytes_);

    for (;;) {
        ULONG_PTR count = static_cast<ULONG_PTR>(batch.size());
        const UINT rc = GetWriteWatch(
            flags,
            entry.cpu_backing_ptr,
            static_cast<SIZE_T>(entry.alloc_size_bytes),
            batch.data(),
            &count,
            &granularity);
        if (rc != 0u) {
            return Status::Error(StatusCode::DeviceError, "Win32 GetWriteWatch failed");
        }
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(batch[i]);
            if (addr >= base_addr && addr < end_addr) {
                dirty_addrs.push_back(batch[i]);
            }
        }
        // If reset_watch is false, GetWriteWatch does not drain pages on retry, so stop after
        // one pass (and grow batch if it filled completely).
        if (count < static_cast<ULONG_PTR>(batch.size())) {
            break;
        }
        if (!reset_watch) {
            dirty_addrs.clear();
            batch.resize(batch.size() * 2u);
        }
    }

    const uint64_t gran = granularity ? static_cast<uint64_t>(granularity) : page_size_bytes_;
    if (dirty_addrs.empty()) {
        out_dirty_pages = 0ull;
        out_clean_pages = total_pages;
        return Status::Ok();
    }

    std::sort(dirty_addrs.begin(), dirty_addrs.end());
    dirty_addrs.erase(std::unique(dirty_addrs.begin(), dirty_addrs.end()), dirty_addrs.end());

    out_dirty_pages = static_cast<uint64_t>(dirty_addrs.size());
    out_clean_pages = (total_pages >= out_dirty_pages) ? (total_pages - out_dirty_pages) : 0ull;

    // Coalesce contiguous dirty page runs into minimal copy ranges clamped to entry.size_bytes.
    for (void* p : dirty_addrs) {
        const uint64_t raw_off =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p) - base_addr);
        if (raw_off >= entry.size_bytes) continue;
        const uint64_t page_len = std::min<uint64_t>(gran, entry.size_bytes - raw_off);
        if (!out_ranges.empty() &&
            out_ranges.back().offset + out_ranges.back().size == raw_off) {
            out_ranges.back().size += page_len;
        } else {
            out_ranges.push_back(DirtyPageRange{raw_off, page_len});
        }
    }
    return Status::Ok();
}

Status TitleVAMirror::QueryDirtyPageRanges(uint64_t title_va,
                                           bool reset_watch,
                                           std::vector<DirtyPageRange>& out_ranges,
                                           uint64_t* out_dirty_page_count,
                                           uint64_t* out_clean_page_count) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) {
        return Status::Error(StatusCode::NotFound, "Title-VA region not found");
    }
    uint64_t dirty_pages = 0ull;
    uint64_t clean_pages = 0ull;
    const Status st = QueryDirtyLocked(
        regions_[static_cast<size_t>(idx)], reset_watch, out_ranges, dirty_pages, clean_pages);
    if (st.ok()) {
        if (out_dirty_page_count) *out_dirty_page_count = dirty_pages;
        if (out_clean_page_count) *out_clean_page_count = clean_pages;
    }
    return st;
}

Status TitleVAMirror::ResetDirtyTracking(uint64_t title_va) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) {
        return Status::Error(StatusCode::NotFound, "Title-VA region not found");
    }
    RegionEntry& entry = regions_[static_cast<size_t>(idx)];
    if (entry.cpu_backing_ptr && entry.write_watch_enabled) {
        if (ResetWriteWatch(entry.cpu_backing_ptr,
                            static_cast<SIZE_T>(entry.alloc_size_bytes)) != 0u) {
            return Status::Error(StatusCode::DeviceError, "Win32 ResetWriteWatch failed");
        }
    }
    return Status::Ok();
}

Status TitleVAMirror::FlushDirtyPagesToGpu(ID3D12GraphicsCommandList* cmd_list,
                                           uint64_t title_va_or_zero_for_all,
                                           FlushStats* out_stats,
                                           D3D12_RESOURCE_STATES after_state) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    FlushStats local_stats{};
    std::vector<size_t> target_indices;
    if (title_va_or_zero_for_all == 0ull) {
        target_indices.reserve(regions_.size());
        for (size_t i = 0; i < regions_.size(); ++i) {
            target_indices.push_back(i);
        }
    } else {
        const ptrdiff_t idx = FindRegionIndexLocked(title_va_or_zero_for_all);
        if (idx < 0) {
            return Status::Error(StatusCode::NotFound,
                                 "TitleVAMirror::FlushDirtyPagesToGpu region not found");
        }
        target_indices.push_back(static_cast<size_t>(idx));
    }

    std::vector<DirtyPageRange> ranges;
    for (const size_t idx : target_indices) {
        RegionEntry& entry = regions_[idx];
        ++local_stats.regions_inspected;

        uint64_t dirty_pages = 0ull;
        uint64_t clean_pages = 0ull;
        const Status qst = QueryDirtyLocked(entry, true, ranges, dirty_pages, clean_pages);
        if (!qst.ok()) {
            return qst;
        }

        local_stats.clean_pages_skipped += clean_pages;
        clean_pages_skipped_ += clean_pages;

        if (ranges.empty()) {
            ++local_stats.clean_regions_skipped;
            ++clean_skips_;
            continue;
        }

        ++local_stats.regions_flushed;
        local_stats.dirty_pages_flushed += dirty_pages;
        local_stats.coalesced_ranges += static_cast<uint64_t>(ranges.size());
        dirty_pages_flushed_ += dirty_pages;

        uint64_t region_bytes = 0ull;
        for (const auto& r : ranges) {
            region_bytes += r.size;
            local_stats.flushed_ranges.push_back(r);
        }

        if (entry.upload_staging != nullptr && entry.cpu_backing_ptr != nullptr) {
            uint8_t* upload_ptr = nullptr;
            const D3D12_RANGE empty_read{0, 0};
            const HRESULT hr = entry.upload_staging->Map(
                0u, &empty_read, reinterpret_cast<void**>(&upload_ptr));
            if (FAILED(hr) || !upload_ptr) {
                return Status::Error(StatusCode::DeviceError,
                                     "Failed to map TitleVAMirror upload staging buffer");
            }
            const auto* cpu_bytes = static_cast<const uint8_t*>(entry.cpu_backing_ptr);
            for (const auto& r : ranges) {
                std::memcpy(upload_ptr + r.offset, cpu_bytes + r.offset, static_cast<size_t>(r.size));
            }
            entry.upload_staging->Unmap(0u, nullptr);
        }

        if (cmd_list != nullptr && entry.gpu_shadow != nullptr &&
            entry.upload_staging != nullptr) {
            EmitTransitionBarrier(
                cmd_list, entry.gpu_shadow, entry.current_state, D3D12_RESOURCE_STATE_COPY_DEST);
            for (const auto& r : ranges) {
                cmd_list->CopyBufferRegion(
                    entry.gpu_shadow, r.offset, entry.upload_staging, r.offset, r.size);
            }
            EmitTransitionBarrier(
                cmd_list, entry.gpu_shadow, D3D12_RESOURCE_STATE_COPY_DEST, after_state);
            entry.current_state = after_state;
        }

        local_stats.bytes_uploaded += region_bytes;
        bytes_uploaded_ += region_bytes;
    }

    if (out_stats) {
        *out_stats = std::move(local_stats);
    }
    return Status::Ok();
}

Status TitleVAMirror::ScheduleGpuReadback(ID3D12GraphicsCommandList* cmd_list,
                                          uint64_t title_va,
                                          uint64_t fence_value,
                                          uint64_t offset,
                                          uint64_t size_bytes,
                                          D3D12_RESOURCE_STATES restore_state) {
    if (!cmd_list) {
        return Status::Error(StatusCode::InvalidArgument,
                             "TitleVAMirror::ScheduleGpuReadback requires a non-null command list");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) {
        return Status::Error(StatusCode::NotFound,
                             "TitleVAMirror::ScheduleGpuReadback region not found");
    }

    RegionEntry& entry = regions_[static_cast<size_t>(idx)];
    if (!entry.gpu_shadow || !entry.readback_staging) {
        return Status::Error(StatusCode::DeviceError,
                             "Title-VA region has no GPU shadow or readback staging buffer");
    }

    const uint64_t base_off = (title_va - entry.title_va) + offset;
    if (base_off > entry.size_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "Readback offset exceeds region size");
    }
    const uint64_t copy_len =
        (size_bytes == 0ull) ? (entry.size_bytes - base_off) : size_bytes;
    if (copy_len == 0ull || base_off + copy_len > entry.size_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "Readback range exceeds region bounds");
    }

    EmitTransitionBarrier(
        cmd_list, entry.gpu_shadow, entry.current_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd_list->CopyBufferRegion(
        entry.readback_staging, base_off, entry.gpu_shadow, base_off, copy_len);
    EmitTransitionBarrier(
        cmd_list, entry.gpu_shadow, D3D12_RESOURCE_STATE_COPY_SOURCE, restore_state);
    entry.current_state = restore_state;

    pending_readbacks_.push_back(PendingReadback{
        entry.title_va,
        fence_value,
        base_off,
        copy_len,
    });
    return Status::Ok();
}

Status TitleVAMirror::CompleteReadbacksOnFence(uint64_t completed_fence_value,
                                               uint64_t* out_completed_count,
                                               uint64_t* out_bytes_copied) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    uint64_t completed = 0ull;
    uint64_t copied = 0ull;
    std::vector<PendingReadback> remaining;
    remaining.reserve(pending_readbacks_.size());

    for (const auto& rb : pending_readbacks_) {
        if (rb.fence_value > completed_fence_value) {
            remaining.push_back(rb);
            continue;
        }

        const ptrdiff_t idx = FindRegionIndexLocked(rb.title_va);
        if (idx < 0) continue;
        RegionEntry& entry = regions_[static_cast<size_t>(idx)];
        if (!entry.readback_staging || !entry.cpu_backing_ptr) continue;

        const D3D12_RANGE read_range{
            static_cast<SIZE_T>(rb.offset),
            static_cast<SIZE_T>(rb.offset + rb.size_bytes),
        };
        const uint8_t* readback_ptr = nullptr;
        const HRESULT hr = entry.readback_staging->Map(
            0u, &read_range, reinterpret_cast<void**>(const_cast<uint8_t**>(&readback_ptr)));
        if (FAILED(hr) || !readback_ptr) {
            return Status::Error(StatusCode::DeviceError,
                                 "Failed to map TitleVAMirror readback staging buffer");
        }

        auto* cpu_bytes = static_cast<uint8_t*>(entry.cpu_backing_ptr);
        std::memcpy(cpu_bytes + rb.offset,
                    readback_ptr + rb.offset,
                    static_cast<size_t>(rb.size_bytes));

        const D3D12_RANGE empty_write{0, 0};
        entry.readback_staging->Unmap(0u, &empty_write);

        // Clear Win32 write-watch on the exact overwritten pages so this GPU->CPU
        // synchronization is NOT falsely detected as a new CPU dirty modification.
        if (entry.write_watch_enabled) {
            const uint64_t reset_start = AlignDown(rb.offset, page_size_bytes_);
            const uint64_t reset_end =
                std::min<uint64_t>(AlignUp(rb.offset + rb.size_bytes, page_size_bytes_),
                                   entry.alloc_size_bytes);
            if (reset_end > reset_start) {
                ResetWriteWatch(
                    cpu_bytes + reset_start,
                    static_cast<SIZE_T>(reset_end - reset_start));
            }
        }

        ++completed;
        copied += rb.size_bytes;
        bytes_readback_ += rb.size_bytes;
        ++completed_readbacks_;
    }

    pending_readbacks_ = std::move(remaining);
    if (out_completed_count) *out_completed_count = completed;
    if (out_bytes_copied) *out_bytes_copied = copied;
    return Status::Ok();
}

Status TitleVAMirror::NotifyGpuResourceState(uint64_t title_va,
                                             D3D12_RESOURCE_STATES new_state) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) {
        return Status::Error(StatusCode::NotFound, "Title-VA region not found");
    }
    regions_[static_cast<size_t>(idx)].current_state = new_state;
    return Status::Ok();
}

bool TitleVAMirror::ResolveTitleVa(uint64_t title_va,
                                   uint64_t& out_pc_gpu_va,
                                   uint64_t& out_offset) const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) return false;
    const RegionEntry& entry = regions_[static_cast<size_t>(idx)];
    const uint64_t rel = title_va - entry.title_va;
    out_offset = rel;
    out_pc_gpu_va = entry.pc_gpu_va ? (entry.pc_gpu_va + rel) : 0ull;
    return true;
}

bool TitleVAMirror::ResolveTitleVaToCpu(uint64_t title_va,
                                        void*& out_cpu_ptr,
                                        uint64_t& out_remaining_bytes) const noexcept {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) return false;
    const RegionEntry& entry = regions_[static_cast<size_t>(idx)];
    if (!entry.cpu_backing_ptr) return false;
    const uint64_t rel = title_va - entry.title_va;
    out_cpu_ptr = static_cast<uint8_t*>(entry.cpu_backing_ptr) + rel;
    out_remaining_bytes = entry.size_bytes - rel;
    return true;
}

bool TitleVAMirror::LookupRegion(uint64_t title_va, TitleVaRegionInfo& out_info) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const ptrdiff_t idx = FindRegionIndexLocked(title_va);
    if (idx < 0) return false;
    const RegionEntry& entry = regions_[static_cast<size_t>(idx)];
    out_info.title_va = entry.title_va;
    out_info.size_bytes = entry.size_bytes;
    out_info.alloc_size_bytes = entry.alloc_size_bytes;
    out_info.alignment = entry.alignment;
    out_info.kind = entry.kind;
    out_info.cpu_backing_ptr = entry.cpu_backing_ptr;
    out_info.owns_cpu_memory = entry.owns_cpu_memory;
    out_info.write_watch_enabled = entry.write_watch_enabled;
    out_info.gpu_shadow_resource = entry.gpu_shadow;
    out_info.pc_gpu_va = entry.pc_gpu_va;
    out_info.current_state = entry.current_state;
    return true;
}

TitleVaMirrorStats TitleVAMirror::GetStats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    TitleVaMirrorStats s{};
    s.total_registrations = total_registrations_;
    s.active_registrations = static_cast<uint64_t>(regions_.size());
    s.dirty_pages_flushed = dirty_pages_flushed_;
    s.bytes_uploaded = bytes_uploaded_;
    s.bytes_readback = bytes_readback_;
    s.clean_skips = clean_skips_;
    s.clean_pages_skipped = clean_pages_skipped_;
    s.pending_readbacks = static_cast<uint64_t>(pending_readbacks_.size());
    s.completed_readbacks = completed_readbacks_;
    return s;
}

// ============================================================================
// Physical Page Double-Mapping / Aliasing (64 KB / 2 MB / 4 MB)
// ============================================================================

namespace {

bool IsValidPhysicalGranularity(PhysicalPageGranularity g) noexcept {
    return g == PhysicalPageGranularity::Page64KB ||
           g == PhysicalPageGranularity::Page2MB ||
           g == PhysicalPageGranularity::Page4MB;
}

}  // namespace

struct PhysicalPageAliasManager::Impl {
    struct PhysicalBlock {
        uint64_t phys_id = 0ull;
        PhysicalPageGranularity granularity = PhysicalPageGranularity::Page64KB;
        uint32_t page_count = 0u;
        uint64_t total_size_bytes = 0ull;
        HANDLE section_handle = nullptr;
        std::vector<uint8_t> dirty_pages;
        uint32_t active_alias_count = 0u;
    };

    mutable std::shared_mutex mutex;
    uint64_t next_phys_id = 1ull;
    uint64_t next_alias_id = 1ull;
    std::unordered_map<uint64_t, PhysicalBlock> blocks;
    std::unordered_map<uint64_t, PhysicalAliasMapping> aliases;

    uint64_t total_physical_bytes = 0ull;
    uint64_t total_alias_mappings_created = 0ull;
    uint64_t dirty_pages_marked = 0ull;
    uint64_t mirror_sync_flushes = 0ull;
};

PhysicalPageAliasManager::PhysicalPageAliasManager() : impl_(std::make_unique<Impl>()) {}

PhysicalPageAliasManager::~PhysicalPageAliasManager() {
    Clear();
}

Status PhysicalPageAliasManager::AllocatePhysicalBlock(
    PhysicalPageGranularity granularity,
    uint32_t page_count,
    uint64_t& out_phys_id) {
    out_phys_id = 0ull;
    if (!IsValidPhysicalGranularity(granularity) || page_count == 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "AllocatePhysicalBlock requires 64KB/2MB/4MB granularity and page_count > 0");
    }

    const uint64_t page_bytes = static_cast<uint64_t>(granularity);
    if (static_cast<uint64_t>(page_count) > (UINT64_MAX / page_bytes)) {
        return Status::Error(StatusCode::InvalidArgument, "Physical block size overflow");
    }
    const uint64_t total_bytes = static_cast<uint64_t>(page_count) * page_bytes;
    const DWORD size_high = static_cast<DWORD>((total_bytes >> 32u) & 0xFFFFFFFFull);
    const DWORD size_low = static_cast<DWORD>(total_bytes & 0xFFFFFFFFull);

    HANDLE h_section = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        size_high,
        size_low,
        nullptr);
    if (!h_section) {
        return Status::Error(StatusCode::OutOfMemory,
                             "CreateFileMappingW failed for physical page block");
    }

    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    Impl::PhysicalBlock blk{};
    blk.phys_id = impl_->next_phys_id++;
    blk.granularity = granularity;
    blk.page_count = page_count;
    blk.total_size_bytes = total_bytes;
    blk.section_handle = h_section;
    blk.dirty_pages.assign(page_count, 0u);
    blk.active_alias_count = 0u;

    const uint64_t id = blk.phys_id;
    impl_->blocks.emplace(id, std::move(blk));
    impl_->total_physical_bytes += total_bytes;
    out_phys_id = id;
    return Status::Ok();
}

Status PhysicalPageAliasManager::MapAlias(
    uint64_t phys_id,
    uint32_t first_page,
    uint32_t page_count,
    uint64_t guest_title_va,
    PhysicalAliasMapping& out_mapping) {
    out_mapping = PhysicalAliasMapping{};
    if (page_count == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "MapAlias requires page_count > 0");
    }

    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    auto it = impl_->blocks.find(phys_id);
    if (it == impl_->blocks.end()) {
        return Status::Error(StatusCode::NotFound, "Physical block not found");
    }
    Impl::PhysicalBlock& blk = it->second;
    if (first_page >= blk.page_count || page_count > blk.page_count - first_page) {
        return Status::Error(StatusCode::InvalidArgument, "MapAlias page range exceeds physical block");
    }

    const uint64_t page_bytes = static_cast<uint64_t>(blk.granularity);
    const uint64_t file_offset = static_cast<uint64_t>(first_page) * page_bytes;
    const uint64_t view_bytes = static_cast<uint64_t>(page_count) * page_bytes;
    const DWORD off_high = static_cast<DWORD>((file_offset >> 32u) & 0xFFFFFFFFull);
    const DWORD off_low = static_cast<DWORD>(file_offset & 0xFFFFFFFFull);

    // 64 KB, 2 MB, and 4 MB are exact multiples of Win32 dwAllocationGranularity (64 KiB),
    // so MapViewOfFile maps identical OS physical page frames directly into a fresh VA!
    void* va = MapViewOfFile(
        blk.section_handle,
        FILE_MAP_ALL_ACCESS,
        off_high,
        off_low,
        static_cast<SIZE_T>(view_bytes));
    if (!va) {
        return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile failed creating physical page alias");
    }

    PhysicalAliasMapping mapping{};
    mapping.alias_id = impl_->next_alias_id++;
    mapping.phys_id = phys_id;
    mapping.granularity = blk.granularity;
    mapping.first_page = first_page;
    mapping.page_count = page_count;
    mapping.size_bytes = view_bytes;
    mapping.cpu_va = va;
    mapping.guest_title_va = guest_title_va ? guest_title_va : reinterpret_cast<uint64_t>(va);

    impl_->aliases.emplace(mapping.alias_id, mapping);
    ++blk.active_alias_count;
    ++impl_->total_alias_mappings_created;
    out_mapping = mapping;
    return Status::Ok();
}

Status PhysicalPageAliasManager::UnmapAlias(uint64_t alias_id) {
    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    auto it = impl_->aliases.find(alias_id);
    if (it == impl_->aliases.end()) {
        return Status::Error(StatusCode::NotFound, "Physical page alias not found");
    }
    const PhysicalAliasMapping m = it->second;
    if (m.cpu_va) {
        UnmapViewOfFile(m.cpu_va);
    }
    auto blk_it = impl_->blocks.find(m.phys_id);
    if (blk_it != impl_->blocks.end() && blk_it->second.active_alias_count > 0u) {
        --blk_it->second.active_alias_count;
    }
    impl_->aliases.erase(it);
    return Status::Ok();
}

Status PhysicalPageAliasManager::FreePhysicalBlock(uint64_t phys_id) {
    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    auto it = impl_->blocks.find(phys_id);
    if (it == impl_->blocks.end()) {
        return Status::Error(StatusCode::NotFound, "Physical block not found");
    }
    if (it->second.active_alias_count > 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Cannot free physical block while active VA aliases remain mapped");
    }
    if (it->second.section_handle) {
        CloseHandle(it->second.section_handle);
        it->second.section_handle = nullptr;
    }
    if (impl_->total_physical_bytes >= it->second.total_size_bytes) {
        impl_->total_physical_bytes -= it->second.total_size_bytes;
    } else {
        impl_->total_physical_bytes = 0ull;
    }
    impl_->blocks.erase(it);
    return Status::Ok();
}

void PhysicalPageAliasManager::Clear() {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    for (auto& kv : impl_->aliases) {
        if (kv.second.cpu_va) {
            UnmapViewOfFile(kv.second.cpu_va);
        }
    }
    impl_->aliases.clear();
    for (auto& kv : impl_->blocks) {
        if (kv.second.section_handle) {
            CloseHandle(kv.second.section_handle);
        }
    }
    impl_->blocks.clear();
    impl_->total_physical_bytes = 0ull;
}

Status PhysicalPageAliasManager::MarkAliasRangeDirty(
    uint64_t alias_id, uint64_t byte_offset, uint64_t byte_size) {
    if (byte_size == 0ull) return Status::Ok();
    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    auto ait = impl_->aliases.find(alias_id);
    if (ait == impl_->aliases.end()) {
        return Status::Error(StatusCode::NotFound, "Physical page alias not found");
    }
    const PhysicalAliasMapping& m = ait->second;
    if (byte_offset >= m.size_bytes || byte_size > m.size_bytes - byte_offset) {
        return Status::Error(StatusCode::InvalidArgument, "Dirty byte range exceeds alias bounds");
    }
    auto bit = impl_->blocks.find(m.phys_id);
    if (bit == impl_->blocks.end()) {
        return Status::Error(StatusCode::NotFound, "Underlying physical block missing");
    }
    Impl::PhysicalBlock& blk = bit->second;
    const uint64_t page_bytes = static_cast<uint64_t>(blk.granularity);
    const uint32_t rel_first = static_cast<uint32_t>(byte_offset / page_bytes);
    const uint32_t rel_last = static_cast<uint32_t>((byte_offset + byte_size - 1ull) / page_bytes);
    for (uint32_t p = rel_first; p <= rel_last; ++p) {
        const uint32_t phys_page = m.first_page + p;
        if (phys_page < blk.dirty_pages.size()) {
            if (blk.dirty_pages[phys_page] == 0u) {
                blk.dirty_pages[phys_page] = 1u;
                ++impl_->dirty_pages_marked;
            }
        }
    }
    return Status::Ok();
}

Status PhysicalPageAliasManager::QueryDirtyPhysicalPages(
    uint64_t phys_id,
    bool reset_dirty,
    std::vector<uint32_t>& out_dirty_pages) {
    out_dirty_pages.clear();
    std::unique_lock<std::shared_mutex> lk(impl_->mutex);
    auto bit = impl_->blocks.find(phys_id);
    if (bit == impl_->blocks.end()) {
        return Status::Error(StatusCode::NotFound, "Physical block not found");
    }
    Impl::PhysicalBlock& blk = bit->second;
    for (uint32_t p = 0u; p < blk.page_count; ++p) {
        if (blk.dirty_pages[p] != 0u) {
            out_dirty_pages.push_back(p);
            if (reset_dirty) {
                blk.dirty_pages[p] = 0u;
            }
        }
    }
    return Status::Ok();
}

Status PhysicalPageAliasManager::SyncDirtyPagesToTitleVaMirror(
    uint64_t phys_id,
    TitleVAMirror& mirror,
    uint64_t mirror_title_va,
    ID3D12GraphicsCommandList* cmd_list,
    FlushStats* out_flush_stats) {
    std::vector<uint32_t> dirty_pages;
    uint64_t page_bytes = 0ull;
    std::vector<uint8_t> snapshot;
    uint64_t block_total_bytes = 0ull;

    {
        std::unique_lock<std::shared_mutex> lk(impl_->mutex);
        auto bit = impl_->blocks.find(phys_id);
        if (bit == impl_->blocks.end()) {
            return Status::Error(StatusCode::NotFound, "Physical block not found");
        }
        Impl::PhysicalBlock& blk = bit->second;
        page_bytes = static_cast<uint64_t>(blk.granularity);
        block_total_bytes = blk.total_size_bytes;

        for (uint32_t p = 0u; p < blk.page_count; ++p) {
            if (blk.dirty_pages[p] != 0u) {
                dirty_pages.push_back(p);
                blk.dirty_pages[p] = 0u;
            }
        }
        if (dirty_pages.empty()) {
            if (out_flush_stats) *out_flush_stats = FlushStats{};
            return Status::Ok();
        }

        // Map a temporary view over the whole physical block (or find an alias) to copy dirty pages
        // coherently into the TitleVAMirror's write-watched CPU backing memory.
        void* mirror_cpu = nullptr;
        uint64_t mirror_rem = 0ull;
        if (!mirror.ResolveTitleVaToCpu(mirror_title_va, mirror_cpu, mirror_rem) || !mirror_cpu) {
            return Status::Error(StatusCode::NotFound, "Target TitleVAMirror region not found");
        }

        void* view = MapViewOfFile(blk.section_handle, FILE_MAP_READ, 0u, 0u, static_cast<SIZE_T>(block_total_bytes));
        if (!view) {
            return Status::Error(StatusCode::OutOfMemory, "MapViewOfFile failed during mirror sync");
        }
        const auto* src_bytes = static_cast<const uint8_t*>(view);
        auto* dst_bytes = static_cast<uint8_t*>(mirror_cpu);
        for (uint32_t p : dirty_pages) {
            const uint64_t off = static_cast<uint64_t>(p) * page_bytes;
            if (off >= mirror_rem) continue;
            const uint64_t len = std::min<uint64_t>(page_bytes, mirror_rem - off);
            std::memcpy(dst_bytes + off, src_bytes + off, static_cast<size_t>(len));
        }
        UnmapViewOfFile(view);
        ++impl_->mirror_sync_flushes;
    }

    return mirror.FlushDirtyPagesToGpu(cmd_list, mirror_title_va, out_flush_stats);
}

bool PhysicalPageAliasManager::ResolveAliasByVa(
    const void* cpu_va, PhysicalAliasMapping& out_mapping) const {
    if (!cpu_va) return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(cpu_va);
    std::shared_lock<std::shared_mutex> lk(impl_->mutex);
    for (const auto& kv : impl_->aliases) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(kv.second.cpu_va);
        const uintptr_t end = base + static_cast<uintptr_t>(kv.second.size_bytes);
        if (addr >= base && addr < end) {
            out_mapping = kv.second;
            return true;
        }
    }
    return false;
}

Status PhysicalPageAliasManager::GetAliasesForBlock(
    uint64_t phys_id, std::vector<PhysicalAliasMapping>& out_aliases) const {
    out_aliases.clear();
    std::shared_lock<std::shared_mutex> lk(impl_->mutex);
    if (impl_->blocks.find(phys_id) == impl_->blocks.end()) {
        return Status::Error(StatusCode::NotFound, "Physical block not found");
    }
    for (const auto& kv : impl_->aliases) {
        if (kv.second.phys_id == phys_id) {
            out_aliases.push_back(kv.second);
        }
    }
    return Status::Ok();
}

PhysicalAliasStats PhysicalPageAliasManager::GetStats() const {
    std::shared_lock<std::shared_mutex> lk(impl_->mutex);
    PhysicalAliasStats s{};
    s.active_physical_blocks = static_cast<uint64_t>(impl_->blocks.size());
    s.active_aliases = static_cast<uint64_t>(impl_->aliases.size());
    s.total_physical_bytes = impl_->total_physical_bytes;
    s.total_alias_mappings_created = impl_->total_alias_mappings_created;
    s.dirty_pages_marked = impl_->dirty_pages_marked;
    s.mirror_sync_flushes = impl_->mirror_sync_flushes;
    return s;
}

#endif  // defined(_WIN32)

}  // namespace openscarlett
