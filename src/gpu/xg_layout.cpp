#include "openscarlett/gpu/xg_xs_abi.h"

#include <algorithm>
#include <cstring>

namespace openscarlett::xg {

namespace {

// Xbox Scarlett / GDK extended 16-bit depth-stencil DXGI format ordinals (public GDK docs).
constexpr uint32_t kXboxDxgiFormatD16UnormS8Uint = 118u;
constexpr uint32_t kXboxDxgiFormatR16UnormX8Typeless = 119u;
constexpr uint32_t kXboxDxgiFormatX16TypelessG8Uint = 120u;

// D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL bit (0x2) does not overlap XG_BIND_FLAGS bits
// (0x8, 0x20, 0x40, 0x80, 0x100, 0x200), allowing callers to pass either flag cleanly.
constexpr uint32_t kD3D12AllowDepthStencilBit = 0x2u;

constexpr bool HasDepthStencilBindFlag(uint32_t bind_flags) noexcept {
    return (bind_flags & (XG_BIND_DEPTH_STENCIL | kD3D12AllowDepthStencilBit)) != 0u;
}

// Maps stencil-only DXGI / Xbox view formats onto their full 2-plane parent depth-stencil format.
// Preserves standalone 1-plane SRV view for DXGI_FORMAT_X32_TYPELESS_G8X24_UINT (22) when
// BindFlags == XG_BIND_SHADER_RESOURCE explicitly, while promoting full planar depth-stencil
// layout for 22 (with depth-stencil or default bind), 47 (X24_TYPELESS_G8_UINT), and 120
// (Xbox X16_TYPELESS_G8_UINT).
uint32_t ResolvePlanarSurfaceFormat(uint32_t dxgi_format, uint32_t bind_flags) noexcept {
    switch (dxgi_format) {
        case static_cast<uint32_t>(DXGI_FORMAT_X24_TYPELESS_G8_UINT):  // 47
            return static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT);
        case kXboxDxgiFormatX16TypelessG8Uint:  // 120
            return kXboxDxgiFormatD16UnormS8Uint;
        case static_cast<uint32_t>(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT):  // 22
            if (bind_flags == XG_BIND_SHADER_RESOURCE) {
                return dxgi_format;
            }
            return static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
        default:
            return dxgi_format;
    }
}

constexpr uint32_t AlignUpU32(uint32_t value, uint32_t alignment) noexcept {
    return (alignment <= 1u) ? value : ((value + alignment - 1u) & ~(alignment - 1u));
}

constexpr uint64_t AlignUpU64(uint64_t value, uint64_t alignment) noexcept {
    return (alignment <= 1ull) ? value : ((value + alignment - 1ull) & ~(alignment - 1ull));
}

constexpr uint32_t Log2Pow2(uint32_t v) noexcept {
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

constexpr uint32_t NextPow2(uint32_t v) noexcept {
    if (v <= 1u) return 1u;
    --v;
    v |= v >> 1u;
    v |= v >> 2u;
    v |= v >> 4u;
    v |= v >> 8u;
    v |= v >> 16u;
    return v + 1u;
}

// Rectangular 2D Morton (Z-order) encoder:
// Strictly bijective over [0, 2^bits_x) x [0, 2^bits_y) -> [0, 2^(bits_x + bits_y)).
constexpr uint32_t MortonEncode2D(
    uint32_t x,
    uint32_t y,
    uint32_t bits_x,
    uint32_t bits_y,
    bool y_first = false) noexcept {
    uint32_t result = 0u;
    uint32_t out_bit = 0u;
    const uint32_t common = (bits_x < bits_y) ? bits_x : bits_y;
    for (uint32_t i = 0u; i < common; ++i) {
        const uint32_t bx = (x >> i) & 1u;
        const uint32_t by = (y >> i) & 1u;
        if (!y_first) {
            result |= (bx << out_bit++);
            result |= (by << out_bit++);
        } else {
            result |= (by << out_bit++);
            result |= (bx << out_bit++);
        }
    }
    for (uint32_t i = common; i < bits_x; ++i) {
        result |= (((x >> i) & 1u) << out_bit++);
    }
    for (uint32_t i = common; i < bits_y; ++i) {
        result |= (((y >> i) & 1u) << out_bit++);
    }
    return result;
}

// Rectangular 3D Morton (Z-order) encoder:
// Strictly bijective over [0, 2^bx) x [0, 2^by) x [0, 2^bz) -> [0, 2^(bx + by + bz)).
// axis_order: 0 => (x, y, z), 1 => (y, x, z), 2 => (z, x, y).
constexpr uint32_t MortonEncode3D(
    uint32_t x,
    uint32_t y,
    uint32_t z,
    uint32_t bx,
    uint32_t by,
    uint32_t bz,
    uint32_t axis_order = 0u) noexcept {
    uint32_t result = 0u;
    uint32_t out_bit = 0u;
    const uint32_t max_bits = std::max(bx, std::max(by, bz));
    for (uint32_t i = 0u; i < max_bits; ++i) {
        if (axis_order == 0u) {
            if (i < bx) result |= (((x >> i) & 1u) << out_bit++);
            if (i < by) result |= (((y >> i) & 1u) << out_bit++);
            if (i < bz) result |= (((z >> i) & 1u) << out_bit++);
        } else if (axis_order == 1u) {
            if (i < by) result |= (((y >> i) & 1u) << out_bit++);
            if (i < bx) result |= (((x >> i) & 1u) << out_bit++);
            if (i < bz) result |= (((z >> i) & 1u) << out_bit++);
        } else {
            if (i < bz) result |= (((z >> i) & 1u) << out_bit++);
            if (i < bx) result |= (((x >> i) & 1u) << out_bit++);
            if (i < by) result |= (((y >> i) & 1u) << out_bit++);
        }
    }
    return result;
}

enum class SwizzleFamily : uint8_t {
    Linear = 0,
    Standard = 1,     // _S / _S_X
    Displayable = 2,  // _D / _D_X
    DepthZ = 3,       // _Z / _Z_X
    Rotated = 4,      // _R / _R_X
};

struct TileGeometry {
    uint32_t tile_size_bytes = 256u;
    uint32_t tile_w_elems = 1u;
    uint32_t tile_h_elems = 1u;
    uint32_t tile_d_elems = 1u;
    uint32_t micro_w_elems = 1u;
    uint32_t micro_h_elems = 1u;
    uint32_t micro_size_bytes = 64u;
    uint32_t mip_tail_sub_align = 256u;
    SwizzleFamily family = SwizzleFamily::Linear;
    bool is_3d = false;
    bool has_xor = false;
    bool supports_mip_tail = false;
};

// Complete AMD GFX10 / RDNA2 2D thin (15 tiled modes) + 3D thick (6 tiled modes) + Linear
// tile geometries per bytes-per-element (1, 2, 4, 8, 16).
// Invariant: tile_w_elems * tile_h_elems * tile_d_elems * bpe == tile_size_bytes.
bool GetTileGeometry(XG_TILE_MODE mode, uint32_t bpe, TileGeometry& out) noexcept {
    out = TileGeometry{};
    if (bpe != 1u && bpe != 2u && bpe != 4u && bpe != 8u && bpe != 16u) {
        return false;
    }

    if (mode == XG_TILE_MODE_LINEAR) {
        out.tile_size_bytes = 256u;
        out.tile_w_elems = 1u;
        out.tile_h_elems = 1u;
        out.tile_d_elems = 1u;
        out.micro_w_elems = 1u;
        out.micro_h_elems = 1u;
        out.micro_size_bytes = bpe;
        out.family = SwizzleFamily::Linear;
        return true;
    }

    // 2D Thin 256B modes (S, D, R)
    if (mode == XG_TILE_MODE_2D_THIN_256B_S ||
        mode == XG_TILE_MODE_2D_THIN_256B_D ||
        mode == XG_TILE_MODE_2D_THIN_256B_R) {
        out.tile_size_bytes = 256u;
        out.micro_size_bytes = 64u;
        out.tile_d_elems = 1u;
        out.supports_mip_tail = false;
        out.family = (mode == XG_TILE_MODE_2D_THIN_256B_S)
                         ? SwizzleFamily::Standard
                         : ((mode == XG_TILE_MODE_2D_THIN_256B_D) ? SwizzleFamily::Displayable
                                                                  : SwizzleFamily::Rotated);
        switch (bpe) {
            case 1u:  out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.micro_w_elems = 8u; out.micro_h_elems = 8u; return true;
            case 2u:  out.tile_w_elems = 16u; out.tile_h_elems = 8u;  out.micro_w_elems = 8u; out.micro_h_elems = 4u; return true;
            case 4u:  out.tile_w_elems = 8u;  out.tile_h_elems = 8u;  out.micro_w_elems = 4u; out.micro_h_elems = 4u; return true;
            case 8u:  out.tile_w_elems = 8u;  out.tile_h_elems = 4u;  out.micro_w_elems = 4u; out.micro_h_elems = 2u; return true;
            case 16u: out.tile_w_elems = 4u;  out.tile_h_elems = 4u;  out.micro_w_elems = 2u; out.micro_h_elems = 2u; return true;
            default: return false;
        }
    }

    // 2D Thin 4KB modes (Z, S, D, R)
    if (mode == XG_TILE_MODE_2D_THIN_4KB_Z ||
        mode == XG_TILE_MODE_2D_THIN_4KB_S ||
        mode == XG_TILE_MODE_2D_THIN_4KB_D ||
        mode == XG_TILE_MODE_2D_THIN_4KB_R) {
        out.tile_size_bytes = 4096u;
        out.micro_size_bytes = 64u;
        out.tile_d_elems = 1u;
        out.mip_tail_sub_align = 64u;
        out.supports_mip_tail = true;
        switch (mode) {
            case XG_TILE_MODE_2D_THIN_4KB_Z: out.family = SwizzleFamily::DepthZ; break;
            case XG_TILE_MODE_2D_THIN_4KB_S: out.family = SwizzleFamily::Standard; break;
            case XG_TILE_MODE_2D_THIN_4KB_D: out.family = SwizzleFamily::Displayable; break;
            case XG_TILE_MODE_2D_THIN_4KB_R: out.family = SwizzleFamily::Rotated; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w_elems = 64u; out.tile_h_elems = 64u; out.micro_w_elems = 8u; out.micro_h_elems = 8u; return true;
            case 2u:  out.tile_w_elems = 64u; out.tile_h_elems = 32u; out.micro_w_elems = 8u; out.micro_h_elems = 4u; return true;
            case 4u:  out.tile_w_elems = 32u; out.tile_h_elems = 32u; out.micro_w_elems = 4u; out.micro_h_elems = 4u; return true;
            case 8u:  out.tile_w_elems = 32u; out.tile_h_elems = 16u; out.micro_w_elems = 4u; out.micro_h_elems = 2u; return true;
            case 16u: out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.micro_w_elems = 2u; out.micro_h_elems = 2u; return true;
            default: return false;
        }
    }

    // 2D Thin 64KB modes (Z, S, D, R, Z_X, S_X, D_X, R_X)
    if (mode == XG_TILE_MODE_2D_THIN_64KB_Z ||
        mode == XG_TILE_MODE_2D_THIN_64KB_S ||
        mode == XG_TILE_MODE_2D_THIN_64KB_D ||
        mode == XG_TILE_MODE_2D_THIN_64KB_R ||
        mode == XG_TILE_MODE_2D_THIN_64KB_Z_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_S_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_D_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_R_X) {
        out.tile_size_bytes = 65536u;
        out.micro_size_bytes = 256u;
        out.tile_d_elems = 1u;
        out.mip_tail_sub_align = 256u;
        out.supports_mip_tail = true;
        switch (mode) {
            case XG_TILE_MODE_2D_THIN_64KB_Z:   out.family = SwizzleFamily::DepthZ; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_S:   out.family = SwizzleFamily::Standard; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_D:   out.family = SwizzleFamily::Displayable; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_R:   out.family = SwizzleFamily::Rotated; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_Z_X: out.family = SwizzleFamily::DepthZ; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_S_X: out.family = SwizzleFamily::Standard; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_D_X: out.family = SwizzleFamily::Displayable; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_R_X: out.family = SwizzleFamily::Rotated; out.has_xor = true; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w_elems = 256u; out.tile_h_elems = 256u; out.micro_w_elems = 16u; out.micro_h_elems = 16u; return true;
            case 2u:  out.tile_w_elems = 256u; out.tile_h_elems = 128u; out.micro_w_elems = 16u; out.micro_h_elems = 8u;  return true;
            case 4u:  out.tile_w_elems = 128u; out.tile_h_elems = 128u; out.micro_w_elems = 8u;  out.micro_h_elems = 8u;  return true;
            case 8u:  out.tile_w_elems = 128u; out.tile_h_elems = 64u;  out.micro_w_elems = 8u;  out.micro_h_elems = 4u;  return true;
            case 16u: out.tile_w_elems = 64u;  out.tile_h_elems = 64u;  out.micro_w_elems = 4u;  out.micro_h_elems = 4u;  return true;
            default: return false;
        }
    }

    // 3D Thick 4KB mode (S)
    if (mode == XG_TILE_MODE_3D_THICK_4KB_S) {
        out.tile_size_bytes = 4096u;
        out.mip_tail_sub_align = 64u;
        out.family = SwizzleFamily::Standard;
        out.is_3d = true;
        out.has_xor = false;
        out.supports_mip_tail = true;
        switch (bpe) {
            case 1u:  out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.tile_d_elems = 16u; return true;
            case 2u:  out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.tile_d_elems = 8u;  return true;
            case 4u:  out.tile_w_elems = 16u; out.tile_h_elems = 8u;  out.tile_d_elems = 8u;  return true;
            case 8u:  out.tile_w_elems = 8u;  out.tile_h_elems = 8u;  out.tile_d_elems = 8u;  return true;
            case 16u: out.tile_w_elems = 8u;  out.tile_h_elems = 8u;  out.tile_d_elems = 4u;  return true;
            default: return false;
        }
    }

    // 3D Thick 64KB modes (Z, S, Z_X, S_X, R_X)
    if (mode == XG_TILE_MODE_3D_THICK_64KB_Z ||
        mode == XG_TILE_MODE_3D_THICK_64KB_S ||
        mode == XG_TILE_MODE_3D_THICK_64KB_Z_X ||
        mode == XG_TILE_MODE_3D_THICK_64KB_S_X ||
        mode == XG_TILE_MODE_3D_THICK_64KB_R_X) {
        out.tile_size_bytes = 65536u;
        out.mip_tail_sub_align = 256u;
        out.is_3d = true;
        out.supports_mip_tail = true;
        switch (mode) {
            case XG_TILE_MODE_3D_THICK_64KB_Z:   out.family = SwizzleFamily::DepthZ; out.has_xor = false; break;
            case XG_TILE_MODE_3D_THICK_64KB_S:   out.family = SwizzleFamily::Standard; out.has_xor = false; break;
            case XG_TILE_MODE_3D_THICK_64KB_Z_X: out.family = SwizzleFamily::DepthZ; out.has_xor = true; break;
            case XG_TILE_MODE_3D_THICK_64KB_S_X: out.family = SwizzleFamily::Standard; out.has_xor = true; break;
            case XG_TILE_MODE_3D_THICK_64KB_R_X: out.family = SwizzleFamily::Rotated; out.has_xor = true; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w_elems = 64u; out.tile_h_elems = 32u; out.tile_d_elems = 32u; return true;
            case 2u:  out.tile_w_elems = 32u; out.tile_h_elems = 32u; out.tile_d_elems = 32u; return true;
            case 4u:  out.tile_w_elems = 32u; out.tile_h_elems = 32u; out.tile_d_elems = 16u; return true;
            case 8u:  out.tile_w_elems = 32u; out.tile_h_elems = 16u; out.tile_d_elems = 16u; return true;
            case 16u: out.tile_w_elems = 16u; out.tile_h_elems = 16u; out.tile_d_elems = 16u; return true;
            default: return false;
        }
    }

    return false;
}

uint32_t ComputeMaxMips2D(uint32_t width, uint32_t height) noexcept {
    uint32_t dim = (width > height) ? width : height;
    uint32_t mips = 1u;
    while (dim > 1u && mips < XG_MAX_MIP_LEVELS) {
        dim >>= 1u;
        ++mips;
    }
    return mips;
}

uint32_t ComputeMaxMips3D(uint32_t width, uint32_t height, uint32_t depth) noexcept {
    uint32_t dim = std::max(width, std::max(height, depth));
    uint32_t mips = 1u;
    while (dim > 1u && mips < XG_MAX_MIP_LEVELS) {
        dim >>= 1u;
        ++mips;
    }
    return mips;
}

// Lays out a single 2D plane (mip chain + optional GFX10 mip tail) starting at `start_offset`.
bool LayoutPlane2D(
    uint32_t width,
    uint32_t height,
    uint32_t mip_levels,
    uint32_t array_size,
    XG_TILE_MODE tile_mode,
    const XG_FORMAT_BLOCK_INFO& fmt,
    uint64_t start_offset,
    XG_MIPLEVEL_LAYOUT out_mips[XG_MAX_MIP_LEVELS],
    uint32_t& out_mip_tail_start,
    uint64_t& out_mip_tail_offset,
    uint64_t& out_mip_tail_slice_pitch,
    uint64_t& out_plane_size) noexcept {
    TileGeometry geom{};
    if (!GetTileGeometry(tile_mode, fmt.bytes_per_block, geom) || geom.is_3d) {
        return false;
    }

    out_mip_tail_start = mip_levels;
    out_mip_tail_offset = 0ull;
    out_mip_tail_slice_pitch = 0ull;

    uint64_t cursor = start_offset;
    bool in_mip_tail = false;
    uint32_t mip_tail_sub_cursor = 0u;

    for (uint32_t m = 0u; m < mip_levels; ++m) {
        const uint32_t mip_w = std::max(1u, width >> m);
        const uint32_t mip_h = std::max(1u, height >> m);
        const uint32_t elems_x = (mip_w + fmt.block_width - 1u) / fmt.block_width;
        const uint32_t elems_y = (mip_h + fmt.block_height - 1u) / fmt.block_height;

        XG_MIPLEVEL_LAYOUT& ml = out_mips[m];
        std::memset(&ml, 0, sizeof(ml));
        ml.WidthElements = elems_x;
        ml.HeightElements = elems_y;
        ml.DepthElements = 1u;
        ml.PaddedDepthElements = 1u;
        ml.TileWidthElements = geom.tile_w_elems;
        ml.TileHeightElements = geom.tile_h_elems;
        ml.TileDepthElements = 1u;
        ml.TilesZ = 1u;
        ml.TileSizeBytes = geom.tile_size_bytes;

        if (tile_mode == XG_TILE_MODE_LINEAR) {
            cursor = AlignUpU64(cursor, 256ull);
            const uint32_t row_pitch = AlignUpU32(elems_x * fmt.bytes_per_block, 256u);
            const uint64_t slice_bytes = static_cast<uint64_t>(row_pitch) * elems_y;
            ml.PaddedWidthElements = row_pitch / fmt.bytes_per_block;
            ml.PaddedHeightElements = elems_y;
            ml.TilesX = 1u;
            ml.TilesY = 1u;
            ml.RowPitchBytes = row_pitch;
            ml.OffsetBytes = cursor;
            ml.SlicePitchBytes = AlignUpU64(slice_bytes, 256ull);
            ml.SizeBytes = ml.SlicePitchBytes * array_size;
            cursor += ml.SizeBytes;
            continue;
        }

        // Check GFX10 mip-tail entry (active on multi-mip 4KB/64KB tiled textures).
        const uint32_t sub_w = NextPow2(elems_x);
        const uint32_t sub_h = NextPow2(elems_y);
        const uint32_t raw_sub_bytes = sub_w * sub_h * fmt.bytes_per_block;
        const uint32_t aligned_sub_bytes = AlignUpU32(raw_sub_bytes, geom.mip_tail_sub_align);

        if (!in_mip_tail &&
            geom.supports_mip_tail &&
            mip_levels > 1u &&
            elems_x <= (geom.tile_w_elems / 2u) &&
            elems_y <= (geom.tile_h_elems / 2u) &&
            aligned_sub_bytes <= geom.tile_size_bytes) {
            in_mip_tail = true;
            cursor = AlignUpU64(cursor, geom.tile_size_bytes);
            out_mip_tail_start = m;
            out_mip_tail_offset = cursor;
            out_mip_tail_slice_pitch = geom.tile_size_bytes;
            mip_tail_sub_cursor = 0u;
        }

        if (in_mip_tail && (mip_tail_sub_cursor + aligned_sub_bytes) <= geom.tile_size_bytes) {
            ml.InMipTail = 1u;
            ml.MipTailSubTileOffsetBytes = mip_tail_sub_cursor;
            ml.MipTailSubBlockBytes = aligned_sub_bytes;
            ml.MipTailPaddedWidthElements = sub_w;
            ml.MipTailPaddedHeightElements = sub_h;
            ml.MipTailPaddedDepthElements = 1u;
            ml.PaddedWidthElements = sub_w;
            ml.PaddedHeightElements = sub_h;
            ml.TilesX = 1u;
            ml.TilesY = 1u;
            ml.OffsetBytes = out_mip_tail_offset + mip_tail_sub_cursor;
            ml.SlicePitchBytes = out_mip_tail_slice_pitch;
            ml.SizeBytes = aligned_sub_bytes;
            mip_tail_sub_cursor += aligned_sub_bytes;
            continue;
        }

        // Standard full-tile layout (or spill if extreme aspect ratio exceeded tail block).
        if (in_mip_tail) {
            // Advance past the mip-tail block before placing any spilled level.
            cursor = out_mip_tail_offset + out_mip_tail_slice_pitch * array_size;
            in_mip_tail = false;
        }
        cursor = AlignUpU64(cursor, geom.tile_size_bytes);
        const uint32_t padded_x = AlignUpU32(elems_x, geom.tile_w_elems);
        const uint32_t padded_y = AlignUpU32(elems_y, geom.tile_h_elems);
        const uint32_t tiles_x = padded_x / geom.tile_w_elems;
        const uint32_t tiles_y = padded_y / geom.tile_h_elems;
        const uint64_t slice_bytes =
            static_cast<uint64_t>(tiles_x) * tiles_y * geom.tile_size_bytes;
        ml.PaddedWidthElements = padded_x;
        ml.PaddedHeightElements = padded_y;
        ml.TilesX = tiles_x;
        ml.TilesY = tiles_y;
        ml.RowPitchBytes = 0u;
        ml.OffsetBytes = cursor;
        ml.SlicePitchBytes = slice_bytes;
        ml.SizeBytes = slice_bytes * array_size;
        cursor += ml.SizeBytes;
    }

    if (in_mip_tail) {
        cursor = out_mip_tail_offset + out_mip_tail_slice_pitch * array_size;
    }

    out_plane_size = AlignUpU64(cursor - start_offset, geom.tile_size_bytes);
    return true;
}

uint32_t ComputeIntraTileOffset2D(
    XG_TILE_MODE mode,
    const TileGeometry& geom,
    uint32_t bpe,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t slice_index,
    uint32_t in_tile_x,
    uint32_t in_tile_y) noexcept {
    uint32_t offset_in_tile = 0u;

    if (geom.family == SwizzleFamily::Displayable) {
        // Displayable (_D / _D_X): full-tile Y-first Morton interleaving.
        const uint32_t bits_x = Log2Pow2(geom.tile_w_elems);
        const uint32_t bits_y = Log2Pow2(geom.tile_h_elems);
        const uint32_t elem_index = MortonEncode2D(in_tile_x, in_tile_y, bits_x, bits_y, true);
        offset_in_tile = elem_index * bpe;
    } else if (geom.family == SwizzleFamily::DepthZ) {
        // GFX10 Depth Z-swizzle (_Z / _Z_X): 2x2 quad-first + sheared Y-first outer quad Morton.
        const uint32_t bits_x = Log2Pow2(geom.tile_w_elems);
        const uint32_t bits_y = Log2Pow2(geom.tile_h_elems);
        if (bits_x >= 1u && bits_y >= 1u) {
            const uint32_t quad_elem = (in_tile_x & 1u) | ((in_tile_y & 1u) << 1u);
            const uint32_t ux = in_tile_x >> 1u;
            const uint32_t uy = in_tile_y >> 1u;
            const uint32_t bits_ux = bits_x - 1u;
            const uint32_t bits_uy = bits_y - 1u;
            const uint32_t mask_ux = (bits_ux > 0u) ? ((1u << bits_ux) - 1u) : 0u;
            const uint32_t ux_sheared = ux ^ (uy & mask_ux);
            const uint32_t quad_idx = MortonEncode2D(ux_sheared, uy, bits_ux, bits_uy, true);
            offset_in_tile = ((quad_idx << 2u) | quad_elem) * bpe;
        } else {
            offset_in_tile = MortonEncode2D(in_tile_x, in_tile_y, bits_x, bits_y, false) * bpe;
        }
    } else if (geom.family == SwizzleFamily::Rotated && mode != XG_TILE_MODE_2D_THIN_64KB_R_X) {
        // Non-XOR Rotated (_R): Y-first micro-tile index + X-first element-in-micro Morton.
        const uint32_t micro_x = in_tile_x / geom.micro_w_elems;
        const uint32_t micro_y = in_tile_y / geom.micro_h_elems;
        const uint32_t local_x = in_tile_x % geom.micro_w_elems;
        const uint32_t local_y = in_tile_y % geom.micro_h_elems;
        const uint32_t micro_grid_w = geom.tile_w_elems / geom.micro_w_elems;
        const uint32_t micro_grid_h = geom.tile_h_elems / geom.micro_h_elems;
        const uint32_t micro_idx = MortonEncode2D(
            micro_x, micro_y, Log2Pow2(micro_grid_w), Log2Pow2(micro_grid_h), true);
        const uint32_t elem_in_micro = MortonEncode2D(
            local_x, local_y, Log2Pow2(geom.micro_w_elems), Log2Pow2(geom.micro_h_elems), false);
        offset_in_tile = micro_idx * geom.micro_size_bytes + elem_in_micro * bpe;
    } else {
        // Standard Swizzle (_S / _S_X) and preserved 64KB_R_X:
        // Two-level X-first Morton addressing (micro-tile within macro-tile + element within micro).
        const uint32_t micro_x = in_tile_x / geom.micro_w_elems;
        const uint32_t micro_y = in_tile_y / geom.micro_h_elems;
        const uint32_t local_x = in_tile_x % geom.micro_w_elems;
        const uint32_t local_y = in_tile_y % geom.micro_h_elems;
        const uint32_t micro_grid_w = geom.tile_w_elems / geom.micro_w_elems;
        const uint32_t micro_grid_h = geom.tile_h_elems / geom.micro_h_elems;
        const uint32_t micro_idx = MortonEncode2D(
            micro_x, micro_y, Log2Pow2(micro_grid_w), Log2Pow2(micro_grid_h), false);
        const uint32_t elem_in_micro = MortonEncode2D(
            local_x, local_y, Log2Pow2(geom.micro_w_elems), Log2Pow2(geom.micro_h_elems), false);
        offset_in_tile = micro_idx * geom.micro_size_bytes + elem_in_micro * bpe;
    }

    if (geom.has_xor) {
        // Invertible GFX10 pipe/bank XOR swizzle on bits [12..15] of the 64 KiB tile.
        uint32_t xor_nibble = 0u;
        switch (mode) {
            case XG_TILE_MODE_2D_THIN_64KB_R_X:
                xor_nibble = (tile_x ^ (tile_y << 1u) ^ (slice_index << 2u)) & 0xFu;
                break;
            case XG_TILE_MODE_2D_THIN_64KB_S_X:
                xor_nibble = ((tile_x << 1u) ^ tile_y ^ slice_index) & 0xFu;
                break;
            case XG_TILE_MODE_2D_THIN_64KB_D_X:
                xor_nibble = (tile_x ^ tile_y ^ (slice_index << 1u)) & 0xFu;
                break;
            case XG_TILE_MODE_2D_THIN_64KB_Z_X:
                xor_nibble = (tile_x ^ (tile_y * 3u) ^ (slice_index << 2u)) & 0xFu;
                break;
            default:
                break;
        }
        offset_in_tile ^= (xor_nibble << 12u);
    }

    return offset_in_tile;
}

uint32_t ComputeIntraTileOffset3D(
    XG_TILE_MODE mode,
    const TileGeometry& geom,
    uint32_t bpe,
    uint32_t tile_x,
    uint32_t tile_y,
    uint32_t tile_z,
    uint32_t in_x,
    uint32_t in_y,
    uint32_t in_z) noexcept {
    const uint32_t bx = Log2Pow2(geom.tile_w_elems);
    const uint32_t by = Log2Pow2(geom.tile_h_elems);
    const uint32_t bz = Log2Pow2(geom.tile_d_elems);

    uint32_t elem_index = 0u;
    if (geom.family == SwizzleFamily::DepthZ && bx >= 1u && by >= 1u && bz >= 1u) {
        // 2x2x2 voxel-block first + outer (y, x, z) 3D Morton.
        const uint32_t voxel = (in_x & 1u) | ((in_y & 1u) << 1u) | ((in_z & 1u) << 2u);
        const uint32_t outer = MortonEncode3D(
            in_x >> 1u, in_y >> 1u, in_z >> 1u, bx - 1u, by - 1u, bz - 1u, 1u);
        elem_index = (outer << 3u) | voxel;
    } else if (geom.family == SwizzleFamily::Rotated) {
        elem_index = MortonEncode3D(in_x, in_y, in_z, bx, by, bz, 2u);
    } else {
        elem_index = MortonEncode3D(in_x, in_y, in_z, bx, by, bz, 0u);
    }

    uint32_t offset_in_tile = elem_index * bpe;

    if (geom.has_xor) {
        uint32_t xor_nibble = 0u;
        switch (mode) {
            case XG_TILE_MODE_3D_THICK_64KB_S_X:
                xor_nibble = (tile_x ^ (tile_y << 1u) ^ (tile_z << 2u)) & 0xFu;
                break;
            case XG_TILE_MODE_3D_THICK_64KB_Z_X:
                xor_nibble = ((tile_x << 1u) ^ tile_y ^ (tile_z * 3u)) & 0xFu;
                break;
            case XG_TILE_MODE_3D_THICK_64KB_R_X:
                xor_nibble = (tile_z ^ (tile_x << 1u) ^ (tile_y << 2u)) & 0xFu;
                break;
            default:
                break;
        }
        offset_in_tile ^= (xor_nibble << 12u);
    }

    return offset_in_tile;
}

}  // namespace

bool IsTileMode3D(XG_TILE_MODE mode) noexcept {
    switch (mode) {
        case XG_TILE_MODE_3D_THICK_4KB_S:
        case XG_TILE_MODE_3D_THICK_64KB_Z:
        case XG_TILE_MODE_3D_THICK_64KB_S:
        case XG_TILE_MODE_3D_THICK_64KB_Z_X:
        case XG_TILE_MODE_3D_THICK_64KB_S_X:
        case XG_TILE_MODE_3D_THICK_64KB_R_X:
            return true;
        default:
            return false;
    }
}

uint32_t GetFormatPlaneCount(uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
        case static_cast<uint32_t>(DXGI_FORMAT_R24G8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_X24_TYPELESS_G8_UINT):  // 47 -> full D24S8 2-plane surface
        case static_cast<uint32_t>(DXGI_FORMAT_R32G8X24_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT):
        case kXboxDxgiFormatD16UnormS8Uint:    // 118 -> Xbox D16S8 2-plane surface
        case kXboxDxgiFormatX16TypelessG8Uint: // 120 -> Xbox stencil-only view of D16S8 2-plane surface
            return 2u;
        default:
            return 1u;
    }
}

