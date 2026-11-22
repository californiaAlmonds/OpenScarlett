#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
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
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/kernel/title_va_mirror.h"

namespace openscarlett {

// ---------------------------------------------------------------------------
// AMD RDNA2 / GFX10.3 Hardware Descriptor Models (V# / T# / S#)
// Based on the AMD RDNA2 Instruction Set Architecture Reference Guide.
// ---------------------------------------------------------------------------

// GFX10.3 channel destination selector (3 bits: DST_SEL_X/Y/Z/W)
enum class Rdna2DstSel : uint8_t {
    Zero = 0,
    One = 1,
    R = 4,
    G = 5,
    B = 6,
    A = 7,
};

// GFX10.3 V# out-of-bounds check select (2 bits: OOB_SELECT)
enum class Rdna2BufferOobSelect : uint8_t {
    StructuredWithIndex = 0,
    StructuredNoIndex = 1,
    NumRecordsIndex = 2,
    NumRecordsAddress = 3,
};

// GFX10.3 T# image resource type (4 bits: SQ_RSRC_IMG_*)
enum class Rdna2ImageType : uint8_t {
    Img1D = 8,
    Img2D = 9,
    Img3D = 10,
    ImgCube = 11,
    Img1DArray = 12,
    Img2DArray = 13,
    Img2DMsaa = 14,
    Img2DArrayMsaa = 15,
};

// GFX10.3 T# 5-bit swizzle modes (subset of public GFX10 SW_MODE encodings)
enum class Rdna2SwizzleMode : uint8_t {
    Linear = 0,
    Sw256B_S = 1,
    Sw256B_D = 2,
    Sw256B_R = 3,
    Sw4KB_S = 5,
    Sw4KB_D = 6,
    Sw4KB_R = 7,
    Sw64KB_S = 9,
    Sw64KB_D = 10,
    Sw64KB_R = 11,
    Sw64KB_S_T = 17,
    Sw64KB_D_T = 18,
    Sw64KB_R_T = 19,
    Sw4KB_S_X = 21,
    Sw4KB_D_X = 22,
    Sw4KB_R_X = 23,
    Sw64KB_S_X = 25,
    Sw64KB_D_X = 26,
    Sw64KB_R_X = 27,
    Sw64KB_Z_X = 24,
};

// GFX10.3 9-bit unified image formats (subset matching common Xbox DXGI formats)
enum class Rdna2ImageFormat : uint16_t {
    Invalid = 0,
    R8_UNORM = 1,
    R8_SNORM = 2,
    R8_UINT = 3,
    R8_SINT = 4,
    R16_UNORM = 5,
    R16_SNORM = 6,
    R16_UINT = 7,
    R16_SINT = 8,
    R16_FLOAT = 9,
    R8G8_UNORM = 10,
    R8G8_SNORM = 11,
    R8G8_UINT = 12,
    R8G8_SINT = 13,
    R32_UINT = 14,
    R32_SINT = 15,
    R32_FLOAT = 16,
    R16G16_UNORM = 17,
    R16G16_SNORM = 18,
    R16G16_UINT = 19,
    R16G16_SINT = 20,
    R16G16_FLOAT = 21,
    R10G10B10A2_UNORM = 24,
    R10G10B10A2_UINT = 25,
    R11G11B10_FLOAT = 26,
    R8G8B8A8_UNORM = 28,
    R8G8B8A8_SNORM = 29,
    R8G8B8A8_UINT = 30,
    R8G8B8A8_SINT = 31,
    R8G8B8A8_SRGB = 32,
    B8G8R8A8_UNORM = 33,
    B8G8R8A8_SRGB = 34,
    R32G32_UINT = 44,
    R32G32_SINT = 45,
    R32G32_FLOAT = 46,
    R16G16B16A16_UNORM = 47,
    R16G16B16A16_SNORM = 48,
    R16G16B16A16_UINT = 49,
    R16G16B16A16_SINT = 50,
    R16G16B16A16_FLOAT = 51,
    R32G32B32A32_UINT = 72,
    R32G32B32A32_SINT = 73,
    R32G32B32A32_FLOAT = 74,
    BC1_UNORM = 80,
    BC1_SRGB = 81,
    BC2_UNORM = 82,
    BC3_UNORM = 84,
    BC4_UNORM = 86,
    BC5_UNORM = 88,
    BC6H_UF16 = 90,
    BC7_UNORM = 92,
    BC7_SRGB = 93,
    D32_FLOAT = 96,
};

// GFX10.3 S# clamp / address mode (3 bits)
enum class Rdna2ClampMode : uint8_t {
    Wrap = 0,
    Mirror = 1,
    ClampLastTexel = 2,
    MirrorOnceLastTexel = 3,
    ClampHalfBorder = 4,
    MirrorOnceHalfBorder = 5,
    ClampBorder = 6,
    MirrorOnceBorder = 7,
};

// GFX10.3 S# depth compare function (3 bits)
enum class Rdna2CompareFunc : uint8_t {
    Never = 0,
    Less = 1,
    Equal = 2,
    LessEqual = 3,
    Greater = 4,
    NotEqual = 5,
    GreaterEqual = 6,
    Always = 7,
};

// GFX10.3 S# XY filter mode (2 bits)
enum class Rdna2XyFilter : uint8_t {
    Point = 0,
    Bilinear = 1,
    AnisoPoint = 2,
    AnisoLinear = 3,
};

// GFX10.3 S# Z / mip filter mode (2 bits)
enum class Rdna2MipFilter : uint8_t {
    None = 0,
    Point = 1,
    Linear = 2,
};

// GFX10.3 S# border color preset (2 bits)
enum class Rdna2BorderColorType : uint8_t {
    TransparentBlack = 0,
    OpaqueBlack = 1,
    OpaqueWhite = 2,
    CustomRegister = 3,
};

// 4-dword GFX10.3 Buffer Resource Descriptor (V#).
struct Rdna2BufferDescriptor {
    uint64_t base_address = 0;  // 48-bit virtual address
    uint16_t stride = 0;        // 14-bit element stride in bytes (0 = raw/untyped)
    bool swizzle_enable = false;
    bool cache_swizzle = false;
    uint32_t num_records = 0;   // 32-bit record count (or byte count when stride == 0)
    Rdna2DstSel dst_sel_x = Rdna2DstSel::R;
    Rdna2DstSel dst_sel_y = Rdna2DstSel::G;
    Rdna2DstSel dst_sel_z = Rdna2DstSel::B;
    Rdna2DstSel dst_sel_w = Rdna2DstSel::A;
    uint8_t format = static_cast<uint8_t>(Rdna2MtbufFormat::R32_UINT);  // 7-bit GFX10 format
    uint8_t index_stride = 0;   // 2-bit swizzle index stride
    bool add_tid_enable = false;
    bool resource_level = true;
    Rdna2BufferOobSelect oob_select = Rdna2BufferOobSelect::NumRecordsAddress;
    uint8_t type = 0;           // 2-bit resource type (0 = buffer)
};

// 8-dword GFX10.3 Image Resource Descriptor (T#).
struct Rdna2ImageDescriptor {
    uint64_t base_address = 0;  // 48-bit byte address (256-byte aligned; 40-bit encoded)
    uint16_t min_lod_raw = 0;   // 12-bit 4.8 unsigned fixed point (min_lod = min_lod_raw / 256.0f)
    uint16_t format = static_cast<uint16_t>(Rdna2ImageFormat::R8G8B8A8_UNORM);  // 9-bit GFX10 format
    bool iterate_256 = false;
    uint32_t width = 1;         // 1..16384 (encoded as width - 1 in 14 bits)
    uint32_t height = 1;        // 1..16384 (encoded as height - 1 in 14 bits)
    uint8_t perf_mod = 0;       // 3-bit perf modifier
    bool resource_level = true;
    Rdna2DstSel dst_sel_x = Rdna2DstSel::R;
    Rdna2DstSel dst_sel_y = Rdna2DstSel::G;
    Rdna2DstSel dst_sel_z = Rdna2DstSel::B;
    Rdna2DstSel dst_sel_w = Rdna2DstSel::A;
    uint8_t base_level = 0;     // 4 bits (0..15)
    uint8_t last_level = 0;     // 4 bits (0..15)
    Rdna2SwizzleMode swizzle_mode = Rdna2SwizzleMode::Linear;  // 5 bits
    uint8_t bc_swizzle = 0;     // 3 bits
    Rdna2ImageType type = Rdna2ImageType::Img2D;               // 4 bits
    uint32_t depth = 1;         // 1..8192 (encoded as depth - 1 in 13 bits)
    uint16_t base_array = 0;    // 13 bits (0..8191)
    uint16_t last_array = 0;    // 13 bits (0..8191)
    uint8_t max_mip = 0;        // 4 bits
    uint8_t array_pitch = 0;    // 4 bits

