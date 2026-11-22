#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/core/types.h"

namespace openscarlett {

enum class MemoryPageSize : uint64_t {
    Page4KB = 4096ull,
    Page64KB = 65536ull,
    Page2MB = 2ull * 1024ull * 1024ull,
    Page4MB = 4ull * 1024ull * 1024ull,
};

enum class MemoryProtection : uint32_t {
    NoAccess = 0,
    ReadOnly = 1,
    ReadWrite = 2,
    WriteCombined = 3,
};

enum UnifiedMemoryFlags : uint32_t {
    kMemFlagNone = 0u,
    kMemFlagGraphics = 1u << 0,       // Scarlett MEM_GRAPHICS (GPU-visible unified memory)
    kMemFlagLargePages = 1u << 1,     // 2 MiB / 4 MiB page backing preference
    kMemFlagWriteCombined = 1u << 2,  // CPU write-combining cache attribute
};

struct UnifiedMemoryBlock {
    void* host_ptr = nullptr;
    GpuVirtualAddress gpu_va = 0;
    size_t size = 0;
    MemoryPageSize page_size = MemoryPageSize::Page64KB;
    bool gpu_visible = true;
    bool write_combined = false;
};

struct EraAllocationAttributes {
    uint32_t object_type = 0;
    uint32_t heap_tracks_attributes = 0;
    uint32_t must_succeed = 0;
    uint32_t fixed_size = 0;
    uint32_t allocator_id = 0;
    uint32_t alignment_code = 0;
    uint32_t memory_type = 0;
    uint32_t page_size_code = 0;
    uint32_t zero_initialize = 0;
    uint32_t alloc_flags = 0;      // Synthesized MEM_GRAPHICS | MEM_TITLE | MEM_WRITE_COMBINE | MEM_LARGE_PAGES | MEM_4MB_PAGES
    uint32_t protect_flags = 0;    // Synthesized PAGE_* flags
    uint64_t alignment_bytes = 16;
    uint64_t page_size_bytes = 4096;
    uint64_t raw_attributes = 0;
};

// Decodes a 64-bit Xbox ERA XALLOC_ATTRIBUTES bitfield into structured fields,
// page size (4KB / 64KB / 2MB / 4MB), alignment, and memory/protection flags.
bool DecodeEraAllocationAttributes(uint64_t raw_attributes,
                                   EraAllocationAttributes& out_decoded) noexcept;

struct PhysicalAllocation {
    uint64_t physical_id = 0;
    size_t size = 0;
    MemoryPageSize page_size = MemoryPageSize::Page64KB;
    uint32_t flags = kMemFlagNone;
};

struct UnifiedMemoryAlias {
    void* host_ptr = nullptr;
    GpuVirtualAddress gpu_va = 0;
    uint64_t physical_id = 0;
    size_t physical_offset = 0;
    size_t size = 0;
    MemoryProtection protection = MemoryProtection::ReadWrite;
    uint32_t flags = kMemFlagNone;
    bool from_placeholder = false;
};

// Scarlett unified CPU/GPU virtual & physical memory manager.
// Uses Windows 10/11 VirtualAlloc2 / MapViewOfFile3 / UnmapViewOfFile2 placeholder
// splitting & coalescing backed by pagefile sections (CreateFileMappingW) to provide
// hardware-coherent multi-VA physical aliasing at 64 KiB and 4 MiB page granules.
class UnifiedMemoryManager {
public:
    UnifiedMemoryManager();
    ~UnifiedMemoryManager();

    UnifiedMemoryManager(const UnifiedMemoryManager&) = delete;
    UnifiedMemoryManager& operator=(const UnifiedMemoryManager&) = delete;

    Status Allocate(size_t size,
                    MemoryPageSize page_size,
                    bool gpu_visible,
                    bool write_combined,
                    UnifiedMemoryBlock& out_block);

    Status Free(GpuVirtualAddress gpu_va);
    bool LookupByGpuVa(GpuVirtualAddress gpu_va, UnifiedMemoryBlock& out_block) const;
    size_t TotalAllocatedBytes() const;
    size_t ActiveAllocationCount() const;

    // Physical-to-virtual aliasing & Scarlett MEM_GRAPHICS placeholder support.
    static bool SupportsOsPlaceholders();

    Status AllocatePhysicalPages(size_t size,
                                 MemoryPageSize page_size,
                                 uint32_t flags,
                                 PhysicalAllocation& out_phys);
    Status FreePhysicalPages(uint64_t physical_id);

    // Reserves a virtual address placeholder aligned to `granule` (64 KiB or 4 MiB).
    Status ReserveVirtualPlaceholder(size_t size,
                                     void*& out_base,
                                     MemoryPageSize granule = MemoryPageSize::Page64KB);
    Status ReleaseVirtualPlaceholder(void* base);

    // Explicit placeholder split / coalesce helpers (also performed automatically during
    // MapPhysicalAlias / UnmapPhysicalAlias).
    Status SplitVirtualPlaceholder(void* split_addr, size_t split_size);
    Status CoalesceVirtualPlaceholders(void* base_addr, size_t total_size);
    size_t PlaceholderSpanCount(const void* base) const;

    Status MapPhysicalAlias(uint64_t physical_id,
                            size_t physical_offset,
                            size_t size,
                            MemoryProtection protection,
                            uint32_t flags,
                            void* placeholder_addr,
                            UnifiedMemoryAlias& out_alias);
    Status UnmapPhysicalAlias(void* host_ptr);
    Status ProtectAlias(void* host_ptr, size_t size, MemoryProtection new_protection);
    bool LookupAliasByHostPtr(const void* host_ptr, UnifiedMemoryAlias& out_alias) const;
    bool LookupBlockByHostPtr(const void* host_ptr, UnifiedMemoryBlock& out_block) const;
    bool LookupPlaceholderByHostPtr(const void* host_ptr,
                                    void*& out_base,
                                    size_t& out_size,
                                    size_t& out_granule,
                                    bool& out_mapped) const;

    size_t ActivePhysicalPoolCount() const;
    size_t ActiveAliasCount() const;
    size_t ActivePlaceholderCount() const;

private:
    struct PhysicalPoolEntry {
        PhysicalAllocation info{};
        void* os_handle = nullptr;  // Win32 HANDLE from CreateFileMappingW
        std::vector<uint8_t> fallback_storage;
    };

    struct PlaceholderSpan {
        uintptr_t base = 0;
        size_t size = 0;
        bool mapped = false;
    };

    struct PlaceholderReservation {
        uintptr_t base = 0;
        size_t total_size = 0;
        size_t granule = 65536u;
        std::vector<PlaceholderSpan> spans;
    };

    Status SplitSpanLocked(PlaceholderReservation& res,
                           size_t span_idx,
                           uintptr_t req_start,
                           size_t aligned_size,
                           size_t& out_target_idx);
    void CoalesceAroundLocked(PlaceholderReservation& res, size_t span_idx);

    mutable std::mutex mutex_;
    GpuVirtualAddress next_gpu_va_ = 0x0000010000000000ull;
    uint64_t next_physical_id_ = 1;
    size_t total_allocated_bytes_ = 0;
    std::unordered_map<GpuVirtualAddress, UnifiedMemoryBlock> blocks_;
    std::unordered_map<uint64_t, PhysicalPoolEntry> physical_pools_;
    std::unordered_map<uintptr_t, UnifiedMemoryAlias> aliases_;
    std::unordered_map<uintptr_t, PlaceholderReservation> placeholders_;
};

}  // namespace openscarlett
