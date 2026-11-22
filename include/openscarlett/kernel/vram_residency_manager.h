#pragma once

// OpenScarlett Sprint 12 — Streaming, BCPack/Metadata Coherency, Physical Page Aliasing,
// and LRU VRAM Residency Management Public Interfaces.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/core/types.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/bcpack.h"
#include "openscarlett/io/dstorage_abi.h"
#include "openscarlett/kernel/title_va_mirror.h"

#if defined(_WIN32)
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
#endif

// ============================================================================
// 1. DirectStorage Sprint 12 Extensions (dstorage_xs.dll)
//    - Custom decompression callbacks (IDStorageCustomDecompressionQueue-style)
//    - Priority queue scheduling telemetry (REALTIME > HIGH > NORMAL > LOW)
//    - Automatic BCPack/GDeflate -> XgGpuUntiler -> D3D12 texture GPU upload
// ============================================================================

constexpr uint8_t OS_DSTORAGE_CUSTOM_COMPRESSION_0 = 0x80u;
constexpr uint32_t OS_DSTORAGE_TEXTURE_DEST_MAGIC = 0x54584453u;  // 'SDXT'

using PfnOpenScarlettDStorageCustomDecompress = int32_t(OPENSCARLETT_DSTORAGE_CALL*)(
    uint8_t format,
    const void* src,
    size_t src_size,
    void* dst,
    size_t dst_size,
    size_t* out_decompressed_size,
    void* user_context);

struct OpenScarlettDStorageTextureDestDesc {
    uint32_t struct_magic = OS_DSTORAGE_TEXTURE_DEST_MAGIC;
    uint32_t struct_size = static_cast<uint32_t>(sizeof(OpenScarlettDStorageTextureDestDesc));
    void* d3d12_resource = nullptr;        // Target ID3D12Resource* (DEFAULT heap texture)
    void* d3d12_device = nullptr;          // Optional ID3D12Device* (falls back to queue Device or Resource::GetDevice)
    uint32_t subresource_index = 0u;
    uint32_t mip_level = 0u;
    uint32_t array_slice = 0u;
    uint32_t plane_slice = 0u;
    uint32_t width = 0u;                   // Texels (0 => query from resource desc)
    uint32_t height = 0u;                  // Texels (0 => query from resource desc)
    uint32_t dxgi_format = 0u;             // 0 => query from resource desc
    uint32_t src_tile_mode = 0u;           // XG_TILE_MODE (0 = LINEAR; non-zero routes through XgGpuUntiler)
    uint32_t src_row_pitch = 0u;           // 0 = tightly packed
    uint32_t console_type = 0u;            // XG_CONSOLE_TYPE
    uint32_t metadata_mode = 0u;           // XgGpuMetadataMode
    const void* metadata_buffer = nullptr;
    uint32_t metadata_size_bytes = 0u;
    uint32_t clear_reg_value[4] = {0u, 0u, 0u, 0u};
    uint32_t initial_resource_state = 0u;  // D3D12_RESOURCE_STATES before copy (0 = COMMON)
    uint32_t final_resource_state = 0u;    // D3D12_RESOURCE_STATES after copy (0 = COMMON)
    uint8_t prefer_gpu_untiler = 1u;
    uint8_t allow_cpu_untile_fallback = 1u;
    uint8_t reserved[2] = {0u, 0u};
    // Optional output telemetry populated on request completion.
    uint32_t out_used_gpu_untiler = 0u;
    uint32_t out_decompressed_bytes = 0u;
    uint32_t out_untiled_bytes = 0u;
    uint32_t out_uploaded_bytes = 0u;
};

struct OpenScarlettDStoragePriorityStats {
    uint64_t realtime_dispatched = 0u;
    uint64_t high_dispatched = 0u;
    uint64_t normal_dispatched = 0u;
    uint64_t low_dispatched = 0u;
    uint64_t custom_decompressed = 0u;
    uint64_t gpu_texture_uploads = 0u;
    uint64_t gpu_untiled_uploads = 0u;
};

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL
OpenScarlettDStorageRegisterCustomDecompressor(
    uint8_t format,
    PfnOpenScarlettDStorageCustomDecompress callback,
    void* user_context);

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL
OpenScarlettDStorageUnregisterCustomDecompressor(uint8_t format);

OPENSCARLETT_DSTORAGE_API int32_t OPENSCARLETT_DSTORAGE_CALL
OpenScarlettDStorageGetPriorityStats(OpenScarlettDStoragePriorityStats* out_stats);

#ifdef __cplusplus
}
#endif

