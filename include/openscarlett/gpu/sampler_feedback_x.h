#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "openscarlett/core/status.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace openscarlett::d3d12_xs {

// Portable DXGI Sampler Feedback opaque format constants (DXGI 1.6 / D3D12 Ultimate).
inline constexpr uint32_t kDxgiFormatSamplerFeedbackMinMipOpaqueValue = 189u;
inline constexpr uint32_t kDxgiFormatSamplerFeedbackMipRegionUsedOpaqueValue = 190u;

// Console & D3D12 decoded sentinel values.
inline constexpr uint8_t kSamplerFeedbackUnreferencedMinMip = 0xFFu;
inline constexpr uint8_t kSamplerFeedbackRegionUnused = 0u;
inline constexpr uint8_t kSamplerFeedbackRegionUsed = 1u;

#if defined(_WIN32)
inline constexpr DXGI_FORMAT kDxgiFormatSamplerFeedbackMinMipOpaque =
    static_cast<DXGI_FORMAT>(kDxgiFormatSamplerFeedbackMinMipOpaqueValue);
inline constexpr DXGI_FORMAT kDxgiFormatSamplerFeedbackMipRegionUsedOpaque =
    static_cast<DXGI_FORMAT>(kDxgiFormatSamplerFeedbackMipRegionUsedOpaqueValue);

inline constexpr bool IsSamplerFeedbackFormat(DXGI_FORMAT fmt) noexcept {
    const uint32_t v = static_cast<uint32_t>(fmt);
    return v == kDxgiFormatSamplerFeedbackMinMipOpaqueValue ||
           v == kDxgiFormatSamplerFeedbackMipRegionUsedOpaqueValue;
}
#endif

// Mirrors D3D12_SAMPLER_FEEDBACK_TIER without requiring newer Windows SDK / MinGW enum definitions.
enum class SamplerFeedbackTier : uint32_t {
    NotSupported = 0u,
    Tier0_9 = 90u,
    Tier1_0 = 100u,
};

enum class SamplerFeedbackKind : uint32_t {
    MinMipOpaque = 0u,
    MipRegionUsedOpaque = 1u,
};

// Portable equivalent of D3D12_MIP_REGION.
struct SamplerFeedbackMipRegion {
    uint32_t width = 16u;
    uint32_t height = 16u;
    uint32_t depth = 1u;

#if defined(_WIN32) && defined(__ID3D12Device8_INTERFACE_DEFINED__)
    static constexpr SamplerFeedbackMipRegion FromD3D12(const D3D12_MIP_REGION& r) noexcept {
        return {r.Width, r.Height, r.Depth};
    }
    constexpr D3D12_MIP_REGION ToD3D12() const noexcept {
        return {width, height, depth};
    }
#endif
};

struct SamplerFeedbackMapDesc {
    SamplerFeedbackKind kind = SamplerFeedbackKind::MinMipOpaque;
    uint32_t target_width = 256u;
    uint32_t target_height = 256u;
    uint16_t target_array_size = 1u;
    uint16_t target_mip_levels = 1u;
    SamplerFeedbackMipRegion mip_region{16u, 16u, 1u};
    // Forces GPU-compute fallback even on Tier 0.9/1.0 hosts (for parity validation & debugging).
    bool force_software_fallback = false;
};

// Individual tile feedback write (used by fallback compute writes and native ENCODE synthesis).
struct SamplerFeedbackSampleWrite {
    uint32_t tile_x = 0u;
    uint32_t tile_y = 0u;
    uint32_t array_slice = 0u;
    uint8_t mip_level = 0u;
};

struct SamplerFeedbackMapInfo {
    uint64_t map_id = 0ull;
    SamplerFeedbackKind kind = SamplerFeedbackKind::MinMipOpaque;
    SamplerFeedbackTier host_tier = SamplerFeedbackTier::NotSupported;
    bool using_native_hardware = false;
    uint32_t target_width = 0u;
    uint32_t target_height = 0u;
    uint16_t target_array_size = 1u;
    uint16_t target_mip_levels = 1u;
    SamplerFeedbackMipRegion mip_region{};
    uint32_t map_tiles_x = 0u;
    uint32_t map_tiles_y = 0u;
    uint64_t clear_count = 0ull;
    uint64_t resolve_count = 0ull;
    uint64_t view_count = 0ull;
#if defined(_WIN32)
    ID3D12Resource* feedback_resource = nullptr;
    ID3D12Resource* paired_target_resource = nullptr;
#endif
};