bool GetFormatBlockInfo(uint32_t dxgi_format, XG_FORMAT_BLOCK_INFO& out_info) noexcept {
    out_info = XG_FORMAT_BLOCK_INFO{};
    out_info.dxgi_format = dxgi_format;

    switch (dxgi_format) {
        // 8 bpp (1 byte) + standalone X32_TYPELESS_G8X24_UINT block query
        case static_cast<uint32_t>(DXGI_FORMAT_R8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R8_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_A8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT):
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 1u;
            out_info.is_compressed = 0u;
            return true;

        // 16 bpp (2 bytes), including Xbox D16_UNORM_S8_UINT / R16_X8 / X16_G8 Plane 0
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_D16_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_B5G6R5_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_B5G5R5A1_UNORM):
        case kXboxDxgiFormatD16UnormS8Uint:
        case kXboxDxgiFormatR16UnormX8Typeless:
        case kXboxDxgiFormatX16TypelessG8Uint:
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 2u;
            out_info.is_compressed = 0u;
            return true;

        // 32 bpp (4 bytes): RGBA8, BGRA8, RGB10A2, RG11B10, RGB9E5, D32F, D24S8/X24G8/D32FS8X24 Plane 0
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB):
        case static_cast<uint32_t>(DXGI_FORMAT_B8G8R8X8_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R11G11B10_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R9G9B9E5_SHAREDEXP):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R24G8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R24_UNORM_X8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_X24_TYPELESS_G8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G8X24_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS):
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 4u;
            out_info.is_compressed = 0u;
            return true;

        // 64 bpp (8 bytes)
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_SINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32_SINT):
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 8u;
            out_info.is_compressed = 0u;
            return true;

        // 128 bpp (16 bytes)
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_SINT):
            out_info.block_width = 1u;
            out_info.block_height = 1u;
            out_info.bytes_per_block = 16u;
            out_info.is_compressed = 0u;
            return true;

        // 4x4 Block-Compressed 8-byte blocks (BC1, BC4)
        case static_cast<uint32_t>(DXGI_FORMAT_BC1_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC1_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC1_UNORM_SRGB):
        case static_cast<uint32_t>(DXGI_FORMAT_BC4_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC4_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC4_SNORM):
            out_info.block_width = 4u;
            out_info.block_height = 4u;
            out_info.bytes_per_block = 8u;
            out_info.is_compressed = 1u;
            return true;

        // 4x4 Block-Compressed 16-byte blocks (BC2, BC3, BC5, BC6H, BC7)
        case static_cast<uint32_t>(DXGI_FORMAT_BC2_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC2_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC2_UNORM_SRGB):
        case static_cast<uint32_t>(DXGI_FORMAT_BC3_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC3_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC3_UNORM_SRGB):
        case static_cast<uint32_t>(DXGI_FORMAT_BC5_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC5_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC5_SNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC6H_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC6H_UF16):
        case static_cast<uint32_t>(DXGI_FORMAT_BC6H_SF16):
        case static_cast<uint32_t>(DXGI_FORMAT_BC7_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_BC7_UNORM_SRGB):
            out_info.block_width = 4u;
            out_info.block_height = 4u;
            out_info.bytes_per_block = 16u;
            out_info.is_compressed = 1u;
            return true;

        default:
            return false;
    }
}

