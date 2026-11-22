#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/core/types.h"
#include "openscarlett/kernel/xmemory.h"

namespace openscarlett {

// Scarlett standard tiled/reserved resource page granularity (64 KiB)
// and optional 4 MiB super-tile granularity (64 standard tiles).
constexpr size_t kGpuTileSizeBytes = 65536u;
constexpr size_t kGpuSuperTileSizeBytes = 4ull * 1024ull * 1024ull;
constexpr uint32_t kGpuTilesPerSuperTile = 64u;

enum GpuTilePoolFlags : uint32_t {
    kTilePoolFlagNone = 0u,
    kTilePoolFlagGraphics = 1u << 0,       // GPU-visible MEM_GRAPHICS physical backing
    kTilePoolFlagLargePages = 1u << 1,     // 4 MiB physical page pool / super-tile alignment
    kTilePoolFlagWriteCombined = 1u << 2,  // CPU write-combined attribute
};

enum GpuTileMappingFlags : uint32_t {
    kTileMapFlagNone = 0u,
    kTileMapFlagNull = 1u << 0,             // Unmap virtual tile(s) (NULL tile)
    kTileMapFlagReadOnly = 1u << 1,         // Map virtual tile(s) with read-only CPU protection
    kTileMapFlagReuseSingleTile = 1u << 2,  // Alias every virtual tile in range to one physical tile
    kTileMapFlagSuperTile4MB = 1u << 3,     // Map 4 MiB super-tiles (for 4 MiB tiled resources)
};

struct GpuTileCoord {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t subresource = 0;
};

struct GpuTileMappingInfo {
    bool mapped = false;
    uint64_t pool_id = 0;
    uint32_t pool_tile_offset = 0;
    void* virtual_address = nullptr;
    GpuVirtualAddress gpu_va = 0;
    size_t tile_size_bytes = kGpuTileSizeBytes;
    uint32_t flags = kTileMapFlagNone;
};

struct GpuTilePoolStats {
    size_t total_pools = 0;
    size_t total_resources = 0;
    size_t total_physical_tiles = 0;
    size_t mapped_virtual_tiles = 0;
    size_t unique_mapped_physical_tiles = 0;
    // Number of duplicate virtual tile mappings sharing an already-mapped physical tile:
    // invariant: mapped_virtual_tiles == unique_mapped_physical_tiles + aliased_tile_count.
    size_t aliased_tile_count = 0;
    size_t physical_bytes = 0;
};

// Scarlett 64 KiB (and optional 4 MiB super-tile) GPU tile pool backed by
// UnifiedMemoryManager physical sections and OS virtual memory placeholders.
class GpuTilePool {
public:
    explicit GpuTilePool(UnifiedMemoryManager& memory_manager);
    ~GpuTilePool();

    GpuTilePool(const GpuTilePool&) = delete;
    GpuTilePool& operator=(const GpuTilePool&) = delete;

    // Allocates an OS-backed physical tile pool of `tile_count` 64 KiB tiles.
    Status CreatePool(uint32_t tile_count, uint32_t flags, uint64_t& out_pool_id);

    // Allocates an OS-backed physical tile pool of `super_tile_count` 4 MiB super-tiles
    // (equivalently `super_tile_count * 64` standard 64 KiB tiles with 4 MiB granule).
    Status CreateSuperTilePool(uint32_t super_tile_count, uint32_t flags, uint64_t& out_pool_id);

    Status DestroyPool(uint64_t pool_id);
    bool GetPoolTileCount(uint64_t pool_id, uint32_t& out_tile_count) const;

    // Reserves a contiguous 2D virtual tile address space
    // (`width_tiles * height_tiles * tile_size_bytes`) backed by an OS placeholder.
    // `tile_size_bytes` must be either `kGpuTileSizeBytes` (64 KiB) or `kGpuSuperTileSizeBytes` (4 MiB).
    Status ReserveTiledResource(uint32_t width_tiles,
                                uint32_t height_tiles,
                                uint64_t& out_resource_id,
                                size_t tile_size_bytes = kGpuTileSizeBytes);
    Status ReleaseTiledResource(uint64_t resource_id);
    bool GetResourceDimensions(uint64_t resource_id,
                               uint32_t& out_width_tiles,
                               uint32_t& out_height_tiles,
                               void*& out_virtual_base) const;