namespace openscarlett {

// ============================================================================
// 2. BCPack Texture-Array + Multi-Mip & Stream Validation Extensions
//    (Implemented in src/io/bcpack.cpp)
// ============================================================================

static constexpr uint32_t kBcPackTextureArrayMagic = 0x41504342u;  // 'BCPA' little-endian
static constexpr uint32_t kBcPackMaxArraySlices = 2048u;

struct BcPackTextureArrayHeader {
    uint32_t magic = kBcPackTextureArrayMagic;
    uint16_t version = kBcPackVersion;
    uint16_t flags = 0u;
    BcFormat format = BcFormat::Unknown;
    uint32_t base_width = 0u;
    uint32_t base_height = 0u;
    uint32_t mip_count = 0u;
    uint32_t array_size = 0u;
    std::vector<uint32_t> subresource_offsets;
    std::vector<uint32_t> subresource_compressed_sizes;
};

// Validates a BCPack single-surface ('BCPK'), mip-chain ('BCPM'), or texture-array ('BCPA')
// header, predictor/transform/codec fields, and payload bounds without huge allocations.
Status BcPackValidateStream(const uint8_t* src, size_t src_size) noexcept;

// Compresses a BC1..BC7 texture array with optional multi-mip chain.
// `subresources` must have `array_size * mip_count` entries indexed as `slice * mip_count + mip`.
Status BcPackCompressTextureArray(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    uint32_t mip_count,
    uint32_t array_size,
    const std::vector<std::vector<uint8_t>>& subresources,
    std::vector<uint8_t>& out_compressed);

Status BcPackParseTextureArrayHeader(
    const uint8_t* src,
    size_t src_size,
    BcPackTextureArrayHeader& out_header);

Status BcPackDecompressTextureArray(
    const uint8_t* src,
    size_t src_size,
    std::vector<std::vector<uint8_t>>& out_subresources,
    BcPackTextureArrayHeader* out_header = nullptr);

Status BcPackDecompressTextureArraySubresource(
    const uint8_t* src,
    size_t src_size,
    uint32_t array_slice,
    uint32_t mip_level,
    std::vector<uint8_t>& out_blocks);

// ============================================================================
// 3. Direct D3D12 / Vulkan HTILE, CMASK & DCC Fast-Clear Metadata Coherency
//    (Implemented in src/gpu/xg_gpu_untile.cpp)
// ============================================================================

struct XgFastClearRect {
    uint32_t left = 0u;
    uint32_t top = 0u;
    uint32_t right = 0u;   // 0 => full width
    uint32_t bottom = 0u;  // 0 => full height
};

struct XgFastClearMetadataSyncStats {
    uint32_t total_meta_blocks = 0u;
    uint32_t fast_cleared_blocks_expanded = 0u;
    uint32_t uncompressed_blocks_preserved = 0u;
    bool executed_on_gpu = false;
    std::string backend_name;
};

// Encodes RGBA float clear color or depth/stencil clear values into format-native 16-byte clear_reg.
void EncodeFormatFastClearRegister(
    uint32_t dxgi_format,
    const float rgba_clear[4],
    float depth_clear,
    uint8_t stencil_clear,
    std::array<uint32_t, 4>& out_clear_reg) noexcept;

// Builds a coherent GFX10 DCC metadata buffer (pipe-aligned or linear) with `clear_state`
// applied inside `rect` (or full surface if rect.right == 0 || rect.bottom == 0) and
// uncompressed (0xFF) outside.
Status BuildDccFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t mip_level,
    uint32_t slice_index,
    bool pipe_aligned,
    XG_DCC_BLOCK_STATE clear_state,
    const XgFastClearRect& rect,
    std::vector<uint8_t>& out_dcc_bytes);

// Builds a coherent GFX10 HTILE metadata buffer (4 bytes per 8x8 pixel block) marking
// tiles inside `rect` as fast-cleared (zmask == 0) and tiles outside as expanded (zmask == 0xF).
Status BuildHtileFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t slice_index,
    const XgFastClearRect& rect,
    float depth_clear,
    uint8_t stencil_clear,
    std::vector<uint8_t>& out_htile_bytes,
    std::array<uint32_t, 4>& out_clear_reg);

// Builds a coherent GFX10 CMASK metadata buffer (4-bit nibble per 8x8 pixel block) marking
// tiles inside `rect` as fast-cleared (nibble == 0x0) and tiles outside as expanded (nibble == 0xF).
Status BuildCmaskFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t slice_index,
    const XgFastClearRect& rect,
    std::vector<uint8_t>& out_cmask_bytes);

// Resets a metadata buffer to fully expanded / uncompressed coherent state after fast-clear expand.
Status ResetFastClearMetadataToExpanded(
    XgGpuMetadataMode mode,
    std::vector<uint8_t>& io_meta_bytes) noexcept;