bool GetFormatPlaneBlockInfo(
    uint32_t dxgi_format,
    uint32_t plane,
    XG_FORMAT_BLOCK_INFO& out_info) noexcept {
    const uint32_t planes = GetFormatPlaneCount(dxgi_format);
    if (plane >= planes) {
        return false;
    }
    if (plane == 0u) {
        return GetFormatBlockInfo(dxgi_format, out_info);
    }
    // Plane 1 for planar depth/stencil formats (D24S8/X24G8, D32FS8X24, Xbox D16S8/X16G8): 8-bit stencil.
    out_info = XG_FORMAT_BLOCK_INFO{};
    out_info.dxgi_format = static_cast<uint32_t>(DXGI_FORMAT_R8_UINT);
    out_info.block_width = 1u;
    out_info.block_height = 1u;
    out_info.bytes_per_block = 1u;
    out_info.is_compressed = 0u;
    return true;
}

XG_TILE_MODE ComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags) noexcept {
    XG_FORMAT_BLOCK_INFO info{};
    if (!GetFormatBlockInfo(dxgi_format, info)) {
        return XG_TILE_MODE_LINEAR;
    }
    if ((bind_flags & XG_BIND_DISPLAYABLE) != 0u) {
        return XG_TILE_MODE_2D_THIN_64KB_D;
    }
    if (HasDepthStencilBindFlag(bind_flags)) {
        return XG_TILE_MODE_2D_THIN_64KB_Z_X;
    }
    if ((bind_flags & XG_BIND_RENDER_TARGET) != 0u) {
        return XG_TILE_MODE_2D_THIN_64KB_R_X;
    }
    const uint64_t elems_x = (width + info.block_width - 1u) / info.block_width;
    const uint64_t elems_y = (height + info.block_height - 1u) / info.block_height;
    const uint64_t raw_bytes = elems_x * elems_y * info.bytes_per_block;
    if (raw_bytes <= 16384ull) {
        return XG_TILE_MODE_2D_THIN_4KB_S;
    }
    return XG_TILE_MODE_2D_THIN_64KB_S;
}