    // Metadata / DCC fields (words 6 & 7)
    bool meta_pipe_aligned = false;
    bool dcc_compression_enable = false;
    bool alpha_is_on_msb = false;
    bool color_transform = false;
    bool meta_linear = false;
    bool write_compress_enable = false;
    uint8_t max_uncompressed_block_size = 0;  // 2 bits
    uint8_t max_compressed_block_size = 0;    // 2 bits
    uint64_t meta_data_address = 0;           // 48-bit byte address (256-byte aligned; 40-bit encoded)
};

// 4-dword GFX10.3 Sampler State Descriptor (S#).
struct Rdna2SamplerDescriptor {
    Rdna2ClampMode clamp_x = Rdna2ClampMode::ClampLastTexel;
    Rdna2ClampMode clamp_y = Rdna2ClampMode::ClampLastTexel;
    Rdna2ClampMode clamp_z = Rdna2ClampMode::ClampLastTexel;
    uint8_t max_aniso_ratio = 0;  // 3 bits: 0=1x, 1=2x, 2=4x, 3=8x, 4=16x
    Rdna2CompareFunc depth_compare_func = Rdna2CompareFunc::Never;
    bool force_unnormalized = false;
    uint8_t aniso_threshold = 0;  // 3 bits
    bool mc_coord_trunc = false;
    bool force_degamma = false;
    uint8_t aniso_bias = 0;       // 6 bits
    bool trunc_coord = false;
    bool disable_cube_wrap = false;
    uint8_t filter_mode = 0;      // 2 bits: 0=Blend, 1=Min, 2=Max
    bool compat_mode = false;

