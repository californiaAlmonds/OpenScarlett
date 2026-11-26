#include "openscarlett/gpu/descriptor_sgpr_bridge.h"

#include <algorithm>
#include <cstring>

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

namespace {

// Portable DXGI numeric constants (matching dxgiformat.h) so format helpers work everywhere.
constexpr uint32_t kDxgiUnknown = 0u;
constexpr uint32_t kDxgiR32G32B32A32Float = 2u;
constexpr uint32_t kDxgiR32G32B32A32Uint = 3u;
constexpr uint32_t kDxgiR32G32B32A32Sint = 4u;
constexpr uint32_t kDxgiR32G32B32Float = 6u;
constexpr uint32_t kDxgiR32G32B32Uint = 7u;
constexpr uint32_t kDxgiR32G32B32Sint = 8u;
constexpr uint32_t kDxgiR16G16B16A16Float = 10u;
constexpr uint32_t kDxgiR16G16B16A16Unorm = 11u;
constexpr uint32_t kDxgiR16G16B16A16Uint = 12u;
constexpr uint32_t kDxgiR16G16B16A16Snorm = 13u;
constexpr uint32_t kDxgiR16G16B16A16Sint = 14u;
constexpr uint32_t kDxgiR32G32Float = 16u;
constexpr uint32_t kDxgiR32G32Uint = 17u;
constexpr uint32_t kDxgiR32G32Sint = 18u;
constexpr uint32_t kDxgiR10G10B10A2Unorm = 24u;
constexpr uint32_t kDxgiR10G10B10A2Uint = 25u;
constexpr uint32_t kDxgiR11G11B10Float = 26u;
constexpr uint32_t kDxgiR8G8B8A8Unorm = 28u;
constexpr uint32_t kDxgiR8G8B8A8UnormSrgb = 29u;
constexpr uint32_t kDxgiR8G8B8A8Uint = 30u;
constexpr uint32_t kDxgiR8G8B8A8Snorm = 31u;
constexpr uint32_t kDxgiR8G8B8A8Sint = 32u;
constexpr uint32_t kDxgiR16G16Float = 34u;
constexpr uint32_t kDxgiR16G16Unorm = 35u;
constexpr uint32_t kDxgiR16G16Uint = 36u;
constexpr uint32_t kDxgiR16G16Snorm = 37u;
constexpr uint32_t kDxgiR16G16Sint = 38u;
constexpr uint32_t kDxgiD32Float = 40u;
constexpr uint32_t kDxgiR32Float = 41u;
constexpr uint32_t kDxgiR32Uint = 42u;
constexpr uint32_t kDxgiR32Sint = 43u;
constexpr uint32_t kDxgiR8G8Unorm = 49u;
constexpr uint32_t kDxgiR8G8Uint = 50u;
constexpr uint32_t kDxgiR8G8Snorm = 51u;
constexpr uint32_t kDxgiR8G8Sint = 52u;
constexpr uint32_t kDxgiR16Float = 54u;
constexpr uint32_t kDxgiR16Unorm = 56u;
constexpr uint32_t kDxgiR16Uint = 57u;
constexpr uint32_t kDxgiR16Snorm = 58u;
constexpr uint32_t kDxgiR16Sint = 59u;
constexpr uint32_t kDxgiR8Unorm = 61u;
constexpr uint32_t kDxgiR8Uint = 62u;
constexpr uint32_t kDxgiR8Snorm = 63u;
constexpr uint32_t kDxgiR8Sint = 64u;
constexpr uint32_t kDxgiBc1Unorm = 71u;
constexpr uint32_t kDxgiBc1UnormSrgb = 72u;
constexpr uint32_t kDxgiBc2Unorm = 74u;
constexpr uint32_t kDxgiBc3Unorm = 77u;
constexpr uint32_t kDxgiBc4Unorm = 80u;
constexpr uint32_t kDxgiBc5Unorm = 83u;
constexpr uint32_t kDxgiB8G8R8A8Unorm = 87u;
constexpr uint32_t kDxgiB8G8R8A8UnormSrgb = 91u;
constexpr uint32_t kDxgiBc6hUf16 = 95u;
constexpr uint32_t kDxgiBc7Unorm = 98u;
constexpr uint32_t kDxgiBc7UnormSrgb = 99u;

uint32_t DxgiFormatElementBytes(uint32_t dxgi_fmt) noexcept {
    switch (dxgi_fmt) {
        case kDxgiR8Unorm:
        case kDxgiR8Snorm:
        case kDxgiR8Uint:
        case kDxgiR8Sint:
            return 1u;
        case kDxgiR16Unorm:
        case kDxgiR16Snorm:
        case kDxgiR16Uint:
        case kDxgiR16Sint:
        case kDxgiR16Float:
        case kDxgiR8G8Unorm:
        case kDxgiR8G8Snorm:
        case kDxgiR8G8Uint:
        case kDxgiR8G8Sint:
            return 2u;
        case kDxgiR32Uint:
        case kDxgiR32Sint:
        case kDxgiR32Float:
        case kDxgiR16G16Unorm:
        case kDxgiR16G16Snorm:
        case kDxgiR16G16Uint:
        case kDxgiR16G16Sint:
        case kDxgiR16G16Float:
        case kDxgiR8G8B8A8Unorm:
        case kDxgiR8G8B8A8Snorm:
        case kDxgiR8G8B8A8Uint:
        case kDxgiR8G8B8A8Sint:
        case kDxgiR8G8B8A8UnormSrgb:
        case kDxgiB8G8R8A8Unorm:
        case kDxgiB8G8R8A8UnormSrgb:
        case kDxgiR10G10B10A2Unorm:
        case kDxgiR10G10B10A2Uint:
        case kDxgiR11G11B10Float:
            return 4u;
        case kDxgiR32G32Float:
        case kDxgiR32G32Uint:
        case kDxgiR32G32Sint:
        case kDxgiR16G16B16A16Unorm:
        case kDxgiR16G16B16A16Snorm:
        case kDxgiR16G16B16A16Uint:
        case kDxgiR16G16B16A16Sint:
        case kDxgiR16G16B16A16Float:
            return 8u;
        case kDxgiR32G32B32Float:
        case kDxgiR32G32B32Uint:
        case kDxgiR32G32B32Sint:
            return 12u;
        case kDxgiR32G32B32A32Float:
        case kDxgiR32G32B32A32Uint:
        case kDxgiR32G32B32A32Sint:
            return 16u;
        default:
            return 4u;
    }
}

uint32_t MaxAnisoRatioToCount(uint8_t ratio_code) noexcept {
    switch (ratio_code & 0x7u) {
        case 0u: return 1u;
        case 1u: return 2u;
        case 2u: return 4u;
        case 3u: return 8u;
        case 4u: return 16u;
        default: return 16u;
    }
}

uint32_t Rdna2ClampToVkAddressMode(Rdna2ClampMode mode) noexcept {
    switch (mode) {
        case Rdna2ClampMode::Wrap:                 return 0u;  // VK_SAMPLER_ADDRESS_MODE_REPEAT
        case Rdna2ClampMode::Mirror:               return 1u;  // VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
        case Rdna2ClampMode::ClampLastTexel:       return 2u;  // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
        case Rdna2ClampMode::MirrorOnceLastTexel:  return 4u;  // VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
        case Rdna2ClampMode::ClampHalfBorder:
        case Rdna2ClampMode::ClampBorder:          return 3u;  // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
        case Rdna2ClampMode::MirrorOnceHalfBorder:
        case Rdna2ClampMode::MirrorOnceBorder:     return 4u;  // VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
        default:                                   return 2u;
    }
}

#if defined(_WIN32)
UINT Rdna2DstSelToD3D12Mapping(Rdna2DstSel sel) noexcept {
    switch (sel) {
        case Rdna2DstSel::Zero: return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
        case Rdna2DstSel::One:  return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        case Rdna2DstSel::R:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
        case Rdna2DstSel::G:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
        case Rdna2DstSel::B:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
        case Rdna2DstSel::A:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
        default:                return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    }
}

D3D12_TEXTURE_ADDRESS_MODE Rdna2ClampToD3D12AddressMode(Rdna2ClampMode mode) noexcept {
    switch (mode) {
        case Rdna2ClampMode::Wrap:                 return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case Rdna2ClampMode::Mirror:               return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case Rdna2ClampMode::ClampLastTexel:       return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case Rdna2ClampMode::MirrorOnceLastTexel:  return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        case Rdna2ClampMode::ClampHalfBorder:
        case Rdna2ClampMode::ClampBorder:          return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case Rdna2ClampMode::MirrorOnceHalfBorder:
        case Rdna2ClampMode::MirrorOnceBorder:     return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default:                                   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

D3D12_COMPARISON_FUNC Rdna2CompareToD3D12(Rdna2CompareFunc fn) noexcept {
    switch (fn) {
        case Rdna2CompareFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
        case Rdna2CompareFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
        case Rdna2CompareFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
        case Rdna2CompareFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case Rdna2CompareFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
        case Rdna2CompareFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case Rdna2CompareFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case Rdna2CompareFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
        default:                             return D3D12_COMPARISON_FUNC_NEVER;
    }
}

D3D12_FILTER SynthesizeD3D12Filter(const Rdna2SamplerDescriptor& ssharp) noexcept {
    const bool is_aniso =
        (ssharp.xy_mag_filter == Rdna2XyFilter::AnisoPoint ||
         ssharp.xy_mag_filter == Rdna2XyFilter::AnisoLinear ||
         ssharp.xy_min_filter == Rdna2XyFilter::AnisoPoint ||
         ssharp.xy_min_filter == Rdna2XyFilter::AnisoLinear ||
         MaxAnisoRatioToCount(ssharp.max_aniso_ratio) > 1u);

    const bool is_compare = (ssharp.depth_compare_func != Rdna2CompareFunc::Never);

    D3D12_FILTER_REDUCTION_TYPE reduction = D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (is_compare) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    } else if (ssharp.filter_mode == 1u) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MINIMUM;
    } else if (ssharp.filter_mode == 2u) {
        reduction = D3D12_FILTER_REDUCTION_TYPE_MAXIMUM;
    }

    if (is_aniso) {
        return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
    }

    const D3D12_FILTER_TYPE min_f =
        (ssharp.xy_min_filter == Rdna2XyFilter::Point)
            ? D3D12_FILTER_TYPE_POINT
            : D3D12_FILTER_TYPE_LINEAR;
    const D3D12_FILTER_TYPE mag_f =
        (ssharp.xy_mag_filter == Rdna2XyFilter::Point)
            ? D3D12_FILTER_TYPE_POINT
            : D3D12_FILTER_TYPE_LINEAR;
    const D3D12_FILTER_TYPE mip_f =
        (ssharp.mip_filter == Rdna2MipFilter::Linear)
            ? D3D12_FILTER_TYPE_LINEAR
            : D3D12_FILTER_TYPE_POINT;

    return D3D12_ENCODE_BASIC_FILTER(min_f, mag_f, mip_f, reduction);
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Bit-Exact GFX10.3 V# / T# / S# Encoders & Decoders
// ---------------------------------------------------------------------------

void EncodeRdna2BufferDescriptor(const Rdna2BufferDescriptor& desc,
                                 uint32_t out_words[4]) noexcept {
    const uint64_t addr48 = desc.base_address & 0x0000FFFFFFFFFFFFull;
    out_words[0] = static_cast<uint32_t>(addr48 & 0xFFFFFFFFull);
    out_words[1] = static_cast<uint32_t>((addr48 >> 32) & 0xFFFFu) |
                   (static_cast<uint32_t>(desc.stride & 0x3FFFu) << 16) |
                   (static_cast<uint32_t>(desc.swizzle_enable ? 1u : 0u) << 30) |
                   (static_cast<uint32_t>(desc.cache_swizzle ? 1u : 0u) << 31);
    out_words[2] = desc.num_records;
    out_words[3] = (static_cast<uint32_t>(desc.dst_sel_x) & 0x7u) |
                   ((static_cast<uint32_t>(desc.dst_sel_y) & 0x7u) << 3) |
                   ((static_cast<uint32_t>(desc.dst_sel_z) & 0x7u) << 6) |
                   ((static_cast<uint32_t>(desc.dst_sel_w) & 0x7u) << 9) |
                   ((static_cast<uint32_t>(desc.format) & 0x7Fu) << 12) |
                   ((static_cast<uint32_t>(desc.index_stride) & 0x3u) << 21) |
                   (static_cast<uint32_t>(desc.add_tid_enable ? 1u : 0u) << 23) |
                   (static_cast<uint32_t>(desc.resource_level ? 1u : 0u) << 24) |
                   ((static_cast<uint32_t>(desc.oob_select) & 0x3u) << 28) |
                   ((static_cast<uint32_t>(desc.type) & 0x3u) << 30);
}

Rdna2BufferDescriptor DecodeRdna2BufferDescriptor(const uint32_t words[4]) noexcept {
    Rdna2BufferDescriptor d{};
    const uint64_t lo = static_cast<uint64_t>(words[0]);
    const uint64_t hi = static_cast<uint64_t>(words[1] & 0xFFFFu);
    d.base_address = lo | (hi << 32);
    d.stride = static_cast<uint16_t>((words[1] >> 16) & 0x3FFFu);
    d.swizzle_enable = ((words[1] >> 30) & 1u) != 0u;
    d.cache_swizzle = ((words[1] >> 31) & 1u) != 0u;
    d.num_records = words[2];
    d.dst_sel_x = static_cast<Rdna2DstSel>(words[3] & 0x7u);
    d.dst_sel_y = static_cast<Rdna2DstSel>((words[3] >> 3) & 0x7u);
    d.dst_sel_z = static_cast<Rdna2DstSel>((words[3] >> 6) & 0x7u);
    d.dst_sel_w = static_cast<Rdna2DstSel>((words[3] >> 9) & 0x7u);
    d.format = static_cast<uint8_t>((words[3] >> 12) & 0x7Fu);
    d.index_stride = static_cast<uint8_t>((words[3] >> 21) & 0x3u);
    d.add_tid_enable = ((words[3] >> 23) & 1u) != 0u;
    d.resource_level = ((words[3] >> 24) & 1u) != 0u;
    d.oob_select = static_cast<Rdna2BufferOobSelect>((words[3] >> 28) & 0x3u);
    d.type = static_cast<uint8_t>((words[3] >> 30) & 0x3u);
    return d;
}

void EncodeRdna2ImageDescriptor(const Rdna2ImageDescriptor& desc,
                                uint32_t out_words[8]) noexcept {
    const uint64_t base256 = (desc.base_address >> 8) & 0xFFFFFFFFFFull;
    const uint32_t w_m1 = (desc.width > 0u ? (desc.width - 1u) : 0u) & 0x3FFFu;
    const uint32_t h_m1 = (desc.height > 0u ? (desc.height - 1u) : 0u) & 0x3FFFu;
    const uint32_t d_m1 = (desc.depth > 0u ? (desc.depth - 1u) : 0u) & 0x1FFFu;
    const uint64_t meta256 = (desc.meta_data_address >> 8) & 0xFFFFFFFFFFull;

    out_words[0] = static_cast<uint32_t>(base256 & 0xFFFFFFFFull);
    out_words[1] = static_cast<uint32_t>((base256 >> 32) & 0xFFu) |
                   ((static_cast<uint32_t>(desc.min_lod_raw) & 0xFFFu) << 8) |
                   ((static_cast<uint32_t>(desc.format) & 0x1FFu) << 20) |
                   (static_cast<uint32_t>(desc.iterate_256 ? 1u : 0u) << 29) |
                   ((w_m1 & 0x3u) << 30);
    out_words[2] = ((w_m1 >> 2) & 0xFFFu) |
                   ((h_m1 & 0x3FFFu) << 12) |
                   ((static_cast<uint32_t>(desc.perf_mod) & 0x7u) << 28) |
                   (static_cast<uint32_t>(desc.resource_level ? 1u : 0u) << 31);
    out_words[3] = (static_cast<uint32_t>(desc.dst_sel_x) & 0x7u) |
                   ((static_cast<uint32_t>(desc.dst_sel_y) & 0x7u) << 3) |
                   ((static_cast<uint32_t>(desc.dst_sel_z) & 0x7u) << 6) |
                   ((static_cast<uint32_t>(desc.dst_sel_w) & 0x7u) << 9) |
                   ((static_cast<uint32_t>(desc.base_level) & 0xFu) << 12) |
                   ((static_cast<uint32_t>(desc.last_level) & 0xFu) << 16) |
                   ((static_cast<uint32_t>(desc.swizzle_mode) & 0x1Fu) << 20) |
                   ((static_cast<uint32_t>(desc.bc_swizzle) & 0x7u) << 25) |
                   ((static_cast<uint32_t>(desc.type) & 0xFu) << 28);
    out_words[4] = d_m1 |
                   ((static_cast<uint32_t>(desc.base_array) & 0x1FFFu) << 13);
    out_words[5] = (static_cast<uint32_t>(desc.last_array) & 0x1FFFu) |
                   ((static_cast<uint32_t>(desc.max_mip) & 0xFu) << 13) |
                   ((static_cast<uint32_t>(desc.array_pitch) & 0xFu) << 17);
    out_words[6] = (desc.meta_pipe_aligned ? 1u : 0u) |
                   ((desc.dcc_compression_enable ? 1u : 0u) << 1) |
                   ((desc.alpha_is_on_msb ? 1u : 0u) << 2) |
                   ((desc.color_transform ? 1u : 0u) << 3) |
                   ((desc.meta_linear ? 1u : 0u) << 4) |
                   ((desc.write_compress_enable ? 1u : 0u) << 5) |
                   ((static_cast<uint32_t>(desc.max_uncompressed_block_size) & 0x3u) << 6) |
                   ((static_cast<uint32_t>(desc.max_compressed_block_size) & 0x3u) << 8) |
                   (static_cast<uint32_t>((meta256 >> 32) & 0xFFu) << 16);
    out_words[7] = static_cast<uint32_t>(meta256 & 0xFFFFFFFFull);
}

Rdna2ImageDescriptor DecodeRdna2ImageDescriptor(const uint32_t words[8]) noexcept {
    Rdna2ImageDescriptor d{};
    const uint64_t base256 =
        static_cast<uint64_t>(words[0]) | (static_cast<uint64_t>(words[1] & 0xFFu) << 32);
    d.base_address = base256 << 8;
    d.min_lod_raw = static_cast<uint16_t>((words[1] >> 8) & 0xFFFu);
    d.format = static_cast<uint16_t>((words[1] >> 20) & 0x1FFu);
    d.iterate_256 = ((words[1] >> 29) & 1u) != 0u;

    const uint32_t w_lo = (words[1] >> 30) & 0x3u;
    const uint32_t w_hi = words[2] & 0xFFFu;
    d.width = (w_lo | (w_hi << 2)) + 1u;
    d.height = ((words[2] >> 12) & 0x3FFFu) + 1u;
    d.perf_mod = static_cast<uint8_t>((words[2] >> 28) & 0x7u);
    d.resource_level = ((words[2] >> 31) & 1u) != 0u;

    d.dst_sel_x = static_cast<Rdna2DstSel>(words[3] & 0x7u);
    d.dst_sel_y = static_cast<Rdna2DstSel>((words[3] >> 3) & 0x7u);
    d.dst_sel_z = static_cast<Rdna2DstSel>((words[3] >> 6) & 0x7u);
    d.dst_sel_w = static_cast<Rdna2DstSel>((words[3] >> 9) & 0x7u);
    d.base_level = static_cast<uint8_t>((words[3] >> 12) & 0xFu);
    d.last_level = static_cast<uint8_t>((words[3] >> 16) & 0xFu);
    d.swizzle_mode = static_cast<Rdna2SwizzleMode>((words[3] >> 20) & 0x1Fu);
    d.bc_swizzle = static_cast<uint8_t>((words[3] >> 25) & 0x7u);
    d.type = static_cast<Rdna2ImageType>((words[3] >> 28) & 0xFu);

    d.depth = (words[4] & 0x1FFFu) + 1u;
    d.base_array = static_cast<uint16_t>((words[4] >> 13) & 0x1FFFu);
    d.last_array = static_cast<uint16_t>(words[5] & 0x1FFFu);
    d.max_mip = static_cast<uint8_t>((words[5] >> 13) & 0xFu);
    d.array_pitch = static_cast<uint8_t>((words[5] >> 17) & 0xFu);

    d.meta_pipe_aligned = (words[6] & 1u) != 0u;
    d.dcc_compression_enable = ((words[6] >> 1) & 1u) != 0u;
    d.alpha_is_on_msb = ((words[6] >> 2) & 1u) != 0u;
    d.color_transform = ((words[6] >> 3) & 1u) != 0u;
    d.meta_linear = ((words[6] >> 4) & 1u) != 0u;
    d.write_compress_enable = ((words[6] >> 5) & 1u) != 0u;
    d.max_uncompressed_block_size = static_cast<uint8_t>((words[6] >> 6) & 0x3u);
    d.max_compressed_block_size = static_cast<uint8_t>((words[6] >> 8) & 0x3u);

    const uint64_t meta256 =
        static_cast<uint64_t>(words[7]) |
        (static_cast<uint64_t>((words[6] >> 16) & 0xFFu) << 32);
    d.meta_data_address = meta256 << 8;
    return d;
}

void EncodeRdna2SamplerDescriptor(const Rdna2SamplerDescriptor& desc,
                                  uint32_t out_words[4]) noexcept {
    out_words[0] = (static_cast<uint32_t>(desc.clamp_x) & 0x7u) |
                   ((static_cast<uint32_t>(desc.clamp_y) & 0x7u) << 3) |
                   ((static_cast<uint32_t>(desc.clamp_z) & 0x7u) << 6) |
                   ((static_cast<uint32_t>(desc.max_aniso_ratio) & 0x7u) << 9) |
                   ((static_cast<uint32_t>(desc.depth_compare_func) & 0x7u) << 12) |
                   ((desc.force_unnormalized ? 1u : 0u) << 15) |
                   ((static_cast<uint32_t>(desc.aniso_threshold) & 0x7u) << 16) |
                   ((desc.mc_coord_trunc ? 1u : 0u) << 19) |
                   ((desc.force_degamma ? 1u : 0u) << 20) |
                   ((static_cast<uint32_t>(desc.aniso_bias) & 0x3Fu) << 21) |
                   ((desc.trunc_coord ? 1u : 0u) << 27) |
                   ((desc.disable_cube_wrap ? 1u : 0u) << 28) |
                   ((static_cast<uint32_t>(desc.filter_mode) & 0x3u) << 29) |
                   ((desc.compat_mode ? 1u : 0u) << 31);

    out_words[1] = (static_cast<uint32_t>(desc.min_lod_raw) & 0xFFFu) |
                   ((static_cast<uint32_t>(desc.max_lod_raw) & 0xFFFu) << 12) |
                   ((static_cast<uint32_t>(desc.perf_mip) & 0xFu) << 24) |
                   ((static_cast<uint32_t>(desc.perf_z) & 0xFu) << 28);

    const uint32_t bias14 = static_cast<uint32_t>(static_cast<uint16_t>(desc.lod_bias_raw)) & 0x3FFFu;
    out_words[2] = bias14 |
                   ((static_cast<uint32_t>(desc.lod_bias_sec) & 0x3Fu) << 14) |
                   ((static_cast<uint32_t>(desc.xy_mag_filter) & 0x3u) << 20) |
                   ((static_cast<uint32_t>(desc.xy_min_filter) & 0x3u) << 22) |
                   ((static_cast<uint32_t>(desc.z_filter) & 0x3u) << 24) |
                   ((static_cast<uint32_t>(desc.mip_filter) & 0x3u) << 26) |
                   ((desc.aniso_override ? 1u : 0u) << 28) |
                   ((desc.blend_zero_prt ? 1u : 0u) << 29);

    out_words[3] = (static_cast<uint32_t>(desc.border_color_ptr) & 0xFFFu) |
                   ((static_cast<uint32_t>(desc.border_color_type) & 0x3u) << 30);
}

Rdna2SamplerDescriptor DecodeRdna2SamplerDescriptor(const uint32_t words[4]) noexcept {
    Rdna2SamplerDescriptor d{};
    d.clamp_x = static_cast<Rdna2ClampMode>(words[0] & 0x7u);
    d.clamp_y = static_cast<Rdna2ClampMode>((words[0] >> 3) & 0x7u);
    d.clamp_z = static_cast<Rdna2ClampMode>((words[0] >> 6) & 0x7u);
    d.max_aniso_ratio = static_cast<uint8_t>((words[0] >> 9) & 0x7u);
    d.depth_compare_func = static_cast<Rdna2CompareFunc>((words[0] >> 12) & 0x7u);
    d.force_unnormalized = ((words[0] >> 15) & 1u) != 0u;
    d.aniso_threshold = static_cast<uint8_t>((words[0] >> 16) & 0x7u);
    d.mc_coord_trunc = ((words[0] >> 19) & 1u) != 0u;
    d.force_degamma = ((words[0] >> 20) & 1u) != 0u;
    d.aniso_bias = static_cast<uint8_t>((words[0] >> 21) & 0x3Fu);
    d.trunc_coord = ((words[0] >> 27) & 1u) != 0u;
    d.disable_cube_wrap = ((words[0] >> 28) & 1u) != 0u;
    d.filter_mode = static_cast<uint8_t>((words[0] >> 29) & 0x3u);
    d.compat_mode = ((words[0] >> 31) & 1u) != 0u;

    d.min_lod_raw = static_cast<uint16_t>(words[1] & 0xFFFu);
    d.max_lod_raw = static_cast<uint16_t>((words[1] >> 12) & 0xFFFu);
    d.perf_mip = static_cast<uint8_t>((words[1] >> 24) & 0xFu);
    d.perf_z = static_cast<uint8_t>((words[1] >> 28) & 0xFu);

    uint16_t bias14 = static_cast<uint16_t>(words[2] & 0x3FFFu);
    if ((bias14 & 0x2000u) != 0u) {
        bias14 = static_cast<uint16_t>(bias14 | 0xC000u);
    }
    d.lod_bias_raw = static_cast<int16_t>(bias14);
    d.lod_bias_sec = static_cast<uint8_t>((words[2] >> 14) & 0x3Fu);
    d.xy_mag_filter = static_cast<Rdna2XyFilter>((words[2] >> 20) & 0x3u);
    d.xy_min_filter = static_cast<Rdna2XyFilter>((words[2] >> 22) & 0x3u);
    d.z_filter = static_cast<Rdna2MipFilter>((words[2] >> 24) & 0x3u);
    d.mip_filter = static_cast<Rdna2MipFilter>((words[2] >> 26) & 0x3u);
    d.aniso_override = ((words[2] >> 28) & 1u) != 0u;
    d.blend_zero_prt = ((words[2] >> 29) & 1u) != 0u;

    d.border_color_ptr = static_cast<uint16_t>(words[3] & 0xFFFu);
    d.border_color_type = static_cast<Rdna2BorderColorType>((words[3] >> 30) & 0x3u);
    return d;
}

// ---------------------------------------------------------------------------
// Format Conversion Helpers
// ---------------------------------------------------------------------------

uint32_t Rdna2BufferFormatToDxgi(uint8_t fmt) noexcept {
    switch (static_cast<Rdna2MtbufFormat>(fmt)) {
        case Rdna2MtbufFormat::R8_UNORM:            return kDxgiR8Unorm;
        case Rdna2MtbufFormat::R8_SNORM:            return kDxgiR8Snorm;
        case Rdna2MtbufFormat::R8_UINT:             return kDxgiR8Uint;
        case Rdna2MtbufFormat::R8_SINT:             return kDxgiR8Sint;
        case Rdna2MtbufFormat::R16_UNORM:           return kDxgiR16Unorm;
        case Rdna2MtbufFormat::R16_SNORM:           return kDxgiR16Snorm;
        case Rdna2MtbufFormat::R16_UINT:            return kDxgiR16Uint;
        case Rdna2MtbufFormat::R16_SINT:            return kDxgiR16Sint;
        case Rdna2MtbufFormat::R16_FLOAT:           return kDxgiR16Float;
        case Rdna2MtbufFormat::R8G8_UNORM:          return kDxgiR8G8Unorm;
        case Rdna2MtbufFormat::R8G8_SNORM:          return kDxgiR8G8Snorm;
        case Rdna2MtbufFormat::R8G8_UINT:           return kDxgiR8G8Uint;
        case Rdna2MtbufFormat::R8G8_SINT:           return kDxgiR8G8Sint;
        case Rdna2MtbufFormat::R32_UINT:            return kDxgiR32Uint;
        case Rdna2MtbufFormat::R32_SINT:            return kDxgiR32Sint;
        case Rdna2MtbufFormat::R32_FLOAT:           return kDxgiR32Float;
        case Rdna2MtbufFormat::R16G16_UNORM:        return kDxgiR16G16Unorm;
        case Rdna2MtbufFormat::R16G16_SNORM:        return kDxgiR16G16Snorm;
        case Rdna2MtbufFormat::R16G16_UINT:         return kDxgiR16G16Uint;
        case Rdna2MtbufFormat::R16G16_SINT:         return kDxgiR16G16Sint;
        case Rdna2MtbufFormat::R16G16_FLOAT:        return kDxgiR16G16Float;
        case Rdna2MtbufFormat::R8G8B8A8_UNORM:      return kDxgiR8G8B8A8Unorm;
        case Rdna2MtbufFormat::R8G8B8A8_SNORM:      return kDxgiR8G8B8A8Snorm;
        case Rdna2MtbufFormat::R8G8B8A8_UINT:       return kDxgiR8G8B8A8Uint;
        case Rdna2MtbufFormat::R8G8B8A8_SINT:       return kDxgiR8G8B8A8Sint;
        case Rdna2MtbufFormat::R32G32_FLOAT:        return kDxgiR32G32Float;
        case Rdna2MtbufFormat::R16G16B16A16_UNORM:  return kDxgiR16G16B16A16Unorm;
        case Rdna2MtbufFormat::R16G16B16A16_SNORM:  return kDxgiR16G16B16A16Snorm;
        case Rdna2MtbufFormat::R16G16B16A16_UINT:   return kDxgiR16G16B16A16Uint;
        case Rdna2MtbufFormat::R16G16B16A16_SINT:   return kDxgiR16G16B16A16Sint;
        case Rdna2MtbufFormat::R16G16B16A16_FLOAT:  return kDxgiR16G16B16A16Float;
        case Rdna2MtbufFormat::R32G32B32_FLOAT:     return kDxgiR32G32B32Float;
        case Rdna2MtbufFormat::R32G32B32A32_FLOAT:  return kDxgiR32G32B32A32Float;
        default:                                    break;
    }
    // Fallback for non-colliding hardware GFX10.3 unified 7-bit UFMT codes (V# word3[18:12])
    switch (static_cast<Rdna2Gfx103Ufmt>(fmt & 0x7Fu)) {
        case Rdna2Gfx103Ufmt::R32_FLOAT:            return kDxgiR32Float;
        case Rdna2Gfx103Ufmt::R16G16_UNORM:         return kDxgiR16G16Unorm;
        case Rdna2Gfx103Ufmt::R16G16_SNORM:         return kDxgiR16G16Snorm;
        case Rdna2Gfx103Ufmt::R16G16_USCALED:
        case Rdna2Gfx103Ufmt::R16G16_UINT:          return kDxgiR16G16Uint;
        case Rdna2Gfx103Ufmt::R16G16_SSCALED:       return kDxgiR16G16Sint;
        case Rdna2Gfx103Ufmt::R10G11B11_FLOAT:
        case Rdna2Gfx103Ufmt::R11G11B10_FLOAT:      return kDxgiR11G11B10Float;
        case Rdna2Gfx103Ufmt::R10G10B10A2_UNORM:    return kDxgiR10G10B10A2Unorm;
        case Rdna2Gfx103Ufmt::R8G8B8A8_UNORM:       return kDxgiR8G8B8A8Unorm;
        case Rdna2Gfx103Ufmt::R8G8B8A8_SNORM:       return kDxgiR8G8B8A8Snorm;
        case Rdna2Gfx103Ufmt::R8G8B8A8_USCALED:     return kDxgiR8G8B8A8Uint;
        case Rdna2Gfx103Ufmt::R8G8B8A8_SSCALED:
        case Rdna2Gfx103Ufmt::R8G8B8A8_SINT:        return kDxgiR8G8B8A8Sint;
        case Rdna2Gfx103Ufmt::R32G32_UINT:          return kDxgiR32G32Uint;
        case Rdna2Gfx103Ufmt::R32G32_SINT:          return kDxgiR32G32Sint;
        case Rdna2Gfx103Ufmt::R32G32_FLOAT:         return kDxgiR32G32Float;
        case Rdna2Gfx103Ufmt::R16G16B16A16_UNORM:   return kDxgiR16G16B16A16Unorm;
        case Rdna2Gfx103Ufmt::R16G16B16A16_SNORM:   return kDxgiR16G16B16A16Snorm;
        case Rdna2Gfx103Ufmt::R16G16B16A16_USCALED:
        case Rdna2Gfx103Ufmt::R16G16B16A16_UINT:    return kDxgiR16G16B16A16Uint;
        case Rdna2Gfx103Ufmt::R16G16B16A16_SSCALED:
        case Rdna2Gfx103Ufmt::R16G16B16A16_SINT:    return kDxgiR16G16B16A16Sint;
        case Rdna2Gfx103Ufmt::R16G16B16A16_FLOAT:   return kDxgiR16G16B16A16Float;
        case Rdna2Gfx103Ufmt::R32G32B32_UINT:       return kDxgiR32G32B32Uint;
        case Rdna2Gfx103Ufmt::R32G32B32_SINT:       return kDxgiR32G32B32Sint;
        case Rdna2Gfx103Ufmt::R32G32B32A32_UINT:    return kDxgiR32G32B32A32Uint;
        case Rdna2Gfx103Ufmt::R32G32B32A32_SINT:    return kDxgiR32G32B32A32Sint;
        case Rdna2Gfx103Ufmt::R32G32B32A32_FLOAT:   return kDxgiR32G32B32A32Float;
        default:                                    return kDxgiUnknown;
    }
}

namespace {

uint32_t Rdna2Gfx103UfmtToDxgi(uint8_t ufmt) noexcept {
    switch (static_cast<Rdna2Gfx103Ufmt>(ufmt & 0x7Fu)) {
        case Rdna2Gfx103Ufmt::R8_UNORM:            return kDxgiR8Unorm;
        case Rdna2Gfx103Ufmt::R8_SNORM:            return kDxgiR8Snorm;
        case Rdna2Gfx103Ufmt::R8_USCALED:
        case Rdna2Gfx103Ufmt::R8_UINT:             return kDxgiR8Uint;
        case Rdna2Gfx103Ufmt::R8_SSCALED:
        case Rdna2Gfx103Ufmt::R8_SINT:             return kDxgiR8Sint;
        case Rdna2Gfx103Ufmt::R16_UNORM:           return kDxgiR16Unorm;
        case Rdna2Gfx103Ufmt::R16_SNORM:           return kDxgiR16Snorm;
        case Rdna2Gfx103Ufmt::R16_USCALED:
        case Rdna2Gfx103Ufmt::R16_UINT:            return kDxgiR16Uint;
        case Rdna2Gfx103Ufmt::R16_SSCALED:
        case Rdna2Gfx103Ufmt::R16_SINT:            return kDxgiR16Sint;
        case Rdna2Gfx103Ufmt::R16_FLOAT:           return kDxgiR16Float;
        case Rdna2Gfx103Ufmt::R8G8_UNORM:          return kDxgiR8G8Unorm;
        case Rdna2Gfx103Ufmt::R8G8_SNORM:          return kDxgiR8G8Snorm;
        case Rdna2Gfx103Ufmt::R8G8_USCALED:
        case Rdna2Gfx103Ufmt::R8G8_UINT:           return kDxgiR8G8Uint;
        case Rdna2Gfx103Ufmt::R8G8_SSCALED:
        case Rdna2Gfx103Ufmt::R8G8_SINT:           return kDxgiR8G8Sint;
        case Rdna2Gfx103Ufmt::R32_UINT:            return kDxgiR32Uint;
        case Rdna2Gfx103Ufmt::R32_SINT:            return kDxgiR32Sint;
        case Rdna2Gfx103Ufmt::R32_FLOAT:           return kDxgiR32Float;
        case Rdna2Gfx103Ufmt::R16G16_UNORM:        return kDxgiR16G16Unorm;
        case Rdna2Gfx103Ufmt::R16G16_SNORM:        return kDxgiR16G16Snorm;
        case Rdna2Gfx103Ufmt::R16G16_USCALED:
        case Rdna2Gfx103Ufmt::R16G16_UINT:         return kDxgiR16G16Uint;
        case Rdna2Gfx103Ufmt::R16G16_SSCALED:
        case Rdna2Gfx103Ufmt::R16G16_SINT:         return kDxgiR16G16Sint;
        case Rdna2Gfx103Ufmt::R16G16_FLOAT:        return kDxgiR16G16Float;
        case Rdna2Gfx103Ufmt::R10G11B11_FLOAT:
        case Rdna2Gfx103Ufmt::R11G11B10_FLOAT:     return kDxgiR11G11B10Float;
        case Rdna2Gfx103Ufmt::R10G10B10A2_UNORM:
        case Rdna2Gfx103Ufmt::R2G10B10A10_UNORM:   return kDxgiR10G10B10A2Unorm;
        case Rdna2Gfx103Ufmt::R10G10B10A2_UINT:
        case Rdna2Gfx103Ufmt::R2G10B10A10_UINT:    return kDxgiR10G10B10A2Uint;
        case Rdna2Gfx103Ufmt::R8G8B8A8_UNORM:      return kDxgiR8G8B8A8Unorm;
        case Rdna2Gfx103Ufmt::R8G8B8A8_SNORM:      return kDxgiR8G8B8A8Snorm;
        case Rdna2Gfx103Ufmt::R8G8B8A8_USCALED:
        case Rdna2Gfx103Ufmt::R8G8B8A8_UINT:       return kDxgiR8G8B8A8Uint;
        case Rdna2Gfx103Ufmt::R8G8B8A8_SSCALED:
        case Rdna2Gfx103Ufmt::R8G8B8A8_SINT:       return kDxgiR8G8B8A8Sint;
        case Rdna2Gfx103Ufmt::R32G32_UINT:         return kDxgiR32G32Uint;
        case Rdna2Gfx103Ufmt::R32G32_SINT:         return kDxgiR32G32Sint;
        case Rdna2Gfx103Ufmt::R32G32_FLOAT:        return kDxgiR32G32Float;
        case Rdna2Gfx103Ufmt::R16G16B16A16_UNORM:  return kDxgiR16G16B16A16Unorm;
        case Rdna2Gfx103Ufmt::R16G16B16A16_SNORM:  return kDxgiR16G16B16A16Snorm;
        case Rdna2Gfx103Ufmt::R16G16B16A16_USCALED:
        case Rdna2Gfx103Ufmt::R16G16B16A16_UINT:   return kDxgiR16G16B16A16Uint;
        case Rdna2Gfx103Ufmt::R16G16B16A16_SSCALED:
        case Rdna2Gfx103Ufmt::R16G16B16A16_SINT:   return kDxgiR16G16B16A16Sint;
        case Rdna2Gfx103Ufmt::R16G16B16A16_FLOAT:  return kDxgiR16G16B16A16Float;
        case Rdna2Gfx103Ufmt::R32G32B32_UINT:      return kDxgiR32G32B32Uint;
        case Rdna2Gfx103Ufmt::R32G32B32_SINT:      return kDxgiR32G32B32Sint;
        case Rdna2Gfx103Ufmt::R32G32B32_FLOAT:     return kDxgiR32G32B32Float;
        case Rdna2Gfx103Ufmt::R32G32B32A32_UINT:   return kDxgiR32G32B32A32Uint;
        case Rdna2Gfx103Ufmt::R32G32B32A32_SINT:   return kDxgiR32G32B32A32Sint;
        case Rdna2Gfx103Ufmt::R32G32B32A32_FLOAT:  return kDxgiR32G32B32A32Float;
        default:                                   return kDxgiUnknown;
    }
}

void ResolveVSharpDxgiFormat(const Rdna2BufferDescriptor& vsharp,
                             uint32_t& out_dxgi_fmt,
                             bool& out_raw_byte_address) noexcept {
    // Hardware GFX10.3 SQ_RSRC_BUF V# descriptors encode word3[31:30] == 2 (type == 2)
    // and word3[24] == 1 (resource_level == true) with unified 7-bit FORMAT in word3[18:12].
    if (vsharp.resource_level && vsharp.type == 2u) {
        out_dxgi_fmt = Rdna2Gfx103UfmtToDxgi(vsharp.format);
        out_raw_byte_address =
            (vsharp.format == 0u) ||
            (vsharp.format == static_cast<uint8_t>(Rdna2Gfx103Ufmt::R32_UINT) &&
             vsharp.oob_select == Rdna2BufferOobSelect::NumRecordsAddress);
    } else {
        out_dxgi_fmt = Rdna2BufferFormatToDxgi(vsharp.format);
        out_raw_byte_address =
            (vsharp.format == static_cast<uint8_t>(Rdna2MtbufFormat::Invalid)) ||
            (vsharp.format == static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT) &&
             vsharp.oob_select == Rdna2BufferOobSelect::NumRecordsAddress);
    }
}

}  // namespace

uint8_t DxgiToRdna2BufferFormat(uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
        case kDxgiR8Unorm:           return static_cast<uint8_t>(Rdna2MtbufFormat::R8_UNORM);
        case kDxgiR8Snorm:           return static_cast<uint8_t>(Rdna2MtbufFormat::R8_SNORM);
        case kDxgiR8Uint:            return static_cast<uint8_t>(Rdna2MtbufFormat::R8_UINT);
        case kDxgiR8Sint:            return static_cast<uint8_t>(Rdna2MtbufFormat::R8_SINT);
        case kDxgiR16Unorm:          return static_cast<uint8_t>(Rdna2MtbufFormat::R16_UNORM);
        case kDxgiR16Snorm:          return static_cast<uint8_t>(Rdna2MtbufFormat::R16_SNORM);
        case kDxgiR16Uint:           return static_cast<uint8_t>(Rdna2MtbufFormat::R16_UINT);
        case kDxgiR16Sint:           return static_cast<uint8_t>(Rdna2MtbufFormat::R16_SINT);
        case kDxgiR16Float:          return static_cast<uint8_t>(Rdna2MtbufFormat::R16_FLOAT);
        case kDxgiR8G8Unorm:         return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8_UNORM);
        case kDxgiR8G8Snorm:         return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8_SNORM);
        case kDxgiR8G8Uint:          return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8_UINT);
        case kDxgiR8G8Sint:          return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8_SINT);
        case kDxgiR32Uint:           return static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT);
        case kDxgiR32Sint:           return static_cast<uint8_t>(Rdna2MtbufFormat::R32_SINT);
        case kDxgiR32Float:          return static_cast<uint8_t>(Rdna2MtbufFormat::R32_FLOAT);
        case kDxgiR16G16Unorm:       return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16_UNORM);
        case kDxgiR16G16Snorm:       return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16_SNORM);
        case kDxgiR16G16Uint:        return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16_UINT);
        case kDxgiR16G16Sint:        return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16_SINT);
        case kDxgiR16G16Float:       return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16_FLOAT);
        case kDxgiR8G8B8A8Unorm:     return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8B8A8_UNORM);
        case kDxgiR8G8B8A8Snorm:     return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8B8A8_SNORM);
        case kDxgiR8G8B8A8Uint:      return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8B8A8_UINT);
        case kDxgiR8G8B8A8Sint:      return static_cast<uint8_t>(Rdna2MtbufFormat::R8G8B8A8_SINT);
        case kDxgiR32G32Float:       return static_cast<uint8_t>(Rdna2MtbufFormat::R32G32_FLOAT);
        case kDxgiR16G16B16A16Unorm: return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_UNORM);
        case kDxgiR16G16B16A16Snorm: return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_SNORM);
        case kDxgiR16G16B16A16Uint:  return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_UINT);
        case kDxgiR16G16B16A16Sint:  return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_SINT);
        case kDxgiR16G16B16A16Float: return static_cast<uint8_t>(Rdna2MtbufFormat::R16G16B16A16_FLOAT);
        case kDxgiR32G32B32Float:    return static_cast<uint8_t>(Rdna2MtbufFormat::R32G32B32_FLOAT);
        case kDxgiR32G32B32A32Float: return static_cast<uint8_t>(Rdna2MtbufFormat::R32G32B32A32_FLOAT);
        default:                     return static_cast<uint8_t>(Rdna2MtbufFormat::Invalid);
    }
}