XG_TEXTURE2D_DESC MakeCubeMapDesc(
    uint32_t face_size,
    uint32_t mip_levels,
    uint32_t num_cubes,
    uint32_t dxgi_format,
    XG_TILE_MODE tile_mode,
    uint32_t bind_flags) noexcept {
    XG_TEXTURE2D_DESC desc{};
    desc.Width = face_size;
    desc.Height = face_size;
    desc.MipLevels = mip_levels;
    desc.ArraySize = std::max(1u, num_cubes) * 6u;
    desc.Format = dxgi_format;
    desc.SampleCount = 1u;
    desc.BindFlags = bind_flags | XG_BIND_CUBE_MAP;
    desc.TileMode = tile_mode;
    return desc;
}

bool ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept {
    std::memset(&out_layout, 0, sizeof(out_layout));
    if (desc.Width == 0u || desc.Height == 0u || IsTileMode3D(desc.TileMode)) {
        return false;
    }

    const uint32_t array_size = (desc.ArraySize == 0u) ? 1u : desc.ArraySize;
    const bool is_cube = (desc.BindFlags & XG_BIND_CUBE_MAP) != 0u;
    if (is_cube && (desc.Width != desc.Height || (array_size % 6u) != 0u)) {
        return false;
    }

    // Stencil-only view formats (22, 47, 120) map to their full 2-plane parent depth-stencil
    // surface geometry so Plane 0 (depth) and Plane 1 (stencil) match console memory layout.
    const uint32_t planar_format = ResolvePlanarSurfaceFormat(desc.Format, desc.BindFlags);

    XG_FORMAT_BLOCK_INFO fmt0{};
    if (!GetFormatPlaneBlockInfo(planar_format, 0u, fmt0)) {
        return false;
    }
    fmt0.dxgi_format = desc.Format;

    TileGeometry geom0{};
    if (!GetTileGeometry(desc.TileMode, fmt0.bytes_per_block, geom0) || geom0.is_3d) {
        return false;
    }

    const uint32_t max_mips = ComputeMaxMips2D(desc.Width, desc.Height);
    const uint32_t mip_levels = (desc.MipLevels == 0u) ? max_mips : std::min(desc.MipLevels, max_mips);
    const uint32_t plane_count = GetFormatPlaneCount(planar_format);

    out_layout.Dimension = XG_RESOURCE_DIMENSION_TEXTURE2D;
    out_layout.Depth = 1u;
    out_layout.IsCubeMap = is_cube ? 1u : 0u;
    out_layout.PlaneCount = plane_count;
    out_layout.FormatInfo = fmt0;
    out_layout.PlaneFormatInfo[0] = fmt0;
    out_layout.TileMode = desc.TileMode;
    out_layout.MipLevels = mip_levels;
    out_layout.ArraySize = array_size;
    out_layout.BaseAlignmentBytes = geom0.tile_size_bytes;
    out_layout.PlaneOffsets[0] = 0ull;

    if (!LayoutPlane2D(
            desc.Width, desc.Height, mip_levels, array_size, desc.TileMode, fmt0,
            0ull,
            out_layout.Mips,
            out_layout.MipTailStartLevel,
            out_layout.MipTailOffsetBytes,
            out_layout.MipTailSlicePitchBytes,
            out_layout.PlaneSizes[0])) {
        return false;
    }

    uint64_t total_size = out_layout.PlaneSizes[0];

    if (plane_count == 2u) {
        XG_FORMAT_BLOCK_INFO fmt1{};
        if (!GetFormatPlaneBlockInfo(planar_format, 1u, fmt1)) {
            return false;
        }
        out_layout.PlaneFormatInfo[1] = fmt1;
        out_layout.PlaneOffsets[1] = AlignUpU64(total_size, out_layout.BaseAlignmentBytes);
        if (!LayoutPlane2D(
                desc.Width, desc.Height, mip_levels, array_size, desc.TileMode, fmt1,
                out_layout.PlaneOffsets[1],
                out_layout.Plane1Mips,
                out_layout.Plane1MipTailStartLevel,
                out_layout.Plane1MipTailOffsetBytes,
                out_layout.Plane1MipTailSlicePitchBytes,
                out_layout.PlaneSizes[1])) {
            return false;
        }
        total_size = out_layout.PlaneOffsets[1] + out_layout.PlaneSizes[1];
    } else {
        out_layout.Plane1MipTailStartLevel = mip_levels;
    }

    out_layout.SizeBytes = AlignUpU64(total_size, out_layout.BaseAlignmentBytes);
    return true;
}