    // Maps, remaps, aliases, or unmaps (kTileMapFlagNull / pool_id == 0) a row-major span of
    // virtual tiles starting at `tile_coord`. Multiple virtual tiles (within the same resource
    // or across different resources) may map to the exact same physical tile simultaneously.
    // For 4 MiB tiled resources, `pool_tile_offset` is in 4 MiB super-tile units.
    Status UpdateTileMappings(uint64_t resource_id,
                              GpuTileCoord tile_coord,
                              uint64_t pool_id,
                              uint32_t pool_tile_offset,
                              uint32_t tile_count,
                              uint32_t flags = kTileMapFlagNone);

    // Reads/writes bytes directly through the mapped virtual address of the given tile.
    Status WriteTileBytes(uint64_t resource_id,
                          GpuTileCoord tile_coord,
                          size_t byte_offset,
                          const void* src,
                          size_t byte_count);
    Status ReadTileBytes(uint64_t resource_id,
                         GpuTileCoord tile_coord,
                         size_t byte_offset,
                         void* dst,
                         size_t byte_count) const;

    // Live tile mapping inspection and telemetry.
    Status QueryTileMapping(uint64_t resource_id,
                            GpuTileCoord tile_coord,
                            GpuTileMappingInfo& out_info) const;
    size_t TotalPhysicalTiles() const;
    size_t MappedVirtualTiles() const;
    size_t AliasedTileCount() const;
    GpuTilePoolStats GetStats() const;

private:
    struct PhysicalTileKey {
        uint64_t pool_id = 0;
        uint32_t tile_offset = 0;
        size_t tile_size_bytes = kGpuTileSizeBytes;

        bool operator==(const PhysicalTileKey& other) const noexcept {
            return pool_id == other.pool_id &&
                   tile_offset == other.tile_offset &&
                   tile_size_bytes == other.tile_size_bytes;
        }
    };

    struct PhysicalTileKeyHash {
        size_t operator()(const PhysicalTileKey& k) const noexcept {
            const uint64_t h = k.pool_id * 0x9E3779B97F4A7C15ull ^
                               (static_cast<uint64_t>(k.tile_offset) + 0x517CC1B727220A95ull) ^
                               (static_cast<uint64_t>(k.tile_size_bytes) << 17);
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };

    struct PoolEntry {
        uint64_t pool_id = 0;
        uint64_t physical_id = 0;
        uint32_t tile_count = 0;       // Count in 64 KiB standard tiles
        uint32_t pool_flags = kTilePoolFlagNone;
        uint32_t mem_flags = kMemFlagGraphics;
        size_t physical_bytes = 0;
        size_t active_mappings = 0;
    };

    struct VirtualTileSlot {
        bool mapped = false;
        uint64_t pool_id = 0;
        uint32_t pool_tile_offset = 0;
        void* virtual_address = nullptr;
        GpuVirtualAddress gpu_va = 0;
        size_t tile_size_bytes = kGpuTileSizeBytes;
        uint32_t flags = kTileMapFlagNone;
    };

    struct TiledResource {
        uint64_t resource_id = 0;
        uint32_t width_tiles = 0;
        uint32_t height_tiles = 0;
        size_t tile_size_bytes = kGpuTileSizeBytes;
        bool use_placeholder = false;
        void* virtual_base = nullptr;
        std::vector<VirtualTileSlot> slots;
    };

    Status UnmapSlotLocked(VirtualTileSlot& slot);
    void DecrementPhysicalRefLocked(uint64_t pool_id, uint32_t tile_offset, size_t tile_size_bytes);
    void IncrementPhysicalRefLocked(uint64_t pool_id, uint32_t tile_offset, size_t tile_size_bytes);

    UnifiedMemoryManager& memory_;
    mutable std::mutex mutex_;
    uint64_t next_pool_id_ = 1;
    uint64_t next_resource_id_ = 1;
    size_t total_physical_tiles_ = 0;
    size_t total_physical_bytes_ = 0;
    size_t mapped_virtual_tiles_ = 0;
    size_t aliased_tile_count_ = 0;
    std::unordered_map<uint64_t, PoolEntry> pools_;
    std::unordered_map<uint64_t, TiledResource> resources_;
    std::unordered_map<PhysicalTileKey, uint32_t, PhysicalTileKeyHash> phys_tile_refcounts_;
};

}  // namespace openscarlett