uint32_t Rdna2ImageFormatToDxgi(uint16_t fmt) noexcept {
    switch (static_cast<Rdna2ImageFormat>(fmt)) {
        case Rdna2ImageFormat::R8_UNORM:            return kDxgiR8Unorm;
        case Rdna2ImageFormat::R8_SNORM:            return kDxgiR8Snorm;
        case Rdna2ImageFormat::R8_UINT:             return kDxgiR8Uint;
        case Rdna2ImageFormat::R8_SINT:             return kDxgiR8Sint;
        case Rdna2ImageFormat::R16_UNORM:           return kDxgiR16Unorm;
        case Rdna2ImageFormat::R16_SNORM:           return kDxgiR16Snorm;
        case Rdna2ImageFormat::R16_UINT:            return kDxgiR16Uint;
        case Rdna2ImageFormat::R16_SINT:            return kDxgiR16Sint;
        case Rdna2ImageFormat::R16_FLOAT:           return kDxgiR16Float;
        case Rdna2ImageFormat::R8G8_UNORM:          return kDxgiR8G8Unorm;
        case Rdna2ImageFormat::R8G8_SNORM:          return kDxgiR8G8Snorm;
        case Rdna2ImageFormat::R8G8_UINT:           return kDxgiR8G8Uint;
        case Rdna2ImageFormat::R8G8_SINT:           return kDxgiR8G8Sint;
        case Rdna2ImageFormat::R32_UINT:            return kDxgiR32Uint;
        case Rdna2ImageFormat::R32_SINT:            return kDxgiR32Sint;
        case Rdna2ImageFormat::R32_FLOAT:           return kDxgiR32Float;
        case Rdna2ImageFormat::R16G16_UNORM:        return kDxgiR16G16Unorm;
        case Rdna2ImageFormat::R16G16_SNORM:        return kDxgiR16G16Snorm;
        case Rdna2ImageFormat::R16G16_UINT:         return kDxgiR16G16Uint;
        case Rdna2ImageFormat::R16G16_SINT:         return kDxgiR16G16Sint;
        case Rdna2ImageFormat::R16G16_FLOAT:        return kDxgiR16G16Float;
        case Rdna2ImageFormat::R10G10B10A2_UNORM:   return kDxgiR10G10B10A2Unorm;
        case Rdna2ImageFormat::R10G10B10A2_UINT:    return kDxgiR10G10B10A2Uint;
        case Rdna2ImageFormat::R11G11B10_FLOAT:     return kDxgiR11G11B10Float;
        case Rdna2ImageFormat::R8G8B8A8_UNORM:      return kDxgiR8G8B8A8Unorm;
        case Rdna2ImageFormat::R8G8B8A8_SNORM:      return kDxgiR8G8B8A8Snorm;
        case Rdna2ImageFormat::R8G8B8A8_UINT:       return kDxgiR8G8B8A8Uint;
        case Rdna2ImageFormat::R8G8B8A8_SINT:       return kDxgiR8G8B8A8Sint;
        case Rdna2ImageFormat::R8G8B8A8_SRGB:       return kDxgiR8G8B8A8UnormSrgb;
        case Rdna2ImageFormat::B8G8R8A8_UNORM:      return kDxgiB8G8R8A8Unorm;
        case Rdna2ImageFormat::B8G8R8A8_SRGB:       return kDxgiB8G8R8A8UnormSrgb;
        case Rdna2ImageFormat::R32G32_UINT:         return kDxgiR32G32Uint;
        case Rdna2ImageFormat::R32G32_SINT:         return kDxgiR32G32Sint;
        case Rdna2ImageFormat::R32G32_FLOAT:        return kDxgiR32G32Float;
        case Rdna2ImageFormat::R16G16B16A16_UNORM:  return kDxgiR16G16B16A16Unorm;
        case Rdna2ImageFormat::R16G16B16A16_SNORM:  return kDxgiR16G16B16A16Snorm;
        case Rdna2ImageFormat::R16G16B16A16_UINT:   return kDxgiR16G16B16A16Uint;
        case Rdna2ImageFormat::R16G16B16A16_SINT:   return kDxgiR16G16B16A16Sint;
        case Rdna2ImageFormat::R16G16B16A16_FLOAT:  return kDxgiR16G16B16A16Float;
        case Rdna2ImageFormat::R32G32B32A32_UINT:   return kDxgiR32G32B32A32Uint;
        case Rdna2ImageFormat::R32G32B32A32_SINT:   return kDxgiR32G32B32A32Sint;
        case Rdna2ImageFormat::R32G32B32A32_FLOAT:  return kDxgiR32G32B32A32Float;
        case Rdna2ImageFormat::BC1_UNORM:           return kDxgiBc1Unorm;
        case Rdna2ImageFormat::BC1_SRGB:            return kDxgiBc1UnormSrgb;
        case Rdna2ImageFormat::BC2_UNORM:           return kDxgiBc2Unorm;
        case Rdna2ImageFormat::BC3_UNORM:           return kDxgiBc3Unorm;
        case Rdna2ImageFormat::BC4_UNORM:           return kDxgiBc4Unorm;
        case Rdna2ImageFormat::BC5_UNORM:           return kDxgiBc5Unorm;
        case Rdna2ImageFormat::BC6H_UF16:           return kDxgiBc6hUf16;
        case Rdna2ImageFormat::BC7_UNORM:           return kDxgiBc7Unorm;
        case Rdna2ImageFormat::BC7_SRGB:            return kDxgiBc7UnormSrgb;
        case Rdna2ImageFormat::D32_FLOAT:           return kDxgiD32Float;
        default:                                    return kDxgiUnknown;
    }
}