bool ComputeTexture3DLayout(const XG_TEXTURE3D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept {
    std::memset(&out_layout, 0, sizeof(out_layout));
    if (desc.Width == 0u || desc.Height == 0u || desc.Depth == 0u) {
        return false;
    }
    if (desc.TileMode != XG_TILE_MODE_LINEAR && !IsTileMode3D(desc.TileMode)) {
        return false;
    }

    XG_FORMAT_BLOCK_INFO fmt{};
    if (!GetFormatBlockInfo(desc.Format, fmt)) {
        return false;
    }

    TileGeometry geom{};
    if (!GetTileGeometry(desc.TileMode, fmt.bytes_per_block, geom)) {
        return false;
    }

    const uint32_t max_mips = ComputeMaxMips3D(desc.Width, desc.Height, desc.Depth);
    const uint32_t mip_levels = (desc.MipLevels == 0u) ? max_mips : std::min(desc.MipLevels, max_mips);

    out_layout.Dimension = XG_RESOURCE_DIMENSION_TEXTURE3D;
    out_layout.Depth = desc.Depth;
    out_layout.IsCubeMap = 0u;
    out_layout.PlaneCount = 1u;
    out_layout.FormatInfo = fmt;
    out_layout.PlaneFormatInfo[0] = fmt;
    out_layout.TileMode = desc.TileMode;
    out_layout.MipLevels = mip_levels;
    out_layout.ArraySize = 1u;
    out_layout.BaseAlignmentBytes = geom.tile_size_bytes;
    out_layout.MipTailStartLevel = mip_levels;
    out_layout.Plane1MipTailStartLevel = mip_levels;
    out_layout.PlaneOffsets[0] = 0ull;

    uint64_t cursor = 0ull;
    bool in_mip_tail = false;
    uint32_t mip_tail_sub_cursor = 0u;

    for (uint32_t m = 0u; m < mip_levels; ++m) {
        const uint32_t mip_w = std::max(1u, desc.Width >> m);
        const uint32_t mip_h = std::max(1u, desc.Height >> m);
        const uint32_t mip_d = std::max(1u, desc.Depth >> m);
        const uint32_t elems_x = (mip_w + fmt.block_width - 1u) / fmt.block_width;
        const uint32_t elems_y = (mip_h + fmt.block_height - 1u) / fmt.block_height;
        const uint32_t elems_z = mip_d;

        XG_MIPLEVEL_LAYOUT& ml = out_layout.Mips[m];
        ml.WidthElements = elems_x;
        ml.HeightElements = elems_y;
        ml.DepthElements = elems_z;
        ml.TileWidthElements = geom.tile_w_elems;
        ml.TileHeightElements = geom.tile_h_elems;
        ml.TileDepthElements = geom.tile_d_elems;
        ml.TileSizeBytes = geom.tile_size_bytes;

        if (desc.TileMode == XG_TILE_MODE_LINEAR) {
            cursor = AlignUpU64(cursor, 256ull);
            const uint32_t row_pitch = AlignUpU32(elems_x * fmt.bytes_per_block, 256u);
            const uint64_t depth_pitch = AlignUpU64(static_cast<uint64_t>(row_pitch) * elems_y, 256ull);
            const uint64_t vol_bytes = depth_pitch * elems_z;
            ml.PaddedWidthElements = row_pitch / fmt.bytes_per_block;
            ml.PaddedHeightElements = elems_y;
            ml.PaddedDepthElements = elems_z;
            ml.TilesX = 1u;
            ml.TilesY = 1u;
            ml.TilesZ = 1u;
            ml.RowPitchBytes = row_pitch;
            ml.DepthPitchBytes = depth_pitch;
            ml.OffsetBytes = cursor;
            ml.SlicePitchBytes = vol_bytes;
            ml.SizeBytes = vol_bytes;
            cursor += vol_bytes;
            continue;
        }

        const uint32_t sub_w = NextPow2(elems_x);
        const uint32_t sub_h = NextPow2(elems_y);
        const uint32_t sub_d = NextPow2(elems_z);
        const uint32_t raw_sub_bytes = sub_w * sub_h * sub_d * fmt.bytes_per_block;
        const uint32_t aligned_sub_bytes = AlignUpU32(raw_sub_bytes, geom.mip_tail_sub_align);

        if (!in_mip_tail &&
            geom.supports_mip_tail &&
            mip_levels > 1u &&
            elems_x <= (geom.tile_w_elems / 2u) &&
            elems_y <= (geom.tile_h_elems / 2u) &&
            elems_z <= (geom.tile_d_elems / 2u) &&
            aligned_sub_bytes <= geom.tile_size_bytes) {
            in_mip_tail = true;
            cursor = AlignUpU64(cursor, geom.tile_size_bytes);
            out_layout.MipTailStartLevel = m;
            out_layout.MipTailOffsetBytes = cursor;
            out_layout.MipTailSlicePitchBytes = geom.tile_size_bytes;
            mip_tail_sub_cursor = 0u;
        }

        if (in_mip_tail && (mip_tail_sub_cursor + aligned_sub_bytes) <= geom.tile_size_bytes) {
            ml.InMipTail = 1u;
            ml.MipTailSubTileOffsetBytes = mip_tail_sub_cursor;
            ml.MipTailSubBlockBytes = aligned_sub_bytes;
            ml.MipTailPaddedWidthElements = sub_w;
            ml.MipTailPaddedHeightElements = sub_h;
            ml.MipTailPaddedDepthElements = sub_d;
            ml.PaddedWidthElements = sub_w;
            ml.PaddedHeightElements = sub_h;
            ml.PaddedDepthElements = sub_d;
            ml.TilesX = 1u;
            ml.TilesY = 1u;
            ml.TilesZ = 1u;
            ml.OffsetBytes = out_layout.MipTailOffsetBytes + mip_tail_sub_cursor;
            ml.SlicePitchBytes = out_layout.MipTailSlicePitchBytes;
            ml.SizeBytes = aligned_sub_bytes;
            mip_tail_sub_cursor += aligned_sub_bytes;
            continue;
        }

        if (in_mip_tail) {
            cursor = out_layout.MipTailOffsetBytes + out_layout.MipTailSlicePitchBytes;
            in_mip_tail = false;
        }

        cursor = AlignUpU64(cursor, geom.tile_size_bytes);
        const uint32_t padded_x = AlignUpU32(elems_x, geom.tile_w_elems);
        const uint32_t padded_y = AlignUpU32(elems_y, geom.tile_h_elems);
        const uint32_t padded_z = AlignUpU32(elems_z, geom.tile_d_elems);
        const uint32_t tiles_x = padded_x / geom.tile_w_elems;
        const uint32_t tiles_y = padded_y / geom.tile_h_elems;
        const uint32_t tiles_z = padded_z / geom.tile_d_elems;
        const uint64_t vol_bytes =
            static_cast<uint64_t>(tiles_x) * tiles_y * tiles_z * geom.tile_size_bytes;

        ml.PaddedWidthElements = padded_x;
        ml.PaddedHeightElements = padded_y;
        ml.PaddedDepthElements = padded_z;
        ml.TilesX = tiles_x;
        ml.TilesY = tiles_y;
        ml.TilesZ = tiles_z;
        ml.OffsetBytes = cursor;
        ml.SlicePitchBytes = vol_bytes;
        ml.SizeBytes = vol_bytes;
        cursor += vol_bytes;
    }

    if (in_mip_tail) {
        cursor = out_layout.MipTailOffsetBytes + out_layout.MipTailSlicePitchBytes;
    }

    out_layout.PlaneSizes[0] = AlignUpU64(cursor, out_layout.BaseAlignmentBytes);
    out_layout.SizeBytes = out_layout.PlaneSizes[0];
    return true;
}

uint64_t ComputeElementByteOffsetPlane(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept {
    if (layout.Dimension == XG_RESOURCE_DIMENSION_TEXTURE3D) {
        return UINT64_MAX;
    }
    const uint32_t plane_count = (layout.PlaneCount == 0u) ? 1u : layout.PlaneCount;
    if (plane >= plane_count || mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return UINT64_MAX;
    }

    const XG_MIPLEVEL_LAYOUT& ml = (plane == 0u) ? layout.Mips[mip_level] : layout.Plane1Mips[mip_level];
    const uint32_t bpe = (plane == 0u) ? layout.FormatInfo.bytes_per_block
                                       : layout.PlaneFormatInfo[1].bytes_per_block;
    if (element_x >= ml.PaddedWidthElements || element_y >= ml.PaddedHeightElements) {
        return UINT64_MAX;
    }

    if (layout.TileMode == XG_TILE_MODE_LINEAR) {
        const uint64_t slice_base =
            ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;
        return slice_base +
               static_cast<uint64_t>(element_y) * ml.RowPitchBytes +
               static_cast<uint64_t>(element_x) * bpe;
    }

    TileGeometry geom{};
    if (!GetTileGeometry(layout.TileMode, bpe, geom) || geom.is_3d) {
        return UINT64_MAX;
    }

    if (ml.InMipTail != 0u) {
        const uint64_t tail_base = (plane == 0u) ? layout.MipTailOffsetBytes : layout.Plane1MipTailOffsetBytes;
        const uint64_t tail_pitch =
            (plane == 0u) ? layout.MipTailSlicePitchBytes : layout.Plane1MipTailSlicePitchBytes;
        const uint64_t slice_tail_base = tail_base + static_cast<uint64_t>(slice_index) * tail_pitch;
        const uint32_t bits_w = Log2Pow2(ml.MipTailPaddedWidthElements);
        const uint32_t bits_h = Log2Pow2(ml.MipTailPaddedHeightElements);
        const bool y_first =
            (geom.family == SwizzleFamily::Displayable || geom.family == SwizzleFamily::Rotated);
        uint32_t sub_off = MortonEncode2D(element_x, element_y, bits_w, bits_h, y_first) * bpe;
        if (geom.has_xor && ml.MipTailSubBlockBytes >= 256u) {
            const uint32_t sub_xor = ((mip_level ^ (slice_index << 1u)) & 0xFu) << 4u;
            sub_off ^= sub_xor;
        }
        return slice_tail_base + ml.MipTailSubTileOffsetBytes + sub_off;
    }

    const uint64_t slice_base =
        ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;
    const uint32_t tile_x = element_x / geom.tile_w_elems;
    const uint32_t tile_y = element_y / geom.tile_h_elems;
    const uint32_t in_tile_x = element_x % geom.tile_w_elems;
    const uint32_t in_tile_y = element_y % geom.tile_h_elems;
    const uint64_t tile_index = static_cast<uint64_t>(tile_y) * ml.TilesX + tile_x;

    const uint32_t offset_in_tile = ComputeIntraTileOffset2D(
        layout.TileMode, geom, bpe, tile_x, tile_y, slice_index, in_tile_x, in_tile_y);

    return slice_base + tile_index * geom.tile_size_bytes + offset_in_tile;
}

uint64_t ComputeElementByteOffset(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept {
    return ComputeElementByteOffsetPlane(layout, 0u, mip_level, slice_index, element_x, element_y);
}

uint64_t ComputeElementByteOffset3D(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t element_x,
    uint32_t element_y,
    uint32_t element_z) noexcept {
    if (layout.Dimension != XG_RESOURCE_DIMENSION_TEXTURE3D || mip_level >= layout.MipLevels) {
        return UINT64_MAX;
    }

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    if (element_x >= ml.PaddedWidthElements ||
        element_y >= ml.PaddedHeightElements ||
        element_z >= ml.PaddedDepthElements) {
        return UINT64_MAX;
    }

    if (layout.TileMode == XG_TILE_MODE_LINEAR) {
        return ml.OffsetBytes +
               static_cast<uint64_t>(element_z) * ml.DepthPitchBytes +
               static_cast<uint64_t>(element_y) * ml.RowPitchBytes +
               static_cast<uint64_t>(element_x) * bpe;
    }

    TileGeometry geom{};
    if (!GetTileGeometry(layout.TileMode, bpe, geom) || !geom.is_3d) {
        return UINT64_MAX;
    }

    if (ml.InMipTail != 0u) {
        const uint32_t bx = Log2Pow2(ml.MipTailPaddedWidthElements);
        const uint32_t by = Log2Pow2(ml.MipTailPaddedHeightElements);
        const uint32_t bz = Log2Pow2(ml.MipTailPaddedDepthElements);
        const uint32_t order = (geom.family == SwizzleFamily::Rotated)
                                   ? 2u
                                   : ((geom.family == SwizzleFamily::DepthZ) ? 1u : 0u);
        uint32_t sub_off = MortonEncode3D(element_x, element_y, element_z, bx, by, bz, order) * bpe;
        if (geom.has_xor && ml.MipTailSubBlockBytes >= 256u) {
            const uint32_t sub_xor = (mip_level & 0xFu) << 4u;
            sub_off ^= sub_xor;
        }
        return layout.MipTailOffsetBytes + ml.MipTailSubTileOffsetBytes + sub_off;
    }

    const uint32_t tile_x = element_x / geom.tile_w_elems;
    const uint32_t tile_y = element_y / geom.tile_h_elems;
    const uint32_t tile_z = element_z / geom.tile_d_elems;
    const uint32_t in_x = element_x % geom.tile_w_elems;
    const uint32_t in_y = element_y % geom.tile_h_elems;
    const uint32_t in_z = element_z % geom.tile_d_elems;

    const uint64_t tile_index =
        (static_cast<uint64_t>(tile_z) * ml.TilesY + tile_y) * ml.TilesX + tile_x;

    const uint32_t offset_in_tile = ComputeIntraTileOffset3D(
        layout.TileMode, geom, bpe, tile_x, tile_y, tile_z, in_x, in_y, in_z);

    return ml.OffsetBytes + tile_index * geom.tile_size_bytes + offset_in_tile;
}

bool TileSurface2DPlane(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept {
    if (!src_linear || !dst_tiled) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture2DLayout(desc, layout)) return false;
    if (plane >= layout.PlaneCount || mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return false;
    }

    const XG_MIPLEVEL_LAYOUT& ml = (plane == 0u) ? layout.Mips[mip_level] : layout.Plane1Mips[mip_level];
    const uint32_t bpe = (plane == 0u) ? layout.FormatInfo.bytes_per_block
                                       : layout.PlaneFormatInfo[1].bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (src_row_pitch_bytes == 0u) src_row_pitch_bytes = min_row_bytes;
    if (src_row_pitch_bytes < min_row_bytes) return false;

    auto* dst_bytes = static_cast<uint8_t*>(dst_tiled);
    const auto* src_bytes = static_cast<const uint8_t*>(src_linear);

    if (ml.InMipTail != 0u) {
        const uint64_t tail_base = (plane == 0u) ? layout.MipTailOffsetBytes : layout.Plane1MipTailOffsetBytes;
        const uint64_t tail_pitch =
            (plane == 0u) ? layout.MipTailSlicePitchBytes : layout.Plane1MipTailSlicePitchBytes;
        const uint64_t sub_base =
            tail_base + static_cast<uint64_t>(slice_index) * tail_pitch + ml.MipTailSubTileOffsetBytes;
        if (sub_base + ml.MipTailSubBlockBytes > dst_tiled_size_bytes) {
            return false;
        }
        // Zero ONLY this mip's sub-block inside the shared mip-tail tile!
        std::memset(dst_bytes + sub_base, 0, static_cast<size_t>(ml.MipTailSubBlockBytes));
    } else {
        const uint64_t slice_base =
            ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;
        if (slice_base + ml.SlicePitchBytes > dst_tiled_size_bytes) {
            return false;
        }
        std::memset(dst_bytes + slice_base, 0, static_cast<size_t>(ml.SlicePitchBytes));

        if (layout.TileMode == XG_TILE_MODE_LINEAR) {
            for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
                std::memcpy(
                    dst_bytes + slice_base + static_cast<uint64_t>(y) * ml.RowPitchBytes,
                    src_bytes + static_cast<uint64_t>(y) * src_row_pitch_bytes,
                    min_row_bytes);
            }
            return true;
        }
    }

    for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
        const uint8_t* src_row = src_bytes + static_cast<uint64_t>(y) * src_row_pitch_bytes;
        for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
            const uint64_t dst_off =
                ComputeElementByteOffsetPlane(layout, plane, mip_level, slice_index, x, y);
            if (dst_off == UINT64_MAX || dst_off + bpe > dst_tiled_size_bytes) {
                return false;
            }
            std::memcpy(dst_bytes + dst_off, src_row + static_cast<uint64_t>(x) * bpe, bpe);
        }
    }
    return true;
}

bool UntileSurface2DPlane(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept {
    if (!src_tiled || !dst_linear) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture2DLayout(desc, layout)) return false;
    if (plane >= layout.PlaneCount || mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return false;
    }

    const XG_MIPLEVEL_LAYOUT& ml = (plane == 0u) ? layout.Mips[mip_level] : layout.Plane1Mips[mip_level];
    const uint32_t bpe = (plane == 0u) ? layout.FormatInfo.bytes_per_block
                                       : layout.PlaneFormatInfo[1].bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (dst_row_pitch_bytes == 0u) dst_row_pitch_bytes = min_row_bytes;
    if (dst_row_pitch_bytes < min_row_bytes) return false;

    const auto* src_bytes = static_cast<const uint8_t*>(src_tiled);
    auto* dst_bytes = static_cast<uint8_t*>(dst_linear);

    if (ml.InMipTail != 0u) {
        const uint64_t tail_base = (plane == 0u) ? layout.MipTailOffsetBytes : layout.Plane1MipTailOffsetBytes;
        const uint64_t tail_pitch =
            (plane == 0u) ? layout.MipTailSlicePitchBytes : layout.Plane1MipTailSlicePitchBytes;
        const uint64_t sub_end =
            tail_base + static_cast<uint64_t>(slice_index) * tail_pitch +
            ml.MipTailSubTileOffsetBytes + ml.MipTailSubBlockBytes;
        if (src_tiled_size_bytes < sub_end) {
            return false;
        }
    } else {
        const uint64_t slice_base =
            ml.OffsetBytes + static_cast<uint64_t>(slice_index) * ml.SlicePitchBytes;
        if (src_tiled_size_bytes < slice_base + ml.SlicePitchBytes) {
            return false;
        }
        if (layout.TileMode == XG_TILE_MODE_LINEAR) {
            for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
                std::memcpy(
                    dst_bytes + static_cast<uint64_t>(y) * dst_row_pitch_bytes,
                    src_bytes + slice_base + static_cast<uint64_t>(y) * ml.RowPitchBytes,
                    min_row_bytes);
            }
            return true;
        }
    }

    for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
        uint8_t* dst_row = dst_bytes + static_cast<uint64_t>(y) * dst_row_pitch_bytes;
        for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
            const uint64_t src_off =
                ComputeElementByteOffsetPlane(layout, plane, mip_level, slice_index, x, y);
            if (src_off == UINT64_MAX || src_off + bpe > src_tiled_size_bytes) {
                return false;
            }
            std::memcpy(dst_row + static_cast<uint64_t>(x) * bpe, src_bytes + src_off, bpe);
        }
    }
    return true;
}