    uint16_t min_lod_raw = 0;     // 12-bit 4.8 unsigned fixed point (0..15.996)
    uint16_t max_lod_raw = 0x0FFFu;  // 12-bit 4.8 unsigned fixed point
    uint8_t perf_mip = 0;         // 4 bits
    uint8_t perf_z = 0;           // 4 bits

    int16_t lod_bias_raw = 0;     // 14-bit signed 6.8 fixed point (-32.0 .. +31.996)
    uint8_t lod_bias_sec = 0;     // 6 bits
    Rdna2XyFilter xy_mag_filter = Rdna2XyFilter::Bilinear;
    Rdna2XyFilter xy_min_filter = Rdna2XyFilter::Bilinear;
    Rdna2MipFilter z_filter = Rdna2MipFilter::Point;
    Rdna2MipFilter mip_filter = Rdna2MipFilter::Linear;
    bool aniso_override = false;
    bool blend_zero_prt = false;

    uint16_t border_color_ptr = 0;  // 12-bit border palette index
    Rdna2BorderColorType border_color_type = Rdna2BorderColorType::TransparentBlack;
};

// Bit-exact GFX10.3 encode/decode helpers for V#, T#, and S#.
void EncodeRdna2BufferDescriptor(const Rdna2BufferDescriptor& desc, uint32_t out_words[4]) noexcept;
Rdna2BufferDescriptor DecodeRdna2BufferDescriptor(const uint32_t words[4]) noexcept;

void EncodeRdna2ImageDescriptor(const Rdna2ImageDescriptor& desc, uint32_t out_words[8]) noexcept;
Rdna2ImageDescriptor DecodeRdna2ImageDescriptor(const uint32_t words[8]) noexcept;

void EncodeRdna2SamplerDescriptor(const Rdna2SamplerDescriptor& desc, uint32_t out_words[4]) noexcept;
Rdna2SamplerDescriptor DecodeRdna2SamplerDescriptor(const uint32_t words[4]) noexcept;

// Fixed-point LOD conversion helpers (4.8 unsigned and 6.8 signed).
inline constexpr uint16_t FloatToFixed4_8(float v) noexcept {
    if (v <= 0.0f) return 0u;
    if (v >= 15.99609375f) return 0x0FFFu;
    return static_cast<uint16_t>(v * 256.0f + 0.5f) & 0x0FFFu;
}
inline constexpr float Fixed4_8ToFloat(uint16_t raw) noexcept {
    return static_cast<float>(raw & 0x0FFFu) / 256.0f;
}
inline constexpr int16_t FloatToFixed6_8(float v) noexcept {
    if (v <= -32.0f) return -8192;
    if (v >= 31.99609375f) return 8191;
    const float scaled = v * 256.0f;
    return static_cast<int16_t>(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
}
inline constexpr float Fixed6_8ToFloat(int16_t raw) noexcept {
    return static_cast<float>(raw) / 256.0f;
}

// ---------------------------------------------------------------------------
// Vulkan VK_EXT_descriptor_indexing Bindless Translation Metadata
// (Khronos numeric constants — zero Vulkan SDK header dependency)
// ---------------------------------------------------------------------------
inline constexpr uint32_t kVkDescriptorTypeSampler = 0u;
inline constexpr uint32_t kVkDescriptorTypeSampledImage = 2u;
inline constexpr uint32_t kVkDescriptorTypeStorageImage = 3u;
inline constexpr uint32_t kVkDescriptorTypeUniformTexelBuffer = 4u;
inline constexpr uint32_t kVkDescriptorTypeStorageTexelBuffer = 5u;
inline constexpr uint32_t kVkDescriptorTypeStorageBuffer = 7u;

inline constexpr uint32_t kVkDescriptorBindingUpdateAfterBindBit = 0x00000001u;
inline constexpr uint32_t kVkDescriptorBindingPartiallyBoundBit = 0x00000004u;
inline constexpr uint32_t kVkDescriptorBindingVariableCountBit = 0x00000008u;

inline constexpr uint32_t kVkImageViewType1D = 0u;
inline constexpr uint32_t kVkImageViewType2D = 1u;
inline constexpr uint32_t kVkImageViewType3D = 2u;
inline constexpr uint32_t kVkImageViewTypeCube = 3u;
inline constexpr uint32_t kVkImageViewType1DArray = 4u;
inline constexpr uint32_t kVkImageViewType2DArray = 5u;

struct VulkanBindlessBindingEntry {
    uint32_t set = 0;
    uint32_t binding = 0;
    uint32_t array_index = 0;
    uint32_t vk_descriptor_type = kVkDescriptorTypeStorageBuffer;
    uint32_t vk_format = 0;           // Khronos VkFormat
    uint32_t vk_image_view_type = kVkImageViewType2D;
    uint32_t binding_flags =
        kVkDescriptorBindingUpdateAfterBindBit | kVkDescriptorBindingPartiallyBoundBit;

    uint64_t guest_va = 0;
    uint64_t resolved_gpu_va = 0;
    uint64_t byte_range = 0;
    uint32_t stride_bytes = 0;
    uint32_t base_mip = 0;
    uint32_t mip_count = 1;
    uint32_t base_array_layer = 0;
    uint32_t layer_count = 1;

    // Sampler state (populated when vk_descriptor_type == kVkDescriptorTypeSampler)
    uint32_t vk_mag_filter = 1;       // 0=NEAREST, 1=LINEAR
    uint32_t vk_min_filter = 1;       // 0=NEAREST, 1=LINEAR
    uint32_t vk_mipmap_mode = 1;      // 0=NEAREST, 1=LINEAR
    uint32_t vk_address_u = 2;        // 0=REPEAT, 1=MIRRORED_REPEAT, 2=CLAMP_TO_EDGE, 3=CLAMP_TO_BORDER, 4=MIRROR_CLAMP_TO_EDGE
    uint32_t vk_address_v = 2;
    uint32_t vk_address_w = 2;
    uint32_t max_anisotropy = 1;
    bool compare_enable = false;
    uint32_t vk_compare_op = 0;       // Khronos VkCompareOp (0..7)
    float min_lod = 0.0f;
    float max_lod = 16.0f;
    float mip_lod_bias = 0.0f;
};

struct VulkanBindlessLayoutTable {
    bool update_after_bind = true;
    bool partially_bound = true;
    uint32_t storage_buffer_binding = 0;
    uint32_t sampled_image_binding = 1;
    uint32_t storage_image_binding = 2;
    uint32_t sampler_binding = 3;
    std::vector<VulkanBindlessBindingEntry> entries;
};

// Format conversion helpers (GFX10.3 <-> DXGI <-> Khronos VkFormat).
uint32_t Rdna2BufferFormatToDxgi(uint8_t fmt) noexcept;
uint8_t DxgiToRdna2BufferFormat(uint32_t dxgi_format) noexcept;
uint32_t Rdna2ImageFormatToDxgi(uint16_t fmt) noexcept;
uint16_t DxgiToRdna2ImageFormat(uint32_t dxgi_format) noexcept;
uint32_t DxgiFormatToVkFormat(uint32_t dxgi_format) noexcept;

// Root SGPR role tag for guest s0..s15 user-data tracking.
enum class RootSgprKind : uint8_t {
    Unassigned = 0,
    InlineConstant32 = 1,
    TablePtrLo = 2,
    TablePtrHi = 3,
    InlineVSharp = 4,
    InlineSSharp = 5,
};

struct DescriptorBridgeStats {
    uint32_t allocated_resource_slots = 0;
    uint32_t allocated_sampler_slots = 0;
    uint32_t max_resource_slots = 0;
    uint32_t max_sampler_slots = 0;
    uint64_t deduplication_hits = 0;
    uint64_t vsharp_bindings = 0;
    uint64_t tsharp_bindings = 0;
    uint64_t ssharp_bindings = 0;
    uint32_t highest_shader_model = 0;
    uint32_t resource_binding_tier = 0;
    bool sm66_direct_heap_indexing = false;
};

// ---------------------------------------------------------------------------
// DescriptorSgprBridge
// Translates guest RDNA2 root SGPRs (s0..s15) and raw V#/T#/S# hardware
// descriptors into:
//   1. PC D3D12 shader-visible bindless CBV_SRV_UAV & SAMPLER descriptor heaps
//      (supporting SM 6.6 ResourceDescriptorHeap/SamplerDescriptorHeap and
//       SM 6.5/6.2/5.1 descriptor-table heap indexing fallback).
//   2. Vulkan VK_EXT_descriptor_indexing bindless layout metadata.
// ---------------------------------------------------------------------------
class DescriptorSgprBridge {
public:
    static constexpr uint32_t kRootSgprCount = 16u;

    DescriptorSgprBridge();
    ~DescriptorSgprBridge();

    DescriptorSgprBridge(const DescriptorSgprBridge&) = delete;
    DescriptorSgprBridge& operator=(const DescriptorSgprBridge&) = delete;

#if defined(_WIN32)
    // Initializes shader-visible D3D12 CBV_SRV_UAV and SAMPLER descriptor heaps
    // and probes SM 6.6 direct heap indexing vs SM 6.5/6.2 root-table fallback.
    Status InitializeD3D12(ID3D12Device* device,
                           uint32_t max_cbv_srv_uav_descriptors = 4096u,
                           uint32_t max_sampler_descriptors = 256u);
    void ShutdownD3D12();

    bool IsD3D12Initialized() const noexcept { return d3d12_device_ != nullptr; }
    bool SupportsSm66DirectHeapIndexing() const noexcept { return sm66_direct_heap_indexing_; }
    uint32_t HighestShaderModel() const noexcept { return highest_shader_model_; }
    uint32_t ResourceBindingTier() const noexcept { return resource_binding_tier_; }

    ID3D12DescriptorHeap* ResourceHeap() const noexcept { return resource_heap_; }
    ID3D12DescriptorHeap* SamplerHeap() const noexcept { return sampler_heap_; }
    uint32_t ResourceDescriptorIncrement() const noexcept { return resource_increment_; }
    uint32_t SamplerDescriptorIncrement() const noexcept { return sampler_increment_; }

    D3D12_GPU_DESCRIPTOR_HANDLE GetResourceHeapGpuStart() const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerHeapGpuStart() const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE GetResourceGpuHandle(uint32_t bindless_index) const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerGpuHandle(uint32_t bindless_index) const noexcept;

    // Calls cmd_list->SetDescriptorHeaps with the active CBV_SRV_UAV and SAMPLER heaps.
    void BindHeapsToCommandList(ID3D12GraphicsCommandList* cmd_list) const;

    // Pure D3D12 view/sampler descriptor synthesizers from decoded RDNA2 V#/T#/S#.
    static bool SynthesizeD3D12BufferSrv(const Rdna2BufferDescriptor& vsharp,
                                         uint64_t resource_offset_bytes,
                                         D3D12_SHADER_RESOURCE_VIEW_DESC& out_desc) noexcept;
    static bool SynthesizeD3D12BufferUav(const Rdna2BufferDescriptor& vsharp,
                                         uint64_t resource_offset_bytes,
                                         D3D12_UNORDERED_ACCESS_VIEW_DESC& out_desc) noexcept;
    static bool SynthesizeD3D12TextureSrv(const Rdna2ImageDescriptor& tsharp,
                                          D3D12_SHADER_RESOURCE_VIEW_DESC& out_desc) noexcept;
    static bool SynthesizeD3D12TextureUav(const Rdna2ImageDescriptor& tsharp,
                                          D3D12_UNORDERED_ACCESS_VIEW_DESC& out_desc) noexcept;
    static bool SynthesizeD3D12Sampler(const Rdna2SamplerDescriptor& ssharp,
                                       D3D12_SAMPLER_DESC& out_desc) noexcept;

    // Decodes/binds raw or structured V#, T#, and S# descriptors into deduplicated
    // bindless slots on the PC D3D12 heaps (and mirrors into the Vulkan bindless table).
    // When `explicit_resource == nullptr`, resolves `base_address` via `mirror`.
    Status BindVSharpSrv(const Rdna2BufferDescriptor& vsharp,
                         const TitleVAMirror* mirror,
                         ID3D12Resource* explicit_resource,
                         uint32_t& out_bindless_index);
    Status BindVSharpUav(const Rdna2BufferDescriptor& vsharp,
                         const TitleVAMirror* mirror,
                         ID3D12Resource* explicit_resource,
                         uint32_t& out_bindless_index);
    Status BindTSharpSrv(const Rdna2ImageDescriptor& tsharp,
                         const TitleVAMirror* mirror,
                         ID3D12Resource* explicit_resource,
                         uint32_t& out_bindless_index);
    Status BindTSharpUav(const Rdna2ImageDescriptor& tsharp,
                         const TitleVAMirror* mirror,
                         ID3D12Resource* explicit_resource,
                         uint32_t& out_bindless_index);
    Status BindSSharp(const Rdna2SamplerDescriptor& ssharp,
                      uint32_t& out_bindless_index);

    // Reads a V# (4 dwords), T# (8 dwords), or S# (4 dwords) from a guest descriptor table
    // pointed to by root SGPR pair `s[ptr_sgpr_index .. ptr_sgpr_index+1]` via `TitleVAMirror`.
    Status ReadTableVSharpFromRootSgpr(uint32_t ptr_sgpr_index,
                                       uint64_t byte_offset,
                                       const TitleVAMirror& mirror,
                                       Rdna2BufferDescriptor& out_vsharp) const;
    Status ReadTableTSharpFromRootSgpr(uint32_t ptr_sgpr_index,
                                       uint64_t byte_offset,
                                       const TitleVAMirror& mirror,
                                       Rdna2ImageDescriptor& out_tsharp) const;
    Status ReadTableSSharpFromRootSgpr(uint32_t ptr_sgpr_index,
                                       uint64_t byte_offset,
                                       const TitleVAMirror& mirror,
                                       Rdna2SamplerDescriptor& out_ssharp) const;
#endif  // defined(_WIN32)

    // Guest root SGPR (s0..s15) state management.
    Status SetRootSgpr(uint32_t index, uint32_t value,
                       RootSgprKind kind = RootSgprKind::InlineConstant32);
    Status SetRootSgprRange(uint32_t start_index,
                            const uint32_t* values,
                            uint32_t count,
                            RootSgprKind kind = RootSgprKind::InlineConstant32);
    Status SetRootSgprTablePtr(uint32_t start_index, uint64_t title_va);
    Status SetRootSgprInlineVSharp(uint32_t start_index, const Rdna2BufferDescriptor& vsharp);
    Status SetRootSgprInlineSSharp(uint32_t start_index, const Rdna2SamplerDescriptor& ssharp);

    uint32_t GetRootSgpr(uint32_t index) const noexcept;
    uint64_t GetRootSgpr64(uint32_t start_index) const noexcept;
    RootSgprKind GetRootSgprKind(uint32_t index) const noexcept;
    Status DecodeInlineVSharpFromRootSgpr(uint32_t start_index,
                                          Rdna2BufferDescriptor& out_vsharp) const;
    Status DecodeInlineSSharpFromRootSgpr(uint32_t start_index,
                                          Rdna2SamplerDescriptor& out_ssharp) const;
    void ResetRootSgprs();

    // Pure Vulkan VK_EXT_descriptor_indexing translators.
    static VulkanBindlessBindingEntry TranslateVSharpToVulkan(
        const Rdna2BufferDescriptor& vsharp,
        uint32_t array_index,
        uint64_t resolved_gpu_va = 0,
        uint32_t binding = 0) noexcept;
    static VulkanBindlessBindingEntry TranslateTSharpToVulkan(
        const Rdna2ImageDescriptor& tsharp,
        uint32_t array_index,
        bool is_storage_image = false,
        uint64_t resolved_gpu_va = 0,
        uint32_t binding = 1) noexcept;
    static VulkanBindlessBindingEntry TranslateSSharpToVulkan(
        const Rdna2SamplerDescriptor& ssharp,
        uint32_t array_index,
        uint32_t binding = 3) noexcept;

    // Returns the accumulated Vulkan VK_EXT_descriptor_indexing layout table for all
    // descriptors bound through this bridge.
    VulkanBindlessLayoutTable GetVulkanBindlessTable() const;

    // Resets all bindless slot allocations and deduplication caches (keeps heaps allocated).
    void ResetBindings();

    DescriptorBridgeStats GetStats() const;

private:
    static uint64_t HashDescriptorKey(uint8_t tag,
                                      const uint32_t* words,
                                      size_t word_count,
                                      uint64_t resource_key) noexcept;

    mutable std::mutex mutex_;
    std::array<uint32_t, kRootSgprCount> root_sgprs_{};
    std::array<RootSgprKind, kRootSgprCount> root_sgpr_kinds_{};

#if defined(_WIN32)
    ID3D12Device* d3d12_device_ = nullptr;
    ID3D12DescriptorHeap* resource_heap_ = nullptr;
    ID3D12DescriptorHeap* sampler_heap_ = nullptr;
    uint32_t resource_increment_ = 0;
    uint32_t sampler_increment_ = 0;
    uint32_t max_resource_slots_ = 0;
    uint32_t max_sampler_slots_ = 0;
    uint32_t next_resource_slot_ = 0;
    uint32_t next_sampler_slot_ = 0;
    uint32_t highest_shader_model_ = 0;
    uint32_t resource_binding_tier_ = 0;
    bool sm66_direct_heap_indexing_ = false;
#endif

    std::unordered_map<uint64_t, uint32_t> resource_dedup_;
    std::unordered_map<uint64_t, uint32_t> sampler_dedup_;
    std::vector<VulkanBindlessBindingEntry> vk_entries_;

    uint64_t dedup_hits_ = 0;
    uint64_t vsharp_bindings_ = 0;
    uint64_t tsharp_bindings_ = 0;
    uint64_t ssharp_bindings_ = 0;
};

}  // namespace openscarlett