uint16_t DxgiToRdna2ImageFormat(uint32_t dxgi_format) noexcept {
    switch (dxgi_format) {
        case kDxgiR8Unorm:           return static_cast<uint16_t>(Rdna2ImageFormat::R8_UNORM);
        case kDxgiR8Snorm:           return static_cast<uint16_t>(Rdna2ImageFormat::R8_SNORM);
        case kDxgiR8Uint:            return static_cast<uint16_t>(Rdna2ImageFormat::R8_UINT);
        case kDxgiR8Sint:            return static_cast<uint16_t>(Rdna2ImageFormat::R8_SINT);
        case kDxgiR16Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::R16_UNORM);
        case kDxgiR16Snorm:          return static_cast<uint16_t>(Rdna2ImageFormat::R16_SNORM);
        case kDxgiR16Uint:           return static_cast<uint16_t>(Rdna2ImageFormat::R16_UINT);
        case kDxgiR16Sint:           return static_cast<uint16_t>(Rdna2ImageFormat::R16_SINT);
        case kDxgiR16Float:          return static_cast<uint16_t>(Rdna2ImageFormat::R16_FLOAT);
        case kDxgiR8G8Unorm:         return static_cast<uint16_t>(Rdna2ImageFormat::R8G8_UNORM);
        case kDxgiR8G8Snorm:         return static_cast<uint16_t>(Rdna2ImageFormat::R8G8_SNORM);
        case kDxgiR8G8Uint:          return static_cast<uint16_t>(Rdna2ImageFormat::R8G8_UINT);
        case kDxgiR8G8Sint:          return static_cast<uint16_t>(Rdna2ImageFormat::R8G8_SINT);
        case kDxgiR32Uint:           return static_cast<uint16_t>(Rdna2ImageFormat::R32_UINT);
        case kDxgiR32Sint:           return static_cast<uint16_t>(Rdna2ImageFormat::R32_SINT);
        case kDxgiR32Float:          return static_cast<uint16_t>(Rdna2ImageFormat::R32_FLOAT);
        case kDxgiR16G16Unorm:       return static_cast<uint16_t>(Rdna2ImageFormat::R16G16_UNORM);
        case kDxgiR16G16Snorm:       return static_cast<uint16_t>(Rdna2ImageFormat::R16G16_SNORM);
        case kDxgiR16G16Uint:        return static_cast<uint16_t>(Rdna2ImageFormat::R16G16_UINT);
        case kDxgiR16G16Sint:        return static_cast<uint16_t>(Rdna2ImageFormat::R16G16_SINT);
        case kDxgiR16G16Float:       return static_cast<uint16_t>(Rdna2ImageFormat::R16G16_FLOAT);
        case kDxgiR10G10B10A2Unorm:  return static_cast<uint16_t>(Rdna2ImageFormat::R10G10B10A2_UNORM);
        case kDxgiR10G10B10A2Uint:   return static_cast<uint16_t>(Rdna2ImageFormat::R10G10B10A2_UINT);
        case kDxgiR11G11B10Float:    return static_cast<uint16_t>(Rdna2ImageFormat::R11G11B10_FLOAT);
        case kDxgiR8G8B8A8Unorm:     return static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_UNORM);
        case kDxgiR8G8B8A8Snorm:     return static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_SNORM);
        case kDxgiR8G8B8A8Uint:      return static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_UINT);
        case kDxgiR8G8B8A8Sint:      return static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_SINT);
        case kDxgiR8G8B8A8UnormSrgb: return static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_SRGB);
        case kDxgiB8G8R8A8Unorm:     return static_cast<uint16_t>(Rdna2ImageFormat::B8G8R8A8_UNORM);
        case kDxgiB8G8R8A8UnormSrgb: return static_cast<uint16_t>(Rdna2ImageFormat::B8G8R8A8_SRGB);
        case kDxgiR32G32Uint:        return static_cast<uint16_t>(Rdna2ImageFormat::R32G32_UINT);
        case kDxgiR32G32Sint:        return static_cast<uint16_t>(Rdna2ImageFormat::R32G32_SINT);
        case kDxgiR32G32Float:       return static_cast<uint16_t>(Rdna2ImageFormat::R32G32_FLOAT);
        case kDxgiR16G16B16A16Unorm: return static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_UNORM);
        case kDxgiR16G16B16A16Snorm: return static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_SNORM);
        case kDxgiR16G16B16A16Uint:  return static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_UINT);
        case kDxgiR16G16B16A16Sint:  return static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_SINT);
        case kDxgiR16G16B16A16Float: return static_cast<uint16_t>(Rdna2ImageFormat::R16G16B16A16_FLOAT);
        case kDxgiR32G32B32A32Uint:  return static_cast<uint16_t>(Rdna2ImageFormat::R32G32B32A32_UINT);
        case kDxgiR32G32B32A32Sint:  return static_cast<uint16_t>(Rdna2ImageFormat::R32G32B32A32_SINT);
        case kDxgiR32G32B32A32Float: return static_cast<uint16_t>(Rdna2ImageFormat::R32G32B32A32_FLOAT);
        case kDxgiBc1Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC1_UNORM);
        case kDxgiBc1UnormSrgb:      return static_cast<uint16_t>(Rdna2ImageFormat::BC1_SRGB);
        case kDxgiBc2Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC2_UNORM);
        case kDxgiBc3Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC3_UNORM);
        case kDxgiBc4Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC4_UNORM);
        case kDxgiBc5Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC5_UNORM);
        case kDxgiBc6hUf16:          return static_cast<uint16_t>(Rdna2ImageFormat::BC6H_UF16);
        case kDxgiBc7Unorm:          return static_cast<uint16_t>(Rdna2ImageFormat::BC7_UNORM);
        case kDxgiBc7UnormSrgb:      return static_cast<uint16_t>(Rdna2ImageFormat::BC7_SRGB);
        case kDxgiD32Float:          return static_cast<uint16_t>(Rdna2ImageFormat::D32_FLOAT);
        default:                     return static_cast<uint16_t>(Rdna2ImageFormat::Invalid);
    }
}