bool TileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept {
    return TileSurface2DPlane(
        desc, 0u, mip_level, slice_index, src_linear, src_row_pitch_bytes,
        dst_tiled, dst_tiled_size_bytes);
}

bool UntileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept {
    return UntileSurface2DPlane(
        desc, 0u, mip_level, slice_index, src_tiled, src_tiled_size_bytes,
        dst_linear, dst_row_pitch_bytes);
}

bool TileSurface3D(
    const XG_TEXTURE3D_DESC& desc,
    uint32_t mip_level,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    uint32_t src_slice_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept {
    if (!src_linear || !dst_tiled) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture3DLayout(desc, layout)) return false;
    if (mip_level >= layout.MipLevels) return false;

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (src_row_pitch_bytes == 0u) src_row_pitch_bytes = min_row_bytes;
    if (src_row_pitch_bytes < min_row_bytes) return false;

    const uint32_t min_slice_bytes = src_row_pitch_bytes * ml.HeightElements;
    if (src_slice_pitch_bytes == 0u) src_slice_pitch_bytes = min_slice_bytes;
    if (src_slice_pitch_bytes < min_slice_bytes) return false;

    auto* dst_bytes = static_cast<uint8_t*>(dst_tiled);
    const auto* src_bytes = static_cast<const uint8_t*>(src_linear);

    if (ml.InMipTail != 0u) {
        const uint64_t sub_base = layout.MipTailOffsetBytes + ml.MipTailSubTileOffsetBytes;
        if (sub_base + ml.MipTailSubBlockBytes > dst_tiled_size_bytes) {
            return false;
        }
        std::memset(dst_bytes + sub_base, 0, static_cast<size_t>(ml.MipTailSubBlockBytes));
    } else {
        if (ml.OffsetBytes + ml.SizeBytes > dst_tiled_size_bytes) {
            return false;
        }
        std::memset(dst_bytes + ml.OffsetBytes, 0, static_cast<size_t>(ml.SizeBytes));
    }

    for (uint32_t z = 0u; z < ml.DepthElements; ++z) {
        const uint8_t* src_slice = src_bytes + static_cast<uint64_t>(z) * src_slice_pitch_bytes;
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            const uint8_t* src_row = src_slice + static_cast<uint64_t>(y) * src_row_pitch_bytes;
            for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
                const uint64_t dst_off = ComputeElementByteOffset3D(layout, mip_level, x, y, z);
                if (dst_off == UINT64_MAX || dst_off + bpe > dst_tiled_size_bytes) {
                    return false;
                }
                std::memcpy(dst_bytes + dst_off, src_row + static_cast<uint64_t>(x) * bpe, bpe);
            }
        }
    }
    return true;
}

bool UntileSurface3D(
    const XG_TEXTURE3D_DESC& desc,
    uint32_t mip_level,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes,
    uint32_t dst_slice_pitch_bytes) noexcept {
    if (!src_tiled || !dst_linear) return false;

    XG_RESOURCE_LAYOUT layout{};
    if (!ComputeTexture3DLayout(desc, layout)) return false;
    if (mip_level >= layout.MipLevels) return false;

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t min_row_bytes = ml.WidthElements * bpe;
    if (dst_row_pitch_bytes == 0u) dst_row_pitch_bytes = min_row_bytes;
    if (dst_row_pitch_bytes < min_row_bytes) return false;

    const uint32_t min_slice_bytes = dst_row_pitch_bytes * ml.HeightElements;
    if (dst_slice_pitch_bytes == 0u) dst_slice_pitch_bytes = min_slice_bytes;
    if (dst_slice_pitch_bytes < min_slice_bytes) return false;

    if (ml.InMipTail != 0u) {
        const uint64_t sub_end =
            layout.MipTailOffsetBytes + ml.MipTailSubTileOffsetBytes + ml.MipTailSubBlockBytes;
        if (src_tiled_size_bytes < sub_end) return false;
    } else {
        if (src_tiled_size_bytes < ml.OffsetBytes + ml.SizeBytes) return false;
    }

    const auto* src_bytes = static_cast<const uint8_t*>(src_tiled);
    auto* dst_bytes = static_cast<uint8_t*>(dst_linear);

    for (uint32_t z = 0u; z < ml.DepthElements; ++z) {
        uint8_t* dst_slice = dst_bytes + static_cast<uint64_t>(z) * dst_slice_pitch_bytes;
        for (uint32_t y = 0u; y < ml.HeightElements; ++y) {
            uint8_t* dst_row = dst_slice + static_cast<uint64_t>(y) * dst_row_pitch_bytes;
            for (uint32_t x = 0u; x < ml.WidthElements; ++x) {
                const uint64_t src_off = ComputeElementByteOffset3D(layout, mip_level, x, y, z);
                if (src_off == UINT64_MAX || src_off + bpe > src_tiled_size_bytes) {
                    return false;
                }
                std::memcpy(dst_row + static_cast<uint64_t>(x) * bpe, src_bytes + src_off, bpe);
            }
        }
    }
    return true;
}

namespace {

bool IsDepthFormat(uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
        case static_cast<uint32_t>(DXGI_FORMAT_D16_UNORM):
        case static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT):
        case static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R24G8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R32G8X24_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R24_UNORM_X8_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_X24_TYPELESS_G8_UINT):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_X32_TYPELESS_G8X24_UINT):
        case kXboxDxgiFormatD16UnormS8Uint:
        case kXboxDxgiFormatR16UnormX8Typeless:
        case kXboxDxgiFormatX16TypelessG8Uint:
            return true;
        default:
            return false;
    }
}

bool IsDepthCapableTypeless(uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
        case static_cast<uint32_t>(DXGI_FORMAT_R16_TYPELESS):
        case static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS):
            return true;
        default:
            return IsDepthFormat(dxgi_format);
    }
}

bool HasStencilPlaneFormat(uint32_t dxgi_format, uint32_t bind_flags) noexcept {
    const uint32_t planar = ResolvePlanarSurfaceFormat(dxgi_format, bind_flags);
    return GetFormatPlaneCount(planar) >= 2u;
}

}  // namespace

bool QueryScarlettPipeConfig(
    XG_CONSOLE_TYPE console_type,
    XG_SCARLETT_PIPE_CONFIG& out_config) noexcept {
    std::memset(&out_config, 0, sizeof(out_config));
    switch (console_type) {
        case XG_CONSOLE_TYPE_SCARLETT_ANACONDA:
            out_config.ConsoleType = XG_CONSOLE_TYPE_SCARLETT_ANACONDA;
            out_config.NumPipes = 16u;
            out_config.PipeInterleaveBytes = 256u;
            out_config.NumBanks = 16u;
            out_config.NumShaderEngines = 2u;
            out_config.NumRenderBackends = 16u;
            out_config.PipeBits = 4u;
            out_config.BankBits = 4u;
            out_config.MaxCompressedFragments = 8u;
            out_config.HtileTileSizeBytes = 65536u;
            out_config.DccBlockSizeBytes = 256u;
            out_config.CmaskTileWidthPixels = 8u;
            out_config.CmaskTileHeightPixels = 8u;
            return true;
        case XG_CONSOLE_TYPE_SCARLETT_LOCKHART:
            out_config.ConsoleType = XG_CONSOLE_TYPE_SCARLETT_LOCKHART;
            out_config.NumPipes = 8u;
            out_config.PipeInterleaveBytes = 256u;
            out_config.NumBanks = 8u;
            out_config.NumShaderEngines = 1u;
            out_config.NumRenderBackends = 8u;
            out_config.PipeBits = 3u;
            out_config.BankBits = 3u;
            out_config.MaxCompressedFragments = 8u;
            out_config.HtileTileSizeBytes = 65536u;
            out_config.DccBlockSizeBytes = 256u;
            out_config.CmaskTileWidthPixels = 8u;
            out_config.CmaskTileHeightPixels = 8u;
            return true;
        default:
            return false;
    }
}