// Coherently expands HTILE / CMASK / DCC fast-clear blocks while untiling a console tiled surface
// into PC linear layout (via D3D12 / Vulkan XgGpuUntiler if ready, or CPU fallback), and resets
// `io_meta_bytes` to expanded state when `reset_metadata_after_expand` is true.
Status ExpandAndSyncFastClearMetadata2D(
    XgGpuUntiler* untiler,
    const XgGpuSurface2DRequest& req,
    const std::vector<uint8_t>& src_tiled_bytes,
    std::vector<uint8_t>& io_meta_bytes,
    std::vector<uint8_t>& out_linear_bytes,
    bool reset_metadata_after_expand = true,
    bool use_vulkan = false,
    XgFastClearMetadataSyncStats* out_stats = nullptr);

// ============================================================================
// 4. Physical Page Double-Mapping / Aliasing (64 KB / 2 MB / 4 MB)
//    (Implemented in src/kernel/title_va_mirror.cpp)
// ============================================================================

enum class PhysicalPageGranularity : uint32_t {
    Page64KB = kPageSize64KB,  // 65,536 bytes
    Page2MB = kPageSize2MB,    // 2,097,152 bytes
    Page4MB = kPageSize4MB,    // 4,194,304 bytes
};

struct PhysicalAliasMapping {
    uint64_t alias_id = 0ull;
    uint64_t phys_id = 0ull;
    PhysicalPageGranularity granularity = PhysicalPageGranularity::Page64KB;
    uint32_t first_page = 0u;
    uint32_t page_count = 0u;
    uint64_t size_bytes = 0ull;
    void* cpu_va = nullptr;
    uint64_t guest_title_va = 0ull;
};

struct PhysicalAliasStats {
    uint64_t active_physical_blocks = 0ull;
    uint64_t active_aliases = 0ull;
    uint64_t total_physical_bytes = 0ull;
    uint64_t total_alias_mappings_created = 0ull;
    uint64_t dirty_pages_marked = 0ull;
    uint64_t mirror_sync_flushes = 0ull;
};

#if defined(_WIN32)
// Manages OS-backed physical page blocks (64 KB, 2 MB, 4 MB) supporting true hardware
// double-mapping (multiple independent virtual addresses backed by identical physical pages)
// with coherent physical-page dirty tracking and TitleVAMirror synchronization.
class PhysicalPageAliasManager {
public:
    PhysicalPageAliasManager();
    ~PhysicalPageAliasManager();

    PhysicalPageAliasManager(const PhysicalPageAliasManager&) = delete;
    PhysicalPageAliasManager& operator=(const PhysicalPageAliasManager&) = delete;

    // Allocates `page_count` physical pages of size `granularity` (64 KB, 2 MB, or 4 MB).
    Status AllocatePhysicalBlock(
        PhysicalPageGranularity granularity,
        uint32_t page_count,
        uint64_t& out_phys_id);

    // Creates a new virtual address mapping aliasing `[first_page, first_page + page_count)`
    // of physical block `phys_id`. Multiple aliases see each other's writes coherently.
    Status MapAlias(
        uint64_t phys_id,
        uint32_t first_page,
        uint32_t page_count,
        uint64_t guest_title_va,
        PhysicalAliasMapping& out_mapping);

    Status UnmapAlias(uint64_t alias_id);
    Status FreePhysicalBlock(uint64_t phys_id);
    void Clear();

    // Coherent physical-page dirty tracking across all aliases of the same physical block.
    Status MarkAliasRangeDirty(uint64_t alias_id, uint64_t byte_offset, uint64_t byte_size);
    Status QueryDirtyPhysicalPages(
        uint64_t phys_id,
        bool reset_dirty,
        std::vector<uint32_t>& out_dirty_pages);

    // Synchronizes dirty physical ranges into a registered TitleVAMirror region.
    Status SyncDirtyPagesToTitleVaMirror(
        uint64_t phys_id,
        TitleVAMirror& mirror,
        uint64_t mirror_title_va,
        ID3D12GraphicsCommandList* cmd_list,
        FlushStats* out_flush_stats = nullptr);