uint32_t DxgiFormatToVkFormat(uint32_t dxgi_format) noexcept {
    // Khronos VkFormat numeric values.
    switch (dxgi_format) {
        case kDxgiR8Unorm:           return 9u;    // VK_FORMAT_R8_UNORM
        case kDxgiR8Snorm:           return 10u;   // VK_FORMAT_R8_SNORM
        case kDxgiR8Uint:            return 13u;   // VK_FORMAT_R8_UINT
        case kDxgiR8Sint:            return 14u;   // VK_FORMAT_R8_SINT
        case kDxgiR8G8Unorm:         return 16u;   // VK_FORMAT_R8G8_UNORM
        case kDxgiR8G8Snorm:         return 17u;   // VK_FORMAT_R8G8_SNORM
        case kDxgiR8G8Uint:          return 20u;   // VK_FORMAT_R8G8_UINT
        case kDxgiR8G8Sint:          return 21u;   // VK_FORMAT_R8G8_SINT
        case kDxgiR8G8B8A8Unorm:     return 37u;   // VK_FORMAT_R8G8B8A8_UNORM
        case kDxgiR8G8B8A8Snorm:     return 38u;   // VK_FORMAT_R8G8B8A8_SNORM
        case kDxgiR8G8B8A8Uint:      return 41u;   // VK_FORMAT_R8G8B8A8_UINT
        case kDxgiR8G8B8A8Sint:      return 42u;   // VK_FORMAT_R8G8B8A8_SINT
        case kDxgiR8G8B8A8UnormSrgb: return 43u;   // VK_FORMAT_R8G8B8A8_SRGB
        case kDxgiB8G8R8A8Unorm:     return 44u;   // VK_FORMAT_B8G8R8A8_UNORM
        case kDxgiB8G8R8A8UnormSrgb: return 50u;   // VK_FORMAT_B8G8R8A8_SRGB
        case kDxgiR10G10B10A2Unorm:  return 64u;   // VK_FORMAT_A2B10G10R10_UNORM_PACK32
        case kDxgiR10G10B10A2Uint:   return 68u;   // VK_FORMAT_A2B10G10R10_UINT_PACK32
        case kDxgiR16Unorm:          return 70u;   // VK_FORMAT_R16_UNORM
        case kDxgiR16Snorm:          return 71u;   // VK_FORMAT_R16_SNORM
        case kDxgiR16Uint:           return 74u;   // VK_FORMAT_R16_UINT
        case kDxgiR16Sint:           return 75u;   // VK_FORMAT_R16_SINT
        case kDxgiR16Float:          return 76u;   // VK_FORMAT_R16_SFLOAT
        case kDxgiR16G16Unorm:       return 77u;   // VK_FORMAT_R16G16_UNORM
        case kDxgiR16G16Snorm:       return 78u;   // VK_FORMAT_R16G16_SNORM
        case kDxgiR16G16Uint:        return 81u;   // VK_FORMAT_R16G16_UINT
        case kDxgiR16G16Sint:        return 82u;   // VK_FORMAT_R16G16_SINT
        case kDxgiR16G16Float:       return 83u;   // VK_FORMAT_R16G16_SFLOAT
        case kDxgiR16G16B16A16Unorm: return 91u;   // VK_FORMAT_R16G16B16A16_UNORM
        case kDxgiR16G16B16A16Snorm: return 92u;   // VK_FORMAT_R16G16B16A16_SNORM
        case kDxgiR16G16B16A16Uint:  return 95u;   // VK_FORMAT_R16G16B16A16_UINT
        case kDxgiR16G16B16A16Sint:  return 96u;   // VK_FORMAT_R16G16B16A16_SINT
        case kDxgiR16G16B16A16Float: return 97u;   // VK_FORMAT_R16G16B16A16_SFLOAT
        case kDxgiR32Uint:           return 98u;   // VK_FORMAT_R32_UINT
        case kDxgiR32Sint:           return 99u;   // VK_FORMAT_R32_SINT
        case kDxgiR32Float:          return 100u;  // VK_FORMAT_R32_SFLOAT
        case kDxgiR32G32Uint:        return 101u;  // VK_FORMAT_R32G32_UINT
        case kDxgiR32G32Sint:        return 102u;  // VK_FORMAT_R32G32_SINT
        case kDxgiR32G32Float:       return 103u;  // VK_FORMAT_R32G32_SFLOAT
        case kDxgiR32G32B32Float:    return 106u;  // VK_FORMAT_R32G32B32_SFLOAT
        case kDxgiR32G32B32A32Uint:  return 107u;  // VK_FORMAT_R32G32B32A32_UINT
        case kDxgiR32G32B32A32Sint:  return 108u;  // VK_FORMAT_R32G32B32A32_SINT
        case kDxgiR32G32B32A32Float: return 109u;  // VK_FORMAT_R32G32B32A32_SFLOAT
        case kDxgiR11G11B10Float:    return 122u;  // VK_FORMAT_B10G11R11_UFLOAT_PACK32
        case kDxgiR32G32B32Uint:     return 104u;  // VK_FORMAT_R32G32B32_UINT
        case kDxgiR32G32B32Sint:     return 105u;  // VK_FORMAT_R32G32B32_SINT
        case kDxgiD32Float:          return 126u;  // VK_FORMAT_D32_SFLOAT
        case kDxgiBc1Unorm:          return 133u;  // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
        case kDxgiBc1UnormSrgb:      return 134u;  // VK_FORMAT_BC1_RGBA_SRGB_BLOCK
        case kDxgiBc2Unorm:          return 135u;  // VK_FORMAT_BC2_UNORM_BLOCK
        case kDxgiBc3Unorm:          return 137u;  // VK_FORMAT_BC3_UNORM_BLOCK
        case kDxgiBc4Unorm:          return 139u;  // VK_FORMAT_BC4_UNORM_BLOCK
        case kDxgiBc5Unorm:          return 141u;  // VK_FORMAT_BC5_UNORM_BLOCK
        case kDxgiBc6hUf16:          return 143u;  // VK_FORMAT_BC6H_UFLOAT_BLOCK
        case kDxgiBc7Unorm:          return 145u;  // VK_FORMAT_BC7_UNORM_BLOCK
        case kDxgiBc7UnormSrgb:      return 146u;  // VK_FORMAT_BC7_SRGB_BLOCK
        default:                     return 0u;    // VK_FORMAT_UNDEFINED
    }
}