struct SamplerFeedbackViewRecord {
    uint64_t map_id = 0ull;
    size_t dest_cpu_descriptor_ptr = 0u;
    bool is_native_view = false;
    SamplerFeedbackKind kind = SamplerFeedbackKind::MinMipOpaque;
#if defined(_WIN32)
    ID3D12Resource* feedback_resource = nullptr;
    ID3D12Resource* paired_target_resource = nullptr;
#endif
};

struct SamplerFeedbackMinMipReadback {
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    uint32_t array_size = 1u;
    bool used_native_resolve = false;
    // Row-major [array_slice][y * tiles_x + x]: 0xFF = unreferenced, 0..15 = min accessed mip.
    std::vector<uint8_t> min_mip_tiles;

    uint8_t At(uint32_t x, uint32_t y, uint32_t slice = 0u) const noexcept {
        if (x >= tiles_x || y >= tiles_y || slice >= array_size) {
            return kSamplerFeedbackUnreferencedMinMip;
        }
        return min_mip_tiles[(static_cast<size_t>(slice) * tiles_y + y) * tiles_x + x];
    }
};

struct SamplerFeedbackMipRegionUsedPlane {
    uint32_t mip_level = 0u;
    uint32_t array_slice = 0u;
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    // Row-major [y * tiles_x + x]: 0 = unused, 1 = used.
    std::vector<uint8_t> used_flags;

    uint8_t At(uint32_t x, uint32_t y) const noexcept {
        if (x >= tiles_x || y >= tiles_y) return kSamplerFeedbackRegionUnused;
        return used_flags[static_cast<size_t>(y) * tiles_x + x];
    }
};

struct SamplerFeedbackMipRegionUsedReadback {
    uint32_t base_tiles_x = 0u;
    uint32_t base_tiles_y = 0u;
    uint32_t array_size = 1u;
    uint32_t mip_levels = 1u;
    bool used_native_resolve = false;
    // Per-base-tile bitmask (bit m == 1 iff mip m covering that base tile was sampled).
    std::vector<uint32_t> tile_mip_bitmasks;
    // Per-(slice, mip) 2D decoded planes (0 = unused, 1 = used).
    std::vector<SamplerFeedbackMipRegionUsedPlane> mip_planes;

    uint32_t BitmaskAt(uint32_t x, uint32_t y, uint32_t slice = 0u) const noexcept {
        if (x >= base_tiles_x || y >= base_tiles_y || slice >= array_size) return 0u;
        return tile_mip_bitmasks[(static_cast<size_t>(slice) * base_tiles_y + y) * base_tiles_x + x];
    }
};

struct SamplerFeedbackManagerStats {
    SamplerFeedbackTier host_tier = SamplerFeedbackTier::NotSupported;
    bool has_device8 = false;
    uint32_t active_maps = 0u;
    uint32_t native_maps = 0u;
    uint32_t fallback_maps = 0u;
    uint32_t preserved_views = 0u;
    uint64_t total_clears = 0ull;
    uint64_t total_resolves = 0ull;
};

// Console-way Sampler Feedback manager ready to embed as a member of DeviceX.
// - Probes D3D12_FEATURE_DATA_D3D12_OPTIONS7::SamplerFeedbackTier & ID3D12Device8.
// - Uses native ID3D12Device8::CreateCommittedResource2, CreateSamplerFeedbackUnorderedAccessView,
//   and ID3D12GraphicsCommandList1::ResolveSubresourceRegion when supported on hardware.
// - Transparently falls back to GPU UAV-buffer + HLSL compute clear/write/readback on desktop
//   GPUs or WARP lacking native Sampler Feedback Tier 0.9/1.0 so games never fail.
class SamplerFeedbackManager {
public:
    SamplerFeedbackManager();
    ~SamplerFeedbackManager();

    SamplerFeedbackManager(const SamplerFeedbackManager&) = delete;
    SamplerFeedbackManager& operator=(const SamplerFeedbackManager&) = delete;

#if defined(_WIN32)
    // Initializes manager against a host ID3D12Device and probes Sampler Feedback tier.
    Status Initialize(ID3D12Device* host_device);
#endif
    void Shutdown();
    bool IsInitialized() const noexcept;

    SamplerFeedbackTier HostTier() const noexcept;
    bool HasNativeSamplerFeedback() const noexcept;
    bool HasDevice8() const noexcept;
    SamplerFeedbackManagerStats GetStats() const;

#if defined(_WIN32)
    // Static feature probe helper usable independently of an initialized manager.
    static SamplerFeedbackTier ProbeHostSamplerFeedbackTier(ID3D12Device* host_device) noexcept;