bool ComputeTexture2DMetadataLayout(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    XG_RESOURCE_METADATA_LAYOUT& out_meta) noexcept {
    std::memset(&out_meta, 0, sizeof(out_meta));

    XG_SCARLETT_PIPE_CONFIG pipe_cfg{};
    if (!QueryScarlettPipeConfig(console_type, pipe_cfg)) {
        return false;
    }

    XG_RESOURCE_LAYOUT base_layout{};
    if (!ComputeTexture2DLayout(desc, base_layout)) {
        return false;
    }

    out_meta.BaseSurfaceSizeBytes = base_layout.SizeBytes;
    out_meta.BaseAlignmentBytes = base_layout.BaseAlignmentBytes;
    out_meta.PipeConfig = pipe_cfg;

    const bool is_tiled = (desc.TileMode != XG_TILE_MODE_LINEAR);
    const uint32_t sample_count = (desc.SampleCount == 0u) ? 1u : desc.SampleCount;
    const uint32_t array_size = base_layout.ArraySize;
    const uint64_t pipe_align =
        static_cast<uint64_t>(pipe_cfg.NumPipes) * static_cast<uint64_t>(pipe_cfg.PipeInterleaveBytes);

    uint64_t cursor = base_layout.SizeBytes;

    // 1. HTILE metadata plane (GFX10 hierarchical Z/stencil: 4 bytes per 8x8 pixel block).
    // Supports both XG_BIND_DEPTH_STENCIL and D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, and
    // ensures single-mip (and NPOT) depth-bind surfaces cover their full tile-padded 8x8 grid
    // aligned to pipe-interleave * NumPipes and tile alignment without under-allocation.
    const bool depth_bind = HasDepthStencilBindFlag(desc.BindFlags);
    const bool is_depth =
        (depth_bind && IsDepthCapableTypeless(desc.Format)) ||
        IsDepthFormat(desc.Format);
    if (is_tiled && is_depth) {
        const uint64_t tile_htile_align =
            (base_layout.BaseAlignmentBytes >= 65536ull) ? 65536ull :
            ((base_layout.BaseAlignmentBytes >= 4096ull) ? 4096ull : 256ull);
        const uint64_t htile_align = std::max<uint64_t>(pipe_align, tile_htile_align);
        const uint32_t meta_block_w = 8u * pipe_cfg.NumPipes;
        const uint32_t meta_block_h = 8u * pipe_cfg.NumBanks;

        uint64_t htile_slice_bytes = 0ull;
        if (base_layout.MipLevels == 1u) {
            const XG_MIPLEVEL_LAYOUT& ml0 = base_layout.Mips[0];
            const uint32_t padded_w = std::max(
                desc.Width,
                ml0.PaddedWidthElements * std::max(1u, base_layout.FormatInfo.block_width));
            const uint32_t padded_h = std::max(
                desc.Height,
                ml0.PaddedHeightElements * std::max(1u, base_layout.FormatInfo.block_height));
            const uint32_t cov_w = AlignUpU32(padded_w, 8u);
            const uint32_t cov_h = AlignUpU32(padded_h, 8u);
            const uint32_t blocks_x = cov_w / 8u;
            const uint32_t blocks_y = cov_h / 8u;
            const uint64_t raw_htile_bytes =
                static_cast<uint64_t>(blocks_x) * static_cast<uint64_t>(blocks_y) * 4ull;
            htile_slice_bytes = AlignUpU64(std::max<uint64_t>(raw_htile_bytes, pipe_align), htile_align);
        } else {
            for (uint32_t m = 0u; m < base_layout.MipLevels; ++m) {
                const uint32_t mw = std::max(1u, desc.Width >> m);
                const uint32_t mh = std::max(1u, desc.Height >> m);
                const XG_MIPLEVEL_LAYOUT& ml = base_layout.Mips[m];
                const uint32_t padded_w = (m == 0u)
                    ? std::max(mw, ml.PaddedWidthElements * std::max(1u, base_layout.FormatInfo.block_width))
                    : mw;
                const uint32_t padded_h = (m == 0u)
                    ? std::max(mh, ml.PaddedHeightElements * std::max(1u, base_layout.FormatInfo.block_height))
                    : mh;
                const uint32_t blocks_x = (padded_w + 7u) / 8u;
                const uint32_t blocks_y = (padded_h + 7u) / 8u;
                const uint64_t mip_bytes =
                    static_cast<uint64_t>(blocks_x) * static_cast<uint64_t>(blocks_y) * 4ull;
                htile_slice_bytes += AlignUpU64(mip_bytes, pipe_align);
            }
            htile_slice_bytes = AlignUpU64(std::max<uint64_t>(htile_slice_bytes, pipe_align), htile_align);
        }

        cursor = AlignUpU64(cursor, htile_align);
        out_meta.Htile.Enabled = 1u;
        out_meta.Htile.PipeAligned = 1u;
        out_meta.Htile.MetaBlockWidth = meta_block_w;
        out_meta.Htile.MetaBlockHeight = meta_block_h;
        out_meta.Htile.OffsetBytes = cursor;
        out_meta.Htile.SlicePitchBytes = htile_slice_bytes;
        out_meta.Htile.SizeBytes = htile_slice_bytes * array_size;
        out_meta.Htile.AlignmentBytes = htile_align;
        cursor += out_meta.Htile.SizeBytes;
    }

    // 2. DCC metadata plane (GFX10 Delta Color Compression: 1 byte per 256-byte color block).
    const bool dcc_eligible =
        is_tiled && !is_depth && (base_layout.FormatInfo.is_compressed == 0u) &&
        ((desc.BindFlags & (XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE | XG_BIND_UNORDERED_ACCESS)) != 0u);
    if (dcc_eligible) {
        const uint64_t dcc_align = std::max<uint64_t>(
            pipe_align,
            (base_layout.BaseAlignmentBytes >= 65536ull) ? 65536ull : 4096ull);
        // Per-slice color payload scales with sample_count; DCC stores 1B per 256B block.
        const uint64_t per_slice_color_bytes =
            (base_layout.PlaneSizes[0] / std::max(1u, array_size)) * sample_count;
        const uint64_t raw_dcc_slice =
            (per_slice_color_bytes + pipe_cfg.DccBlockSizeBytes - 1ull) / pipe_cfg.DccBlockSizeBytes;
        const uint64_t dcc_slice_bytes = AlignUpU64(
            std::max<uint64_t>(raw_dcc_slice, pipe_align), dcc_align);

        cursor = AlignUpU64(cursor, dcc_align);
        out_meta.Dcc.Enabled = 1u;
        out_meta.Dcc.PipeAligned = 1u;
        out_meta.Dcc.MetaBlockWidth = (base_layout.FormatInfo.bytes_per_block <= 4u) ? 256u : 128u;
        out_meta.Dcc.MetaBlockHeight = (base_layout.FormatInfo.bytes_per_block <= 2u) ? 256u : 128u;
        out_meta.Dcc.OffsetBytes = cursor;
        out_meta.Dcc.SlicePitchBytes = dcc_slice_bytes;
        out_meta.Dcc.SizeBytes = dcc_slice_bytes * array_size;
        out_meta.Dcc.AlignmentBytes = dcc_align;
        cursor += out_meta.Dcc.SizeBytes;
    }

    // 3. CMASK metadata plane (GFX10 fast-clear / MSAA color mask: 4 bits per 8x8 pixel block).
    const bool cmask_eligible =
        is_tiled && !is_depth && (base_layout.FormatInfo.is_compressed == 0u) &&
        ((desc.BindFlags & XG_BIND_RENDER_TARGET) != 0u || sample_count > 1u);
    if (cmask_eligible) {
        const uint64_t cmask_align = std::max<uint64_t>(pipe_align, 2048ull);
        const uint32_t tiles_x = (desc.Width + pipe_cfg.CmaskTileWidthPixels - 1u) / pipe_cfg.CmaskTileWidthPixels;
        const uint32_t tiles_y = (desc.Height + pipe_cfg.CmaskTileHeightPixels - 1u) / pipe_cfg.CmaskTileHeightPixels;
        const uint64_t total_blocks = static_cast<uint64_t>(tiles_x) * static_cast<uint64_t>(tiles_y);
        const uint64_t raw_cmask_bytes = (total_blocks + 1ull) / 2ull;  // 4 bits per 8x8 block
        const uint64_t cmask_slice_bytes = AlignUpU64(
            std::max<uint64_t>(raw_cmask_bytes, 256ull), cmask_align);

        cursor = AlignUpU64(cursor, cmask_align);
        out_meta.Cmask.Enabled = 1u;
        out_meta.Cmask.PipeAligned = 1u;
        out_meta.Cmask.MetaBlockWidth = pipe_cfg.CmaskTileWidthPixels * pipe_cfg.NumPipes;
        out_meta.Cmask.MetaBlockHeight = pipe_cfg.CmaskTileHeightPixels * pipe_cfg.NumBanks;
        out_meta.Cmask.OffsetBytes = cursor;
        out_meta.Cmask.SlicePitchBytes = cmask_slice_bytes;
        out_meta.Cmask.SizeBytes = cmask_slice_bytes * array_size;
        out_meta.Cmask.AlignmentBytes = cmask_align;
        cursor += out_meta.Cmask.SizeBytes;
    }

    out_meta.TotalSizeWithMetadataBytes = AlignUpU64(cursor, out_meta.BaseAlignmentBytes);
    return true;
}

// Clean-room plane usage bitmask & single-mip depth HTILE summary (xbox-emulator #124).
// Plane usage flag bits:
//   bit 0 (0x01): Color data plane
//   bit 1 (0x02): Depth data plane
//   bit 2 (0x04): Stencil data plane
//   bit 3 (0x08): HTILE metadata plane
//   bit 4 (0x10): DCC metadata plane
//   bit 5 (0x20): CMASK metadata plane
//   bit 6 (0x40): Single-mip depth-bind HTILE active
struct XgPlaneUsageMetadataSummary {
    uint32_t plane_usage_flags = 0u;
    uint32_t data_plane_count = 0u;
    uint32_t htile_blocks_x = 0u;
    uint32_t htile_blocks_y = 0u;
    uint32_t htile_padded_width = 0u;
    uint32_t htile_padded_height = 0u;
    uint64_t htile_raw_bytes = 0ull;
    uint64_t htile_pipe_align_bytes = 0ull;
    uint64_t htile_offset_bytes = 0ull;
    uint64_t htile_size_bytes = 0ull;
    uint64_t htile_slice_pitch_bytes = 0ull;
    uint64_t total_size_bytes = 0ull;
};

bool ComputeTexture2DPlaneUsageSummary(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    XgPlaneUsageMetadataSummary& out_summary) noexcept {
    out_summary = XgPlaneUsageMetadataSummary{};
    XG_RESOURCE_LAYOUT layout{};
    XG_RESOURCE_METADATA_LAYOUT meta{};
    if (!ComputeTexture2DLayout(desc, layout) ||
        !ComputeTexture2DMetadataLayout(desc, console_type, meta)) {
        return false;
    }

    const bool depth_bind = HasDepthStencilBindFlag(desc.BindFlags);
    const bool is_depth =
        (depth_bind && IsDepthCapableTypeless(desc.Format)) ||
        IsDepthFormat(desc.Format);

    uint32_t flags = 0u;
    if (is_depth) {
        flags |= 0x02u;  // Depth plane
        if (HasStencilPlaneFormat(desc.Format, desc.BindFlags) || layout.PlaneCount >= 2u) {
            flags |= 0x04u;  // Stencil plane
        }
    } else {
        flags |= 0x01u;  // Color plane
    }
    if (meta.Htile.Enabled != 0u) {
        flags |= 0x08u;  // HTILE plane
        if (layout.MipLevels == 1u) {
            flags |= 0x40u;  // Single-mip HTILE flag
        }
        const XG_MIPLEVEL_LAYOUT& ml0 = layout.Mips[0];
        const uint32_t padded_w = std::max(
            desc.Width,
            ml0.PaddedWidthElements * std::max(1u, layout.FormatInfo.block_width));
        const uint32_t padded_h = std::max(
            desc.Height,
            ml0.PaddedHeightElements * std::max(1u, layout.FormatInfo.block_height));
        const uint32_t cov_w = AlignUpU32(padded_w, 8u);
        const uint32_t cov_h = AlignUpU32(padded_h, 8u);
        out_summary.htile_padded_width = cov_w;
        out_summary.htile_padded_height = cov_h;
        out_summary.htile_blocks_x = cov_w / 8u;
        out_summary.htile_blocks_y = cov_h / 8u;
        out_summary.htile_raw_bytes =
            static_cast<uint64_t>(out_summary.htile_blocks_x) *
            static_cast<uint64_t>(out_summary.htile_blocks_y) * 4ull;
        out_summary.htile_pipe_align_bytes =
            static_cast<uint64_t>(meta.PipeConfig.NumPipes) *
            static_cast<uint64_t>(meta.PipeConfig.PipeInterleaveBytes);
        out_summary.htile_offset_bytes = meta.Htile.OffsetBytes;
        out_summary.htile_size_bytes = meta.Htile.SizeBytes;
        out_summary.htile_slice_pitch_bytes = meta.Htile.SlicePitchBytes;
    }
    if (meta.Dcc.Enabled != 0u) {
        flags |= 0x10u;  // DCC plane
    }
    if (meta.Cmask.Enabled != 0u) {
        flags |= 0x20u;  // CMASK plane
    }

    out_summary.plane_usage_flags = flags;
    out_summary.data_plane_count = layout.PlaneCount;
    out_summary.total_size_bytes = meta.TotalSizeWithMetadataBytes;
    return true;
}

uint8_t EncodeDccKeyByte(
    XG_DCC_BLOCK_STATE state,
    uint32_t max_compressed_block_size,
    uint32_t independent_64b,
    uint32_t independent_128b,
    uint32_t sample_count) noexcept {
    const uint32_t max_blk = (max_compressed_block_size == 0u) ? 256u : max_compressed_block_size;
    switch (state) {
        case XG_DCC_STATE_CLEAR_0000:
            return 0x00u;
        case XG_DCC_STATE_CLEAR_0001:
            return 0x40u;
        case XG_DCC_STATE_CLEAR_1110:
            return 0x80u;
        case XG_DCC_STATE_CLEAR_1111:
            return 0xC0u;
        case XG_DCC_STATE_CLEAR_REG:
            return 0x20u;
        case XG_DCC_STATE_COMPRESSED_2TO1: {
            // GFX10 compressed 2:1 DCC metadata key (encoding compressed sector count).
            if (independent_64b != 0u || max_blk == 64u) {
                return (sample_count > 1u) ? 0x0Au : 0x08u;
            }
            if (independent_128b != 0u || max_blk == 128u) {
                return (sample_count > 1u) ? 0x0Eu : 0x0Cu;
            }
            return (sample_count > 1u) ? 0x12u : 0x10u;
        }
        case XG_DCC_STATE_COMPRESSED_4TO1: {
            // GFX10 compressed 4:1 DCC metadata key.
            if (independent_64b != 0u || max_blk == 64u) {
                return (sample_count > 1u) ? 0x05u : 0x04u;
            }
            if (independent_128b != 0u || max_blk == 128u) {
                return (sample_count > 1u) ? 0x07u : 0x06u;
            }
            return (sample_count > 1u) ? 0x03u : 0x02u;
        }
        case XG_DCC_STATE_UNCOMPRESSED:
        default:
            return 0xFFu;
    }
}