// ---------------------------------------------------------------------------
// DescriptorSgprBridge Implementation
// ---------------------------------------------------------------------------

DescriptorSgprBridge::DescriptorSgprBridge() = default;

DescriptorSgprBridge::~DescriptorSgprBridge() {
#if defined(_WIN32)
    ShutdownD3D12();
#endif
}

uint64_t DescriptorSgprBridge::HashDescriptorKey(uint8_t tag,
                                                 const uint32_t* words,
                                                 size_t word_count,
                                                 uint64_t resource_key) noexcept {
    // 64-bit FNV-1a over tag, descriptor dwords, and backing resource pointer/VA.
    uint64_t h = 14695981039346656037ull;
    auto mix_byte = [&h](uint8_t b) noexcept {
        h ^= static_cast<uint64_t>(b);
        h *= 1099511628211ull;
    };
    auto mix_u64 = [&](uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            mix_byte(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
        }
    };
    mix_byte(tag);
    mix_u64(resource_key);
    for (size_t i = 0; i < word_count; ++i) {
        const uint32_t w = words[i];
        mix_byte(static_cast<uint8_t>(w & 0xFFu));
        mix_byte(static_cast<uint8_t>((w >> 8) & 0xFFu));
        mix_byte(static_cast<uint8_t>((w >> 16) & 0xFFu));
        mix_byte(static_cast<uint8_t>((w >> 24) & 0xFFu));
    }
    return h;
}

Status DescriptorSgprBridge::SetRootSgpr(uint32_t index, uint32_t value, RootSgprKind kind) {
    if (index >= kRootSgprCount) {
        return Status::Error(StatusCode::InvalidArgument, "Root SGPR index must be in 0..15");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    root_sgprs_[index] = value;
    root_sgpr_kinds_[index] = kind;
    return Status::Ok();
}

Status DescriptorSgprBridge::SetRootSgprRange(uint32_t start_index,
                                              const uint32_t* values,
                                              uint32_t count,
                                              RootSgprKind kind) {
    if (!values || count == 0u || start_index >= kRootSgprCount ||
        start_index + count > kRootSgprCount) {
        return Status::Error(StatusCode::InvalidArgument, "Root SGPR range out of bounds (s0..s15)");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < count; ++i) {
        root_sgprs_[start_index + i] = values[i];
        root_sgpr_kinds_[start_index + i] = kind;
    }
    return Status::Ok();
}

Status DescriptorSgprBridge::SetRootSgprTablePtr(uint32_t start_index, uint64_t title_va) {
    if (start_index + 1u >= kRootSgprCount) {
        return Status::Error(StatusCode::InvalidArgument,
                             "64-bit table pointer requires two consecutive SGPRs in s0..s15");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    root_sgprs_[start_index] = static_cast<uint32_t>(title_va & 0xFFFFFFFFull);
    root_sgprs_[start_index + 1u] = static_cast<uint32_t>((title_va >> 32) & 0xFFFFFFFFull);
    root_sgpr_kinds_[start_index] = RootSgprKind::TablePtrLo;
    root_sgpr_kinds_[start_index + 1u] = RootSgprKind::TablePtrHi;
    return Status::Ok();
}

Status DescriptorSgprBridge::SetRootSgprInlineVSharp(uint32_t start_index,
                                                     const Rdna2BufferDescriptor& vsharp) {
    uint32_t words[4] = {};
    EncodeRdna2BufferDescriptor(vsharp, words);
    return SetRootSgprRange(start_index, words, 4u, RootSgprKind::InlineVSharp);
}

Status DescriptorSgprBridge::SetRootSgprInlineSSharp(uint32_t start_index,
                                                     const Rdna2SamplerDescriptor& ssharp) {
    uint32_t words[4] = {};
    EncodeRdna2SamplerDescriptor(ssharp, words);
    return SetRootSgprRange(start_index, words, 4u, RootSgprKind::InlineSSharp);
}

uint32_t DescriptorSgprBridge::GetRootSgpr(uint32_t index) const noexcept {
    if (index >= kRootSgprCount) return 0u;
    std::lock_guard<std::mutex> lock(mutex_);
    return root_sgprs_[index];
}

uint64_t DescriptorSgprBridge::GetRootSgpr64(uint32_t start_index) const noexcept {
    if (start_index + 1u >= kRootSgprCount) return 0ull;
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint64_t>(root_sgprs_[start_index]) |
           (static_cast<uint64_t>(root_sgprs_[start_index + 1u]) << 32);
}

RootSgprKind DescriptorSgprBridge::GetRootSgprKind(uint32_t index) const noexcept {
    if (index >= kRootSgprCount) return RootSgprKind::Unassigned;
    std::lock_guard<std::mutex> lock(mutex_);
    return root_sgpr_kinds_[index];
}

Status DescriptorSgprBridge::DecodeInlineVSharpFromRootSgpr(
    uint32_t start_index, Rdna2BufferDescriptor& out_vsharp) const {
    if (start_index + 4u > kRootSgprCount) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Inline V# requires 4 consecutive SGPRs in s0..s15");
    }
    uint32_t words[4] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < 4u; ++i) words[i] = root_sgprs_[start_index + i];
    }
    out_vsharp = DecodeRdna2BufferDescriptor(words);
    return Status::Ok();
}

Status DescriptorSgprBridge::DecodeInlineSSharpFromRootSgpr(
    uint32_t start_index, Rdna2SamplerDescriptor& out_ssharp) const {
    if (start_index + 4u > kRootSgprCount) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Inline S# requires 4 consecutive SGPRs in s0..s15");
    }
    uint32_t words[4] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < 4u; ++i) words[i] = root_sgprs_[start_index + i];
    }
    out_ssharp = DecodeRdna2SamplerDescriptor(words);
    return Status::Ok();
}

void DescriptorSgprBridge::ResetRootSgprs() {
    std::lock_guard<std::mutex> lock(mutex_);
    root_sgprs_.fill(0u);
    root_sgpr_kinds_.fill(RootSgprKind::Unassigned);
}

#if defined(_WIN32)

Status DescriptorSgprBridge::InitializeD3D12(ID3D12Device* device,
                                             uint32_t max_cbv_srv_uav_descriptors,
                                             uint32_t max_sampler_descriptors) {
    if (!device || max_cbv_srv_uav_descriptors == 0u || max_sampler_descriptors == 0u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "DescriptorSgprBridge::InitializeD3D12 invalid arguments");
    }

    ShutdownD3D12();

    std::lock_guard<std::mutex> lock(mutex_);

    // Probe highest supported Shader Model (SM 6.6 down to SM 5.1) and Resource Binding Tier.
    static constexpr D3D_SHADER_MODEL kCandidateModels[] = {
        static_cast<D3D_SHADER_MODEL>(0x67),
        static_cast<D3D_SHADER_MODEL>(0x66),
        static_cast<D3D_SHADER_MODEL>(0x65),
        static_cast<D3D_SHADER_MODEL>(0x64),
        static_cast<D3D_SHADER_MODEL>(0x63),
        static_cast<D3D_SHADER_MODEL>(0x62),
        static_cast<D3D_SHADER_MODEL>(0x61),
        static_cast<D3D_SHADER_MODEL>(0x60),
        static_cast<D3D_SHADER_MODEL>(0x51),
    };
    highest_shader_model_ = 0x51u;
    for (D3D_SHADER_MODEL candidate : kCandidateModels) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm_data{};
        sm_data.HighestShaderModel = candidate;
        if (SUCCEEDED(device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL, &sm_data, sizeof(sm_data)))) {
            highest_shader_model_ = static_cast<uint32_t>(sm_data.HighestShaderModel);
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(device->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        resource_binding_tier_ = static_cast<uint32_t>(opts.ResourceBindingTier);
    } else {
        resource_binding_tier_ = static_cast<uint32_t>(D3D12_RESOURCE_BINDING_TIER_1);
    }
    sm66_direct_heap_indexing_ =
        (highest_shader_model_ >= 0x66u) &&
        (resource_binding_tier_ >= static_cast<uint32_t>(D3D12_RESOURCE_BINDING_TIER_3));

    D3D12_DESCRIPTOR_HEAP_DESC res_desc{};
    res_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    res_desc.NumDescriptors = max_cbv_srv_uav_descriptors;
    res_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    res_desc.NodeMask = 0u;

    HRESULT hr = device->CreateDescriptorHeap(
        &res_desc, OS_UUIDOF(ID3D12DescriptorHeap), reinterpret_cast<void**>(&resource_heap_));
    if (FAILED(hr) || !resource_heap_) {
        return Status::Error(StatusCode::DeviceError,
                             "Failed to create shader-visible CBV_SRV_UAV descriptor heap");
    }

    D3D12_DESCRIPTOR_HEAP_DESC samp_desc{};
    samp_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samp_desc.NumDescriptors = std::min<uint32_t>(max_sampler_descriptors, 2048u);
    samp_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    samp_desc.NodeMask = 0u;

    hr = device->CreateDescriptorHeap(
        &samp_desc, OS_UUIDOF(ID3D12DescriptorHeap), reinterpret_cast<void**>(&sampler_heap_));
    if (FAILED(hr) || !sampler_heap_) {
        resource_heap_->Release();
        resource_heap_ = nullptr;
        return Status::Error(StatusCode::DeviceError,
                             "Failed to create shader-visible SAMPLER descriptor heap");
    }

    d3d12_device_ = device;
    d3d12_device_->AddRef();
    resource_increment_ =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sampler_increment_ =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    max_resource_slots_ = max_cbv_srv_uav_descriptors;
    max_sampler_slots_ = samp_desc.NumDescriptors;
    next_resource_slot_ = 0u;
    next_sampler_slot_ = 0u;
    resource_dedup_.clear();
    sampler_dedup_.clear();
    vk_entries_.clear();
    return Status::Ok();
}

void DescriptorSgprBridge::ShutdownD3D12() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampler_heap_) {
        sampler_heap_->Release();
        sampler_heap_ = nullptr;
    }
    if (resource_heap_) {
        resource_heap_->Release();
        resource_heap_ = nullptr;
    }
    if (d3d12_device_) {
        d3d12_device_->Release();
        d3d12_device_ = nullptr;
    }
    resource_increment_ = 0u;
    sampler_increment_ = 0u;
    max_resource_slots_ = 0u;
    max_sampler_slots_ = 0u;
    next_resource_slot_ = 0u;
    next_sampler_slot_ = 0u;
    highest_shader_model_ = 0u;
    resource_binding_tier_ = 0u;
    sm66_direct_heap_indexing_ = false;
    resource_dedup_.clear();
    sampler_dedup_.clear();
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorSgprBridge::GetResourceHeapGpuStart() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resource_heap_) return D3D12_GPU_DESCRIPTOR_HANDLE{0ull};
    return resource_heap_->GetGPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorSgprBridge::GetSamplerHeapGpuStart() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sampler_heap_) return D3D12_GPU_DESCRIPTOR_HANDLE{0ull};
    return sampler_heap_->GetGPUDescriptorHandleForHeapStart();
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorSgprBridge::GetResourceGpuHandle(
    uint32_t bindless_index) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resource_heap_) return D3D12_GPU_DESCRIPTOR_HANDLE{0ull};
    D3D12_GPU_DESCRIPTOR_HANDLE h = resource_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(bindless_index) * static_cast<UINT64>(resource_increment_);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorSgprBridge::GetSamplerGpuHandle(
    uint32_t bindless_index) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sampler_heap_) return D3D12_GPU_DESCRIPTOR_HANDLE{0ull};
    D3D12_GPU_DESCRIPTOR_HANDLE h = sampler_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(bindless_index) * static_cast<UINT64>(sampler_increment_);
    return h;
}

void DescriptorSgprBridge::BindHeapsToCommandList(ID3D12GraphicsCommandList* cmd_list) const {
    if (!cmd_list) return;
    ID3D12DescriptorHeap* heaps[2] = {};
    UINT count = 0u;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resource_heap_) heaps[count++] = resource_heap_;
        if (sampler_heap_) heaps[count++] = sampler_heap_;
    }
    if (count > 0u) {
        cmd_list->SetDescriptorHeaps(count, heaps);
    }
}

bool DescriptorSgprBridge::SynthesizeD3D12BufferSrv(
    const Rdna2BufferDescriptor& vsharp,
    uint64_t resource_offset_bytes,
    D3D12_SHADER_RESOURCE_VIEW_DESC& out_desc) noexcept {
    out_desc = {};
    out_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    out_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        Rdna2DstSelToD3D12Mapping(vsharp.dst_sel_x),
        Rdna2DstSelToD3D12Mapping(vsharp.dst_sel_y),
        Rdna2DstSelToD3D12Mapping(vsharp.dst_sel_z),
        Rdna2DstSelToD3D12Mapping(vsharp.dst_sel_w));

    if (vsharp.stride > 0u) {
        // Structured buffer V#
        out_desc.Format = DXGI_FORMAT_UNKNOWN;
        out_desc.Buffer.FirstElement = resource_offset_bytes / static_cast<uint64_t>(vsharp.stride);
        out_desc.Buffer.NumElements = std::max<uint32_t>(1u, vsharp.num_records);
        out_desc.Buffer.StructureByteStride = vsharp.stride;
        out_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        return true;
    }

    uint32_t dxgi_fmt = kDxgiUnknown;
    bool raw_byte_address = false;
    ResolveVSharpDxgiFormat(vsharp, dxgi_fmt, raw_byte_address);

    if (raw_byte_address) {
        // Raw ByteAddressBuffer V# (num_records is in bytes for raw GFX10 descriptors)
        out_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        out_desc.Buffer.FirstElement = resource_offset_bytes / 4ull;
        out_desc.Buffer.NumElements = std::max<uint32_t>(1u, vsharp.num_records / 4u);
        out_desc.Buffer.StructureByteStride = 0u;
        out_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        return true;
    }

    if (dxgi_fmt == kDxgiUnknown) {
        return false;
    }
    const uint32_t elem_bytes = DxgiFormatElementBytes(dxgi_fmt);
    out_desc.Format = static_cast<DXGI_FORMAT>(dxgi_fmt);
    out_desc.Buffer.FirstElement = resource_offset_bytes / static_cast<uint64_t>(elem_bytes);
    out_desc.Buffer.NumElements = std::max<uint32_t>(
        1u,
        (vsharp.oob_select == Rdna2BufferOobSelect::NumRecordsAddress)
            ? (vsharp.num_records / elem_bytes)
            : vsharp.num_records);
    out_desc.Buffer.StructureByteStride = 0u;
    out_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    return true;
}

bool DescriptorSgprBridge::SynthesizeD3D12BufferUav(
    const Rdna2BufferDescriptor& vsharp,
    uint64_t resource_offset_bytes,
    D3D12_UNORDERED_ACCESS_VIEW_DESC& out_desc) noexcept {
    out_desc = {};
    out_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

    if (vsharp.stride > 0u) {
        out_desc.Format = DXGI_FORMAT_UNKNOWN;
        out_desc.Buffer.FirstElement = resource_offset_bytes / static_cast<uint64_t>(vsharp.stride);
        out_desc.Buffer.NumElements = std::max<uint32_t>(1u, vsharp.num_records);
        out_desc.Buffer.StructureByteStride = vsharp.stride;
        out_desc.Buffer.CounterOffsetInBytes = 0ull;
        out_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        return true;
    }

    uint32_t dxgi_fmt = kDxgiUnknown;
    bool raw_byte_address = false;
    ResolveVSharpDxgiFormat(vsharp, dxgi_fmt, raw_byte_address);

    if (raw_byte_address) {
        out_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        out_desc.Buffer.FirstElement = resource_offset_bytes / 4ull;
        out_desc.Buffer.NumElements = std::max<uint32_t>(1u, vsharp.num_records / 4u);
        out_desc.Buffer.StructureByteStride = 0u;
        out_desc.Buffer.CounterOffsetInBytes = 0ull;
        out_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        return true;
    }

    if (dxgi_fmt == kDxgiUnknown) {
        return false;
    }
    const uint32_t elem_bytes = DxgiFormatElementBytes(dxgi_fmt);
    out_desc.Format = static_cast<DXGI_FORMAT>(dxgi_fmt);
    out_desc.Buffer.FirstElement = resource_offset_bytes / static_cast<uint64_t>(elem_bytes);
    out_desc.Buffer.NumElements = std::max<uint32_t>(
        1u,
        (vsharp.oob_select == Rdna2BufferOobSelect::NumRecordsAddress)
            ? (vsharp.num_records / elem_bytes)
            : vsharp.num_records);
    out_desc.Buffer.StructureByteStride = 0u;
    out_desc.Buffer.CounterOffsetInBytes = 0ull;
    out_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    return true;
}

bool DescriptorSgprBridge::SynthesizeD3D12TextureSrv(
    const Rdna2ImageDescriptor& tsharp,
    D3D12_SHADER_RESOURCE_VIEW_DESC& out_desc) noexcept {
    uint32_t dxgi_fmt = Rdna2ImageFormatToDxgi(tsharp.format);
    if (dxgi_fmt == kDxgiUnknown) return false;
    if (dxgi_fmt == kDxgiD32Float) {
        // Depth textures are sampled as R32_FLOAT in D3D12 SRVs.
        dxgi_fmt = kDxgiR32Float;
    }

    out_desc = {};
    out_desc.Format = static_cast<DXGI_FORMAT>(dxgi_fmt);
    out_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        Rdna2DstSelToD3D12Mapping(tsharp.dst_sel_x),
        Rdna2DstSelToD3D12Mapping(tsharp.dst_sel_y),
        Rdna2DstSelToD3D12Mapping(tsharp.dst_sel_z),
        Rdna2DstSelToD3D12Mapping(tsharp.dst_sel_w));

    const uint32_t mip_count =
        (tsharp.last_level >= tsharp.base_level)
            ? static_cast<uint32_t>(tsharp.last_level - tsharp.base_level + 1u)
            : 1u;
    const uint32_t array_count =
        (tsharp.last_array >= tsharp.base_array)
            ? static_cast<uint32_t>(tsharp.last_array - tsharp.base_array + 1u)
            : std::max<uint32_t>(1u, tsharp.depth);
    const float min_lod = Fixed4_8ToFloat(tsharp.min_lod_raw);

    switch (tsharp.type) {
        case Rdna2ImageType::Img1D:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            out_desc.Texture1D.MostDetailedMip = tsharp.base_level;
            out_desc.Texture1D.MipLevels = mip_count;
            out_desc.Texture1D.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::Img2D:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            out_desc.Texture2D.MostDetailedMip = tsharp.base_level;
            out_desc.Texture2D.MipLevels = mip_count;
            out_desc.Texture2D.PlaneSlice = 0u;
            out_desc.Texture2D.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::Img3D:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            out_desc.Texture3D.MostDetailedMip = tsharp.base_level;
            out_desc.Texture3D.MipLevels = mip_count;
            out_desc.Texture3D.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::ImgCube:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            out_desc.TextureCube.MostDetailedMip = tsharp.base_level;
            out_desc.TextureCube.MipLevels = mip_count;
            out_desc.TextureCube.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::Img1DArray:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            out_desc.Texture1DArray.MostDetailedMip = tsharp.base_level;
            out_desc.Texture1DArray.MipLevels = mip_count;
            out_desc.Texture1DArray.FirstArraySlice = tsharp.base_array;
            out_desc.Texture1DArray.ArraySize = array_count;
            out_desc.Texture1DArray.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::Img2DArray:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            out_desc.Texture2DArray.MostDetailedMip = tsharp.base_level;
            out_desc.Texture2DArray.MipLevels = mip_count;
            out_desc.Texture2DArray.FirstArraySlice = tsharp.base_array;
            out_desc.Texture2DArray.ArraySize = array_count;
            out_desc.Texture2DArray.PlaneSlice = 0u;
            out_desc.Texture2DArray.ResourceMinLODClamp = min_lod;
            return true;
        case Rdna2ImageType::Img2DMsaa:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            return true;
        case Rdna2ImageType::Img2DArrayMsaa:
            out_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            out_desc.Texture2DMSArray.FirstArraySlice = tsharp.base_array;
            out_desc.Texture2DMSArray.ArraySize = array_count;
            return true;
        default:
            return false;
    }
}