    bool ResolveAliasByVa(const void* cpu_va, PhysicalAliasMapping& out_mapping) const;
    Status GetAliasesForBlock(uint64_t phys_id, std::vector<PhysicalAliasMapping>& out_aliases) const;
    PhysicalAliasStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif  // defined(_WIN32)

// ============================================================================
// 5. LRU VRAM Residency Manager (14+ GB Series X -> 8-12 GB PC GPU Budget)
//    (Implemented in src/kernel/vram_residency_manager.cpp)
// ============================================================================

struct VramResidencyConfig {
    // Hard VRAM residency budget in bytes (e.g., 8 GiB on desktop or 8-16 MiB in tests).
    uint64_t vram_budget_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    // When true, evicting an allocation calls ID3D12Device::Evict AND releases the GPU resource
    // after copying contents to system RAM (guaranteeing real VRAM reclamation on all drivers).
    bool release_gpu_resource_on_evict = false;
    // Automatically evict LRU unpinned allocations on Allocate/EnsureResident when over budget.
    bool auto_evict_on_pressure = true;
};

struct VramAllocationDesc {
    uint64_t size_bytes = 0ull;
    uint64_t alignment = kPageSize64KB;
    std::string debug_name;
    bool start_resident = true;
    bool start_pinned = false;
    bool allow_uav = false;
};

struct VramAllocationInfo {
    uint64_t handle = 0ull;
    uint64_t size_bytes = 0ull;
    uint64_t aligned_size_bytes = 0ull;
    std::string debug_name;
    bool is_resident = false;
    uint32_t pin_count = 0u;
    uint64_t gpu_va = 0ull;
    uint64_t eviction_count = 0ull;
    uint64_t page_in_count = 0ull;
    uint64_t last_touch_tick = 0ull;
};

struct VramResidencyStats {
    uint64_t budget_bytes = 0ull;
    uint64_t resident_bytes = 0ull;
    uint64_t evicted_bytes = 0ull;
    uint64_t total_virtual_bytes = 0ull;
    uint64_t pinned_bytes = 0ull;
    uint64_t peak_resident_bytes = 0ull;
    uint64_t peak_virtual_bytes = 0ull;
    uint64_t active_allocations = 0ull;
    uint64_t resident_allocations = 0ull;
    uint64_t evicted_allocations = 0ull;
    uint64_t total_allocations = 0ull;
    uint64_t total_evictions = 0ull;
    uint64_t total_page_ins = 0ull;
    uint64_t total_bytes_paged_out = 0ull;
    uint64_t total_bytes_paged_in = 0ull;
    uint64_t d3d12_evict_calls = 0ull;
    uint64_t d3d12_make_resident_calls = 0ull;
    uint64_t oom_prevented_count = 0ull;
    bool using_d3d12_device = false;
    bool is_warp = false;
    std::string adapter_name;
};

class VramResidencyManager {
public:
    VramResidencyManager();
    ~VramResidencyManager();

    VramResidencyManager(const VramResidencyManager&) = delete;
    VramResidencyManager& operator=(const VramResidencyManager&) = delete;

#if defined(_WIN32)
    // Initializes with an optional host ID3D12Device* (hardware GPU or WARP).
    // Passing `device == nullptr` operates in pure CPU/synthetic residency mode.
    Status Initialize(ID3D12Device* device, const VramResidencyConfig& config);
#else
    Status Initialize(void* device, const VramResidencyConfig& config);
#endif

    void Shutdown();

    Status SetBudgetBytes(uint64_t new_budget_bytes);

    Status Allocate(const VramAllocationDesc& desc, uint64_t& out_handle);
    Status Free(uint64_t handle);

    // Marks allocation MRU and transparently pages it back into VRAM if currently evicted.
    Status Touch(uint64_t handle);
    Status EnsureResident(uint64_t handle);

    // Ref-counted pinning prevents eviction while an allocation is bound to active GPU work.
    Status Pin(uint64_t handle);
    Status Unpin(uint64_t handle);

    // Evicts LRU unpinned resident allocations until resident_bytes <= target_budget_bytes.
    // Preserves exact byte contents in system RAM backing store before evicting from GPU VRAM.
    Status EvictToBudget(
        uint64_t target_budget_bytes,
        uint64_t* out_evicted_count = nullptr,
        uint64_t* out_evicted_bytes = nullptr);

    // Byte-exact GPU VRAM <-> system RAM data transfer helpers.
    // WriteData ensures residency and uploads to GPU VRAM; ReadData verifies/reads from GPU
    // VRAM when resident (via real D3D12 readback) or from system RAM backing store when evicted.
    Status WriteData(uint64_t handle, uint64_t offset, const void* data, uint64_t size_bytes);
    Status ReadData(uint64_t handle, uint64_t offset, void* out_data, uint64_t size_bytes, bool ensure_resident = false);

#if defined(_WIN32)
    // Returns the live D3D12 resource pointer if resident (nullptr if evicted or headless).
    ID3D12Resource* GetResidentD3D12Resource(uint64_t handle) const;
#endif

    bool GetAllocationInfo(uint64_t handle, VramAllocationInfo& out_info) const;
    VramResidencyStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett
