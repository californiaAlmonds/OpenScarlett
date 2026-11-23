#pragma once

#include <cstdint>

#include "openscarlett/gpu/xg_xs_abi.h"

namespace openscarlett::xg {

// Selects how 3D GFX10 SW_64KB_* mip chains lay out tail mips smaller than half a 3D tile:
//  - PackedMipTail: packs eligible small 3D mips into a single 64 KiB mip-tail tile block.
//  - UnpackedTiledMips: gives every 3D mip level its own full tile-aligned 3D subresource region.
enum class Xg3dMipTailMode : uint32_t {
    PackedMipTail = 0u,
    UnpackedTiledMips = 1u,
};

struct Xg3dMipChainSummary {
    uint32_t mip_levels = 0u;
    uint32_t packed_mip_tail_start_level = 0u;
    uint32_t packed_mip_count = 0u;
    uint32_t unpacked_mip_count = 0u;
    uint32_t base_alignment_bytes = 0u;
    uint64_t mip_tail_offset_bytes = 0ull;
    uint64_t mip_tail_size_bytes = 0ull;
    uint64_t total_size_bytes = 0ull;
};

// Computes a GFX10 3D texture layout with explicit packed vs. unpacked 3D mip-tail control
// and full 64-bit overflow & bounds checking (TuxBox 949862a / d2e5108).
bool ComputeTexture3DLayoutEx(
    const XG_TEXTURE3D_DESC& desc,
    Xg3dMipTailMode mip_tail_mode,
    XG_RESOURCE_LAYOUT& out_layout,
    Xg3dMipChainSummary* out_summary = nullptr) noexcept;

// Validates that every mip level and packed mip-tail sub-block in a 3D layout stays
// strictly within [0, layout.SizeBytes] without overlap or overflow.
bool ValidateTexture3DLayoutBounds(const XG_RESOURCE_LAYOUT& layout) noexcept;

// Lossless 3D tile/untile using an explicit precomputed XG_RESOURCE_LAYOUT
// (works identically for both packed 3D mip tails and unpacked tiled 3D mip chains).
bool TileSurface3DWithLayout(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    uint32_t src_slice_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileSurface3DWithLayout(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes,
    uint32_t dst_slice_pitch_bytes) noexcept;

}  // namespace openscarlett::xg