bool DescriptorSgprBridge::SynthesizeD3D12TextureUav(
    const Rdna2ImageDescriptor& tsharp,
    D3D12_UNORDERED_ACCESS_VIEW_DESC& out_desc) noexcept {
    const uint32_t dxgi_fmt = Rdna2ImageFormatToDxgi(tsharp.format);
    if (dxgi_fmt == kDxgiUnknown) return false;

    out_desc = {};
    out_desc.Format = static_cast<DXGI_FORMAT>(dxgi_fmt);
    const uint32_t array_count =
        (tsharp.last_array >= tsharp.base_array)
            ? static_cast<uint32_t>(tsharp.last_array - tsharp.base_array + 1u)
            : std::max<uint32_t>(1u, tsharp.depth);

    switch (tsharp.type) {
        case Rdna2ImageType::Img1D:
            out_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            out_desc.Texture1D.MipSlice = tsharp.base_level;
            return true;
        case Rdna2ImageType::Img2D:
            out_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            out_desc.Texture2D.MipSlice = tsharp.base_level;
            out_desc.Texture2D.PlaneSlice = 0u;
            return true;
        case Rdna2ImageType::Img3D:
            out_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            out_desc.Texture3D.MipSlice = tsharp.base_level;
            out_desc.Texture3D.FirstWSlice = tsharp.base_array;
            out_desc.Texture3D.WSize = std::max<uint32_t>(1u, tsharp.depth);
            return true;
        case Rdna2ImageType::Img1DArray:
            out_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            out_desc.Texture1DArray.MipSlice = tsharp.base_level;
            out_desc.Texture1DArray.FirstArraySlice = tsharp.base_array;
            out_desc.Texture1DArray.ArraySize = array_count;
            return true;
        case Rdna2ImageType::Img2DArray:
        case Rdna2ImageType::ImgCube:
            out_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            out_desc.Texture2DArray.MipSlice = tsharp.base_level;
            out_desc.Texture2DArray.FirstArraySlice = tsharp.base_array;
            out_desc.Texture2DArray.ArraySize = array_count;
            out_desc.Texture2DArray.PlaneSlice = 0u;
            return true;
        default:
            return false;
    }
}

bool DescriptorSgprBridge::SynthesizeD3D12Sampler(
    const Rdna2SamplerDescriptor& ssharp,
    D3D12_SAMPLER_DESC& out_desc) noexcept {
    out_desc = {};
    out_desc.Filter = SynthesizeD3D12Filter(ssharp);
    out_desc.AddressU = Rdna2ClampToD3D12AddressMode(ssharp.clamp_x);
    out_desc.AddressV = Rdna2ClampToD3D12AddressMode(ssharp.clamp_y);
    out_desc.AddressW = Rdna2ClampToD3D12AddressMode(ssharp.clamp_z);
    out_desc.MipLODBias = Fixed6_8ToFloat(ssharp.lod_bias_raw);
    out_desc.MaxAnisotropy = MaxAnisoRatioToCount(ssharp.max_aniso_ratio);
    out_desc.ComparisonFunc = Rdna2CompareToD3D12(ssharp.depth_compare_func);
    out_desc.MinLOD = Fixed4_8ToFloat(ssharp.min_lod_raw);
    out_desc.MaxLOD = Fixed4_8ToFloat(ssharp.max_lod_raw);
    if (out_desc.MaxLOD < out_desc.MinLOD) {
        out_desc.MaxLOD = out_desc.MinLOD;
    }
    switch (ssharp.border_color_type) {
        case Rdna2BorderColorType::TransparentBlack:
            out_desc.BorderColor[0] = 0.0f;
            out_desc.BorderColor[1] = 0.0f;
            out_desc.BorderColor[2] = 0.0f;
            out_desc.BorderColor[3] = 0.0f;
            break;
        case Rdna2BorderColorType::OpaqueBlack:
            out_desc.BorderColor[0] = 0.0f;
            out_desc.BorderColor[1] = 0.0f;
            out_desc.BorderColor[2] = 0.0f;
            out_desc.BorderColor[3] = 1.0f;
            break;
        case Rdna2BorderColorType::OpaqueWhite:
            out_desc.BorderColor[0] = 1.0f;
            out_desc.BorderColor[1] = 1.0f;
            out_desc.BorderColor[2] = 1.0f;
            out_desc.BorderColor[3] = 1.0f;
            break;
        case Rdna2BorderColorType::CustomRegister:
        default:
            out_desc.BorderColor[0] = 0.0f;
            out_desc.BorderColor[1] = 0.0f;
            out_desc.BorderColor[2] = 0.0f;
            out_desc.BorderColor[3] = 1.0f;
            break;
    }
    return true;
}

Status DescriptorSgprBridge::BindVSharpSrv(const Rdna2BufferDescriptor& vsharp,
                                           const TitleVAMirror* mirror,
                                           ID3D12Resource* explicit_resource,
                                           uint32_t& out_bindless_index) {
    ID3D12Resource* resource = explicit_resource;
    uint64_t offset_bytes = 0ull;
    uint64_t resolved_va = 0ull;
    if (!resource && mirror && vsharp.base_address != 0ull) {
        TitleVaRegionInfo info{};
        if (mirror->LookupRegion(vsharp.base_address, info)) {
            resource = info.gpu_shadow_resource;
            offset_bytes = vsharp.base_address - info.title_va;
            resolved_va = info.pc_gpu_va ? (info.pc_gpu_va + offset_bytes) : 0ull;
        }
    } else if (resource) {
        const D3D12_RESOURCE_DESC rdesc = resource->GetDesc();
        if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            resolved_va = resource->GetGPUVirtualAddress();
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    if (!SynthesizeD3D12BufferSrv(vsharp, offset_bytes, srv_desc)) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported RDNA2 V# buffer format for D3D12 SRV");
    }

    uint32_t words[4] = {};
    EncodeRdna2BufferDescriptor(vsharp, words);
    const uint64_t key = HashDescriptorKey(
        1u, words, 4u, reinterpret_cast<uint64_t>(resource) ^ (offset_bytes << 1));

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resource_dedup_.find(key);
    if (it != resource_dedup_.end()) {
        ++dedup_hits_;
        out_bindless_index = it->second;
        return Status::Ok();
    }

    if (d3d12_device_ && resource_heap_) {
        if (next_resource_slot_ >= max_resource_slots_) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "Bindless CBV_SRV_UAV descriptor heap exhausted");
        }
        const uint32_t slot = next_resource_slot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = resource_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(resource_increment_);
        d3d12_device_->CreateShaderResourceView(resource, &srv_desc, cpu_h);
        resource_dedup_.emplace(key, slot);
        ++vsharp_bindings_;
        vk_entries_.push_back(TranslateVSharpToVulkan(vsharp, slot, resolved_va, 0u));
        out_bindless_index = slot;
        return Status::Ok();
    }

    // Headless / Vulkan-only metadata path (no D3D12 device attached).
    const uint32_t slot = static_cast<uint32_t>(resource_dedup_.size());
    resource_dedup_.emplace(key, slot);
    ++vsharp_bindings_;
    vk_entries_.push_back(TranslateVSharpToVulkan(vsharp, slot, resolved_va, 0u));
    out_bindless_index = slot;
    return Status::Ok();
}

Status DescriptorSgprBridge::BindVSharpUav(const Rdna2BufferDescriptor& vsharp,
                                           const TitleVAMirror* mirror,
                                           ID3D12Resource* explicit_resource,
                                           uint32_t& out_bindless_index) {
    ID3D12Resource* resource = explicit_resource;
    uint64_t offset_bytes = 0ull;
    uint64_t resolved_va = 0ull;
    if (!resource && mirror && vsharp.base_address != 0ull) {
        TitleVaRegionInfo info{};
        if (mirror->LookupRegion(vsharp.base_address, info)) {
            resource = info.gpu_shadow_resource;
            offset_bytes = vsharp.base_address - info.title_va;
            resolved_va = info.pc_gpu_va ? (info.pc_gpu_va + offset_bytes) : 0ull;
        }
    } else if (resource) {
        const D3D12_RESOURCE_DESC rdesc = resource->GetDesc();
        if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            resolved_va = resource->GetGPUVirtualAddress();
        }
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    if (!SynthesizeD3D12BufferUav(vsharp, offset_bytes, uav_desc)) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported RDNA2 V# buffer format for D3D12 UAV");
    }

    uint32_t words[4] = {};
    EncodeRdna2BufferDescriptor(vsharp, words);
    const uint64_t key = HashDescriptorKey(
        2u, words, 4u, reinterpret_cast<uint64_t>(resource) ^ (offset_bytes << 1));

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resource_dedup_.find(key);
    if (it != resource_dedup_.end()) {
        ++dedup_hits_;
        out_bindless_index = it->second;
        return Status::Ok();
    }

    if (d3d12_device_ && resource_heap_) {
        if (next_resource_slot_ >= max_resource_slots_) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "Bindless CBV_SRV_UAV descriptor heap exhausted");
        }
        const uint32_t slot = next_resource_slot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = resource_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(resource_increment_);
        d3d12_device_->CreateUnorderedAccessView(resource, nullptr, &uav_desc, cpu_h);
        resource_dedup_.emplace(key, slot);
        ++vsharp_bindings_;
        vk_entries_.push_back(TranslateVSharpToVulkan(vsharp, slot, resolved_va, 0u));
        out_bindless_index = slot;
        return Status::Ok();
    }

    const uint32_t slot = static_cast<uint32_t>(resource_dedup_.size());
    resource_dedup_.emplace(key, slot);
    ++vsharp_bindings_;
    vk_entries_.push_back(TranslateVSharpToVulkan(vsharp, slot, resolved_va, 0u));
    out_bindless_index = slot;
    return Status::Ok();
}

Status DescriptorSgprBridge::BindTSharpSrv(const Rdna2ImageDescriptor& tsharp,
                                           const TitleVAMirror* mirror,
                                           ID3D12Resource* explicit_resource,
                                           uint32_t& out_bindless_index) {
    ID3D12Resource* resource = explicit_resource;
    uint64_t resolved_va = 0ull;
    if (!resource && mirror && tsharp.base_address != 0ull) {
        TitleVaRegionInfo info{};
        if (mirror->LookupRegion(tsharp.base_address, info)) {
            resource = info.gpu_shadow_resource;
            resolved_va = info.pc_gpu_va;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    if (!SynthesizeD3D12TextureSrv(tsharp, srv_desc)) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported RDNA2 T# image format/type for D3D12 SRV");
    }

    uint32_t words[8] = {};
    EncodeRdna2ImageDescriptor(tsharp, words);
    const uint64_t key = HashDescriptorKey(3u, words, 8u, reinterpret_cast<uint64_t>(resource));

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resource_dedup_.find(key);
    if (it != resource_dedup_.end()) {
        ++dedup_hits_;
        out_bindless_index = it->second;
        return Status::Ok();
    }

    if (d3d12_device_ && resource_heap_) {
        if (next_resource_slot_ >= max_resource_slots_) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "Bindless CBV_SRV_UAV descriptor heap exhausted");
        }
        const uint32_t slot = next_resource_slot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = resource_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(resource_increment_);
        d3d12_device_->CreateShaderResourceView(resource, &srv_desc, cpu_h);
        resource_dedup_.emplace(key, slot);
        ++tsharp_bindings_;
        vk_entries_.push_back(TranslateTSharpToVulkan(tsharp, slot, false, resolved_va, 1u));
        out_bindless_index = slot;
        return Status::Ok();
    }

    const uint32_t slot = static_cast<uint32_t>(resource_dedup_.size());
    resource_dedup_.emplace(key, slot);
    ++tsharp_bindings_;
    vk_entries_.push_back(TranslateTSharpToVulkan(tsharp, slot, false, resolved_va, 1u));
    out_bindless_index = slot;
    return Status::Ok();
}

Status DescriptorSgprBridge::BindTSharpUav(const Rdna2ImageDescriptor& tsharp,
                                           const TitleVAMirror* mirror,
                                           ID3D12Resource* explicit_resource,
                                           uint32_t& out_bindless_index) {
    ID3D12Resource* resource = explicit_resource;
    uint64_t resolved_va = 0ull;
    if (!resource && mirror && tsharp.base_address != 0ull) {
        TitleVaRegionInfo info{};
        if (mirror->LookupRegion(tsharp.base_address, info)) {
            resource = info.gpu_shadow_resource;
            resolved_va = info.pc_gpu_va;
        }
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    if (!SynthesizeD3D12TextureUav(tsharp, uav_desc)) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Unsupported RDNA2 T# image format/type for D3D12 UAV");
    }

    uint32_t words[8] = {};
    EncodeRdna2ImageDescriptor(tsharp, words);
    const uint64_t key = HashDescriptorKey(4u, words, 8u, reinterpret_cast<uint64_t>(resource));

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resource_dedup_.find(key);
    if (it != resource_dedup_.end()) {
        ++dedup_hits_;
        out_bindless_index = it->second;
        return Status::Ok();
    }

    if (d3d12_device_ && resource_heap_) {
        if (next_resource_slot_ >= max_resource_slots_) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "Bindless CBV_SRV_UAV descriptor heap exhausted");
        }
        const uint32_t slot = next_resource_slot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = resource_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(resource_increment_);
        d3d12_device_->CreateUnorderedAccessView(resource, nullptr, &uav_desc, cpu_h);
        resource_dedup_.emplace(key, slot);
        ++tsharp_bindings_;
        vk_entries_.push_back(TranslateTSharpToVulkan(tsharp, slot, true, resolved_va, 2u));
        out_bindless_index = slot;
        return Status::Ok();
    }

    const uint32_t slot = static_cast<uint32_t>(resource_dedup_.size());
    resource_dedup_.emplace(key, slot);
    ++tsharp_bindings_;
    vk_entries_.push_back(TranslateTSharpToVulkan(tsharp, slot, true, resolved_va, 2u));
    out_bindless_index = slot;
    return Status::Ok();
}