    // Creates a paired feedback map texture (native opaque texture on Tier 0.9/1.0 or GPU UAV
    // fallback buffer otherwise) and registers its pairing with paired_target.
    // Caller receives an owning reference in *out_feedback_resource.
    Status CreateFeedbackMap(
        const SamplerFeedbackMapDesc& desc,
        ID3D12Resource* paired_target,
        ID3D12Resource** out_feedback_resource,
        SamplerFeedbackMapInfo* out_info = nullptr);

    // Registers an externally created feedback resource (hook for DeviceX::CreateCommittedResource2).
    Status RegisterExternalFeedbackMap(
        ID3D12Resource* feedback_resource,
        ID3D12Resource* paired_target,
        const SamplerFeedbackMapDesc& desc,
        bool is_native_resource,
        SamplerFeedbackMapInfo* out_info = nullptr);

    Status UnregisterFeedbackMap(ID3D12Resource* feedback_resource);

    // Lifecycle hook matching DeviceX::NotifyResourceDestroyed.
    void NotifyResourceDestroyed(ID3D12Resource* host_resource) noexcept;

    bool LookupFeedbackMap(
        ID3D12Resource* feedback_resource,
        SamplerFeedbackMapInfo& out_info) const;

    // Preserves sampler feedback UAVs cleanly on Desktop D3D12:
    // - Native map: invokes ID3D12Device8::CreateSamplerFeedbackUnorderedAccessView.
    // - Fallback map: creates a valid R32_UINT buffer UAV on the backing GPU buffer.
    // Always records the pairing metadata for LookupPreservedView / CopyPreservedView.
    Status CreateSamplerFeedbackUnorderedAccessView(
        ID3D12Resource* targeted_resource,
        ID3D12Resource* feedback_resource,
        D3D12_CPU_DESCRIPTOR_HANDLE dest_descriptor,
        SamplerFeedbackViewRecord* out_record = nullptr);

    bool LookupPreservedView(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
        SamplerFeedbackViewRecord& out_record) const;

    void CopyPreservedView(
        D3D12_CPU_DESCRIPTOR_HANDLE dst_handle,
        D3D12_CPU_DESCRIPTOR_HANDLE src_handle);

    // Xbox console-style feedback map clear recorded onto cmd_list:
    // - MinMipOpaque -> 0xFF (unreferenced)
    // - MipRegionUsedOpaque -> 0 (unused)
    Status ClearSamplerFeedbackMap(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* feedback_resource,
        D3D12_RESOURCE_STATES state_before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATES state_after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Records deterministic GPU feedback sample writes onto cmd_list (using HLSL compute
    // InterlockedMin/InterlockedOr on fallback maps, or ResolveSubresourceRegion ENCODE on native).
    Status RecordFeedbackSampleWrites(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* feedback_resource,
        const std::vector<SamplerFeedbackSampleWrite>& samples,
        D3D12_RESOURCE_STATES state_before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATES state_after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Records decode/resolve commands into an R8_UINT destination texture (native DECODE resolve)
    // or copies/decodes fallback UAV data into a caller-provided destination buffer/texture.
    Status RecordResolveToR8Texture(
        ID3D12GraphicsCommandList* cmd_list,
        ID3D12Resource* feedback_resource,
        ID3D12Resource* dst_r8_texture,
        D3D12_RESOURCE_STATES feedback_state_before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATES feedback_state_after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATES dst_state_before = D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATES dst_state_after = D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Executes GPU resolve + synchronous readback on queue, decoding MIN_MIP_OPAQUE into
    // deterministic per-tile uint8 min-mip values (0xFF = unreferenced).
    Status ResolveAndReadbackMinMip(
        ID3D12CommandQueue* queue,
        ID3D12Resource* feedback_resource,
        SamplerFeedbackMinMipReadback& out_readback,
        D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Executes GPU resolve + synchronous readback on queue, decoding MIP_REGION_USED_OPAQUE into
    // per-mip 2D used-flag planes AND compact per-base-tile mip bitmasks.
    Status ResolveAndReadbackMipRegionUsed(
        ID3D12CommandQueue* queue,
        ID3D12Resource* feedback_resource,
        SamplerFeedbackMipRegionUsedReadback& out_readback,
        D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett::d3d12_xs