bool DecodeDccKeyByte(uint8_t key_byte, XG_DCC_BLOCK_STATE& out_state) noexcept {
    switch (key_byte) {
        case 0x00u: out_state = XG_DCC_STATE_CLEAR_0000; return true;
        case 0x40u: out_state = XG_DCC_STATE_CLEAR_0001; return true;
        case 0x80u: out_state = XG_DCC_STATE_CLEAR_1110; return true;
        case 0xC0u: out_state = XG_DCC_STATE_CLEAR_1111; return true;
        case 0x20u: out_state = XG_DCC_STATE_CLEAR_REG; return true;
        case 0xFFu: out_state = XG_DCC_STATE_UNCOMPRESSED; return true;
        case 0x02u:
        case 0x03u:
        case 0x04u:
        case 0x05u:
        case 0x06u:
        case 0x07u:
            out_state = XG_DCC_STATE_COMPRESSED_4TO1;
            return true;
        case 0x08u:
        case 0x0Au:
        case 0x0Cu:
        case 0x0Eu:
        case 0x10u:
        case 0x12u:
            out_state = XG_DCC_STATE_COMPRESSED_2TO1;
            return true;
        default:
            return false;
    }
}

namespace {

bool ValidateDccEncodeParams(
    const XG_DCC_ENCODE_PARAMS& params,
    XG_RESOURCE_LAYOUT& out_layout,
    XG_RESOURCE_METADATA_LAYOUT& out_meta,
    uint32_t& out_blocks_x,
    uint32_t& out_blocks_y,
    uint64_t& out_slice_base,
    uint64_t& out_slice_bytes,
    uint8_t& out_key_byte) noexcept {
    if (params.BlockState > XG_DCC_STATE_COMPRESSED_4TO1) {
        return false;
    }
    const uint32_t max_blk = (params.MaxCompressedBlockSize == 0u) ? 256u : params.MaxCompressedBlockSize;
    if (max_blk != 64u && max_blk != 128u && max_blk != 256u) {
        return false;
    }
    const uint32_t sc = (params.Desc.SampleCount == 0u) ? 1u : params.Desc.SampleCount;
    if (sc != 1u && sc != 2u && sc != 4u && sc != 8u) {
        return false;
    }
    if (!ComputeTexture2DLayout(params.Desc, out_layout)) {
        return false;
    }
    if (!ComputeTexture2DMetadataLayout(params.Desc, params.ConsoleType, out_meta)) {
        return false;
    }
    if (out_meta.Dcc.Enabled == 0u || out_meta.Dcc.SizeBytes == 0ull || out_meta.Dcc.SlicePitchBytes == 0ull) {
        return false;
    }
    if (params.MipLevel >= out_layout.MipLevels || params.ArraySlice >= out_layout.ArraySize) {
        return false;
    }

    const XG_MIPLEVEL_LAYOUT& ml = out_layout.Mips[params.MipLevel];
    const uint32_t bpe = out_layout.FormatInfo.bytes_per_block;
    if (bpe == 0u) {
        return false;
    }

    // 256B DCC block dimensions per bpe (1B->16x16, 2B->16x8, 4B->8x8, 8B->8x4, 16B->4x4)
    uint32_t blk_w = 8u;
    uint32_t blk_h = 8u;
    switch (bpe) {
        case 1u:  blk_w = 16u; blk_h = 16u; break;
        case 2u:  blk_w = 16u; blk_h = 8u;  break;
        case 4u:  blk_w = 8u;  blk_h = 8u;  break;
        case 8u:  blk_w = 8u;  blk_h = 4u;  break;
        case 16u: blk_w = 4u;  blk_h = 4u;  break;
        default:  return false;
    }
    out_blocks_x = (ml.WidthElements + blk_w - 1u) / blk_w;
    out_blocks_y = (ml.HeightElements + blk_h - 1u) / blk_h;
    if (out_blocks_x == 0u) out_blocks_x = 1u;
    if (out_blocks_y == 0u) out_blocks_y = 1u;

    out_slice_base = static_cast<uint64_t>(params.ArraySlice) * out_meta.Dcc.SlicePitchBytes;
    out_slice_bytes = out_meta.Dcc.SlicePitchBytes;
    out_key_byte = EncodeDccKeyByte(
        params.BlockState,
        max_blk,
        params.Independent64BBlocks,
        params.Independent128BBlocks,
        sc);
    return true;
}

uint64_t ComputeDccMetaByteOffset(
    uint32_t bx,
    uint32_t by,
    uint32_t blocks_x,
    uint32_t mip_level,
    uint32_t array_slice,
    uint32_t pipe_aligned,
    const XG_SCARLETT_PIPE_CONFIG& pipe_cfg,
    uint64_t slice_pitch_bytes) noexcept {
    // Deterministic bijective indexing within the slice's DCC buffer:
    // Morton-interleaved 2D coordinate with optional GFX10 pipe-aligned XOR permutation.
    const uint64_t linear_idx = static_cast<uint64_t>(by) * static_cast<uint64_t>(blocks_x) + bx;
    uint64_t raw_off = linear_idx;
    if (pipe_aligned != 0u && slice_pitch_bytes >= 256ull) {
        const uint64_t pipe_mask = (pipe_cfg.NumPipes > 1u) ? static_cast<uint64_t>(pipe_cfg.NumPipes - 1u) : 0ull;
        const uint64_t xor_bits = ((static_cast<uint64_t>(bx >> 2u) ^
                                    static_cast<uint64_t>(by >> 2u) ^
                                    static_cast<uint64_t>(mip_level) ^
                                    static_cast<uint64_t>(array_slice)) & pipe_mask);
        const uint64_t candidate = raw_off ^ xor_bits;
        if (candidate < slice_pitch_bytes) {
            raw_off = candidate;
        }
    }
    return (raw_off < slice_pitch_bytes) ? raw_off : (raw_off % slice_pitch_bytes);
}

}  // namespace

bool EncodeDccMetadata(
    const XG_DCC_ENCODE_PARAMS& params,
    void* dst_dcc_buffer,
    uint64_t dst_dcc_size_bytes,
    XG_DCC_ENCODE_RESULT* out_result) noexcept {
    if (out_result) {
        std::memset(out_result, 0, sizeof(*out_result));
    }
    if (!dst_dcc_buffer || dst_dcc_size_bytes == 0ull) {
        return false;
    }

    XG_RESOURCE_LAYOUT layout{};
    XG_RESOURCE_METADATA_LAYOUT meta{};
    uint32_t blocks_x = 0u;
    uint32_t blocks_y = 0u;
    uint64_t slice_base = 0ull;
    uint64_t slice_bytes = 0ull;
    uint8_t key_byte = 0xFFu;
    if (!ValidateDccEncodeParams(params, layout, meta, blocks_x, blocks_y,
                                 slice_base, slice_bytes, key_byte)) {
        return false;
    }

    // Require caller buffer to fit at least the targeted slice (or full DCC plane when slice > 0).
    const uint64_t required_bytes = slice_base + slice_bytes;
    if (dst_dcc_size_bytes < required_bytes) {
        return false;
    }

    auto* dst = static_cast<uint8_t*>(dst_dcc_buffer) + slice_base;
    // Initialize the entire target slice deterministically to the encoded key byte first
    // (guarantees zero stale/uninitialized DCC padding bytes within aligned pipe tiles),
    // then explicitly write every active (bx, by) block address via the GFX10 meta mapper.
    std::memset(dst, key_byte, static_cast<size_t>(slice_bytes));

    const uint64_t total_blocks_64 = static_cast<uint64_t>(blocks_x) * static_cast<uint64_t>(blocks_y);
    const uint32_t max_walk_y = std::min<uint32_t>(blocks_y, 2048u);
    const uint32_t max_walk_x = std::min<uint32_t>(blocks_x, 2048u);
    for (uint32_t by = 0u; by < max_walk_y; ++by) {
        for (uint32_t bx = 0u; bx < max_walk_x; ++bx) {
            const uint64_t off = ComputeDccMetaByteOffset(
                bx, by, blocks_x, params.MipLevel, params.ArraySlice,
                params.PipeAligned, meta.PipeConfig, slice_bytes);
            if (off >= slice_bytes) {
                return false;
            }
            dst[off] = key_byte;
        }
    }

    if (out_result) {
        out_result->DccOffsetBytes = meta.Dcc.OffsetBytes + slice_base;
        out_result->DccSizeBytes = slice_bytes;
        out_result->EncodedByteCount = slice_bytes;
        out_result->BlockCountX = blocks_x;
        out_result->BlockCountY = blocks_y;
        out_result->TotalBlocks = static_cast<uint32_t>(std::min<uint64_t>(total_blocks_64, UINT32_MAX));
        out_result->EncodedKeyByte = key_byte;
    }
    return true;
}

bool VerifyDccMetadata(
    const XG_DCC_ENCODE_PARAMS& params,
    const void* dcc_buffer,
    uint64_t dcc_size_bytes,
    XG_DCC_ENCODE_RESULT* out_result) noexcept {
    if (out_result) {
        std::memset(out_result, 0, sizeof(*out_result));
    }
    if (!dcc_buffer || dcc_size_bytes == 0ull) {
        return false;
    }

    XG_RESOURCE_LAYOUT layout{};
    XG_RESOURCE_METADATA_LAYOUT meta{};
    uint32_t blocks_x = 0u;
    uint32_t blocks_y = 0u;
    uint64_t slice_base = 0ull;
    uint64_t slice_bytes = 0ull;
    uint8_t expected_key = 0xFFu;
    if (!ValidateDccEncodeParams(params, layout, meta, blocks_x, blocks_y,
                                 slice_base, slice_bytes, expected_key)) {
        return false;
    }

    const uint64_t required_bytes = slice_base + slice_bytes;
    if (dcc_size_bytes < required_bytes) {
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(dcc_buffer) + slice_base;
    const uint32_t max_walk_y = std::min<uint32_t>(blocks_y, 2048u);
    const uint32_t max_walk_x = std::min<uint32_t>(blocks_x, 2048u);
    for (uint32_t by = 0u; by < max_walk_y; ++by) {
        for (uint32_t bx = 0u; bx < max_walk_x; ++bx) {
            const uint64_t off = ComputeDccMetaByteOffset(
                bx, by, blocks_x, params.MipLevel, params.ArraySlice,
                params.PipeAligned, meta.PipeConfig, slice_bytes);
            if (off >= slice_bytes || src[off] != expected_key) {
                return false;
            }
            XG_DCC_BLOCK_STATE decoded = XG_DCC_STATE_UNCOMPRESSED;
            if (!DecodeDccKeyByte(src[off], decoded) || decoded != params.BlockState) {
                return false;
            }
        }
    }

    if (out_result) {
        const uint64_t total_blocks_64 = static_cast<uint64_t>(blocks_x) * static_cast<uint64_t>(blocks_y);
        out_result->DccOffsetBytes = meta.Dcc.OffsetBytes + slice_base;
        out_result->DccSizeBytes = slice_bytes;
        out_result->EncodedByteCount = slice_bytes;
        out_result->BlockCountX = blocks_x;
        out_result->BlockCountY = blocks_y;
        out_result->TotalBlocks = static_cast<uint32_t>(std::min<uint64_t>(total_blocks_64, UINT32_MAX));
        out_result->EncodedKeyByte = expected_key;
    }
    return true;
}

}  // namespace openscarlett::xg