Status DescriptorSgprBridge::BindSSharp(const Rdna2SamplerDescriptor& ssharp,
                                        uint32_t& out_bindless_index) {
    D3D12_SAMPLER_DESC samp_desc{};
    if (!SynthesizeD3D12Sampler(ssharp, samp_desc)) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Failed to synthesize D3D12 sampler from RDNA2 S#");
    }

    uint32_t words[4] = {};
    EncodeRdna2SamplerDescriptor(ssharp, words);
    const uint64_t key = HashDescriptorKey(5u, words, 4u, 0ull);

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sampler_dedup_.find(key);
    if (it != sampler_dedup_.end()) {
        ++dedup_hits_;
        out_bindless_index = it->second;
        return Status::Ok();
    }

    if (d3d12_device_ && sampler_heap_) {
        if (next_sampler_slot_ >= max_sampler_slots_) {
            return Status::Error(StatusCode::OutOfMemory,
                                 "Bindless SAMPLER descriptor heap exhausted");
        }
        const uint32_t slot = next_sampler_slot_++;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += static_cast<SIZE_T>(slot) * static_cast<SIZE_T>(sampler_increment_);
        d3d12_device_->CreateSampler(&samp_desc, cpu_h);
        sampler_dedup_.emplace(key, slot);
        ++ssharp_bindings_;
        vk_entries_.push_back(TranslateSSharpToVulkan(ssharp, slot, 3u));
        out_bindless_index = slot;
        return Status::Ok();
    }

    const uint32_t slot = static_cast<uint32_t>(sampler_dedup_.size());
    sampler_dedup_.emplace(key, slot);
    ++ssharp_bindings_;
    vk_entries_.push_back(TranslateSSharpToVulkan(ssharp, slot, 3u));
    out_bindless_index = slot;
    return Status::Ok();
}

Status DescriptorSgprBridge::ReadTableVSharpFromRootSgpr(
    uint32_t ptr_sgpr_index,
    uint64_t byte_offset,
    const TitleVAMirror& mirror,
    Rdna2BufferDescriptor& out_vsharp) const {
    const uint64_t table_va = GetRootSgpr64(ptr_sgpr_index);
    if (table_va == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Root SGPR descriptor table pointer is null");
    }
    void* cpu_ptr = nullptr;
    uint64_t remaining = 0ull;
    if (!mirror.ResolveTitleVaToCpu(table_va + byte_offset, cpu_ptr, remaining) ||
        remaining < 16ull) {
        return Status::Error(StatusCode::NotFound,
                             "Descriptor table V# address not mapped in TitleVAMirror");
    }
    uint32_t words[4] = {};
    std::memcpy(words, cpu_ptr, sizeof(words));
    out_vsharp = DecodeRdna2BufferDescriptor(words);
    return Status::Ok();
}

Status DescriptorSgprBridge::ReadTableTSharpFromRootSgpr(
    uint32_t ptr_sgpr_index,
    uint64_t byte_offset,
    const TitleVAMirror& mirror,
    Rdna2ImageDescriptor& out_tsharp) const {
    const uint64_t table_va = GetRootSgpr64(ptr_sgpr_index);
    if (table_va == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Root SGPR descriptor table pointer is null");
    }
    void* cpu_ptr = nullptr;
    uint64_t remaining = 0ull;
    if (!mirror.ResolveTitleVaToCpu(table_va + byte_offset, cpu_ptr, remaining) ||
        remaining < 32ull) {
        return Status::Error(StatusCode::NotFound,
                             "Descriptor table T# address not mapped in TitleVAMirror");
    }
    uint32_t words[8] = {};
    std::memcpy(words, cpu_ptr, sizeof(words));
    out_tsharp = DecodeRdna2ImageDescriptor(words);
    return Status::Ok();
}

Status DescriptorSgprBridge::ReadTableSSharpFromRootSgpr(
    uint32_t ptr_sgpr_index,
    uint64_t byte_offset,
    const TitleVAMirror& mirror,
    Rdna2SamplerDescriptor& out_ssharp) const {
    const uint64_t table_va = GetRootSgpr64(ptr_sgpr_index);
    if (table_va == 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Root SGPR descriptor table pointer is null");
    }
    void* cpu_ptr = nullptr;
    uint64_t remaining = 0ull;
    if (!mirror.ResolveTitleVaToCpu(table_va + byte_offset, cpu_ptr, remaining) ||
        remaining < 16ull) {
        return Status::Error(StatusCode::NotFound,
                             "Descriptor table S# address not mapped in TitleVAMirror");
    }
    uint32_t words[4] = {};
    std::memcpy(words, cpu_ptr, sizeof(words));
    out_ssharp = DecodeRdna2SamplerDescriptor(words);
    return Status::Ok();
}

#endif  // defined(_WIN32)

// ---------------------------------------------------------------------------
// Vulkan VK_EXT_descriptor_indexing Translators
// ---------------------------------------------------------------------------

VulkanBindlessBindingEntry DescriptorSgprBridge::TranslateVSharpToVulkan(
    const Rdna2BufferDescriptor& vsharp,
    uint32_t array_index,
    uint64_t resolved_gpu_va,
    uint32_t binding) noexcept {
    VulkanBindlessBindingEntry e{};
    e.set = 0u;
    e.binding = binding;
    e.array_index = array_index;
    e.binding_flags =
        kVkDescriptorBindingUpdateAfterBindBit | kVkDescriptorBindingPartiallyBoundBit;
    e.guest_va = vsharp.base_address;
    e.resolved_gpu_va = resolved_gpu_va;
    e.stride_bytes = vsharp.stride;

    uint32_t dxgi_fmt = kDxgiUnknown;
    bool raw_byte_address = false;
    ResolveVSharpDxgiFormat(vsharp, dxgi_fmt, raw_byte_address);
    e.vk_format = DxgiFormatToVkFormat(dxgi_fmt);

    if (vsharp.stride > 0u) {
        e.vk_descriptor_type = kVkDescriptorTypeStorageBuffer;
        e.byte_range = static_cast<uint64_t>(vsharp.num_records) * static_cast<uint64_t>(vsharp.stride);
    } else if (raw_byte_address) {
        e.vk_descriptor_type = kVkDescriptorTypeStorageBuffer;
        e.byte_range = static_cast<uint64_t>(vsharp.num_records);
    } else {
        e.vk_descriptor_type = kVkDescriptorTypeUniformTexelBuffer;
        const uint32_t elem_bytes = DxgiFormatElementBytes(dxgi_fmt);
        e.byte_range = (vsharp.oob_select == Rdna2BufferOobSelect::NumRecordsAddress)
                           ? static_cast<uint64_t>(vsharp.num_records)
                           : static_cast<uint64_t>(vsharp.num_records) * elem_bytes;
    }
    return e;
}

VulkanBindlessBindingEntry DescriptorSgprBridge::TranslateTSharpToVulkan(
    const Rdna2ImageDescriptor& tsharp,
    uint32_t array_index,
    bool is_storage_image,
    uint64_t resolved_gpu_va,
    uint32_t binding) noexcept {
    VulkanBindlessBindingEntry e{};
    e.set = 0u;
    e.binding = binding;
    e.array_index = array_index;
    e.vk_descriptor_type =
        is_storage_image ? kVkDescriptorTypeStorageImage : kVkDescriptorTypeSampledImage;
    e.vk_format = DxgiFormatToVkFormat(Rdna2ImageFormatToDxgi(tsharp.format));
    e.binding_flags =
        kVkDescriptorBindingUpdateAfterBindBit | kVkDescriptorBindingPartiallyBoundBit;
    e.guest_va = tsharp.base_address;
    e.resolved_gpu_va = resolved_gpu_va;
    e.base_mip = tsharp.base_level;
    e.mip_count = (tsharp.last_level >= tsharp.base_level)
                      ? static_cast<uint32_t>(tsharp.last_level - tsharp.base_level + 1u)
                      : 1u;
    e.base_array_layer = tsharp.base_array;
    e.layer_count = (tsharp.last_array >= tsharp.base_array)
                        ? static_cast<uint32_t>(tsharp.last_array - tsharp.base_array + 1u)
                        : std::max<uint32_t>(1u, tsharp.depth);

    switch (tsharp.type) {
        case Rdna2ImageType::Img1D:          e.vk_image_view_type = kVkImageViewType1D; break;
        case Rdna2ImageType::Img2D:
        case Rdna2ImageType::Img2DMsaa:      e.vk_image_view_type = kVkImageViewType2D; break;
        case Rdna2ImageType::Img3D:          e.vk_image_view_type = kVkImageViewType3D; break;
        case Rdna2ImageType::ImgCube:        e.vk_image_view_type = kVkImageViewTypeCube; break;
        case Rdna2ImageType::Img1DArray:     e.vk_image_view_type = kVkImageViewType1DArray; break;
        case Rdna2ImageType::Img2DArray:
        case Rdna2ImageType::Img2DArrayMsaa: e.vk_image_view_type = kVkImageViewType2DArray; break;
        default:                             e.vk_image_view_type = kVkImageViewType2D; break;
    }
    return e;
}

VulkanBindlessBindingEntry DescriptorSgprBridge::TranslateSSharpToVulkan(
    const Rdna2SamplerDescriptor& ssharp,
    uint32_t array_index,
    uint32_t binding) noexcept {
    VulkanBindlessBindingEntry e{};
    e.set = 0u;
    e.binding = binding;
    e.array_index = array_index;
    e.vk_descriptor_type = kVkDescriptorTypeSampler;
    e.binding_flags =
        kVkDescriptorBindingUpdateAfterBindBit | kVkDescriptorBindingPartiallyBoundBit;
    e.vk_mag_filter = (ssharp.xy_mag_filter == Rdna2XyFilter::Point) ? 0u : 1u;
    e.vk_min_filter = (ssharp.xy_min_filter == Rdna2XyFilter::Point) ? 0u : 1u;
    e.vk_mipmap_mode = (ssharp.mip_filter == Rdna2MipFilter::Linear) ? 1u : 0u;
    e.vk_address_u = Rdna2ClampToVkAddressMode(ssharp.clamp_x);
    e.vk_address_v = Rdna2ClampToVkAddressMode(ssharp.clamp_y);
    e.vk_address_w = Rdna2ClampToVkAddressMode(ssharp.clamp_z);
    e.max_anisotropy = MaxAnisoRatioToCount(ssharp.max_aniso_ratio);
    e.compare_enable = (ssharp.depth_compare_func != Rdna2CompareFunc::Never);
    e.vk_compare_op = static_cast<uint32_t>(ssharp.depth_compare_func) & 0x7u;
    e.min_lod = Fixed4_8ToFloat(ssharp.min_lod_raw);
    e.max_lod = Fixed4_8ToFloat(ssharp.max_lod_raw);
    e.mip_lod_bias = Fixed6_8ToFloat(ssharp.lod_bias_raw);
    return e;
}

VulkanBindlessLayoutTable DescriptorSgprBridge::GetVulkanBindlessTable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VulkanBindlessLayoutTable table{};
    table.update_after_bind = true;
    table.partially_bound = true;
    table.storage_buffer_binding = 0u;
    table.sampled_image_binding = 1u;
    table.storage_image_binding = 2u;
    table.sampler_binding = 3u;
    table.entries = vk_entries_;
    return table;
}

void DescriptorSgprBridge::ResetBindings() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    next_resource_slot_ = 0u;
    next_sampler_slot_ = 0u;
#endif
    resource_dedup_.clear();
    sampler_dedup_.clear();
    vk_entries_.clear();
    dedup_hits_ = 0ull;
    vsharp_bindings_ = 0ull;
    tsharp_bindings_ = 0ull;
    ssharp_bindings_ = 0ull;
}

DescriptorBridgeStats DescriptorSgprBridge::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DescriptorBridgeStats s{};
#if defined(_WIN32)
    s.allocated_resource_slots = d3d12_device_ ? next_resource_slot_
                                               : static_cast<uint32_t>(resource_dedup_.size());
    s.allocated_sampler_slots = d3d12_device_ ? next_sampler_slot_
                                              : static_cast<uint32_t>(sampler_dedup_.size());
    s.max_resource_slots = max_resource_slots_;
    s.max_sampler_slots = max_sampler_slots_;
    s.highest_shader_model = highest_shader_model_;
    s.resource_binding_tier = resource_binding_tier_;
    s.sm66_direct_heap_indexing = sm66_direct_heap_indexing_;
#else
    s.allocated_resource_slots = static_cast<uint32_t>(resource_dedup_.size());
    s.allocated_sampler_slots = static_cast<uint32_t>(sampler_dedup_.size());
#endif
    s.deduplication_hits = dedup_hits_;
    s.vsharp_bindings = vsharp_bindings_;
    s.tsharp_bindings = tsharp_bindings_;
    s.ssharp_bindings = ssharp_bindings_;
    return s;
}

}  // namespace openscarlett
