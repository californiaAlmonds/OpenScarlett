#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#include <dxgiformat.h>
#include <windows.h>
#if defined(OPENSCARLETT_BUILDING_XG_XS)
#define OPENSCARLETT_XG_API __declspec(dllexport)
#else
#define OPENSCARLETT_XG_API __declspec(dllimport)
#endif
#else
#define OPENSCARLETT_XG_API
#endif

// Xbox Scarlett texture swizzle / tiling ABI (xg_xs.dll).
// Implements AMD GFX10 / RDNA2 2D thin (all 15 tiled modes + linear),
// 3D thick (all 6 modes), mip chains with GFX10 mip tails, texture arrays,
// cube maps, 3D volumes, BC1-BC7, RGB9E5, and planar depth/stencil formats.

enum XG_TILE_MODE : uint32_t {
    // Preserved ABI values (0..4)
    XG_TILE_MODE_LINEAR = 0u,
    XG_TILE_MODE_2D_THIN_4KB_S = 1u,
    XG_TILE_MODE_2D_THIN_64KB_S = 2u,
    XG_TILE_MODE_2D_THIN_64KB_D = 3u,
    XG_TILE_MODE_2D_THIN_64KB_R_X = 4u,

    // Additional GFX10 2D Thin swizzle modes (total 15 tiled 2D modes)
    XG_TILE_MODE_2D_THIN_256B_S = 5u,
    XG_TILE_MODE_2D_THIN_256B_D = 6u,
    XG_TILE_MODE_2D_THIN_256B_R = 7u,
    XG_TILE_MODE_2D_THIN_4KB_Z = 8u,
    XG_TILE_MODE_2D_THIN_4KB_D = 9u,
    XG_TILE_MODE_2D_THIN_4KB_R = 10u,
    XG_TILE_MODE_2D_THIN_64KB_Z = 11u,
    XG_TILE_MODE_2D_THIN_64KB_R = 12u,
    XG_TILE_MODE_2D_THIN_64KB_Z_X = 13u,
    XG_TILE_MODE_2D_THIN_64KB_S_X = 14u,
    XG_TILE_MODE_2D_THIN_64KB_D_X = 15u,

    // GFX10 3D Thick swizzle modes (all 6 modes)
    XG_TILE_MODE_3D_THICK_4KB_S = 16u,
    XG_TILE_MODE_3D_THICK_64KB_Z = 17u,
    XG_TILE_MODE_3D_THICK_64KB_S = 18u,
    XG_TILE_MODE_3D_THICK_64KB_Z_X = 19u,
    XG_TILE_MODE_3D_THICK_64KB_S_X = 20u,
    XG_TILE_MODE_3D_THICK_64KB_R_X = 21u,
};

enum XG_RESOURCE_DIMENSION : uint32_t {
    XG_RESOURCE_DIMENSION_TEXTURE2D = 3u,
    XG_RESOURCE_DIMENSION_TEXTURE3D = 4u,
};

enum XG_BIND_FLAGS : uint32_t {
    XG_BIND_NONE = 0x0u,
    XG_BIND_SHADER_RESOURCE = 0x8u,
    XG_BIND_RENDER_TARGET = 0x20u,
    XG_BIND_DEPTH_STENCIL = 0x40u,
    XG_BIND_UNORDERED_ACCESS = 0x80u,
    XG_BIND_DISPLAYABLE = 0x100u,
    XG_BIND_CUBE_MAP = 0x200u,
};

enum XG_CONSOLE_TYPE : uint32_t {
    XG_CONSOLE_TYPE_SCARLETT_ANACONDA = 0u,  // Xbox Series X (52 CU, 16 pipes, 2 SE)
    XG_CONSOLE_TYPE_SCARLETT_LOCKHART = 1u,  // Xbox Series S (20 CU, 8 pipes, 1 SE)
};

struct XG_SCARLETT_PIPE_CONFIG {
    XG_CONSOLE_TYPE ConsoleType;
    uint32_t NumPipes;
    uint32_t PipeInterleaveBytes;
    uint32_t NumBanks;
    uint32_t NumShaderEngines;
    uint32_t NumRenderBackends;
    uint32_t PipeBits;
    uint32_t BankBits;
    uint32_t MaxCompressedFragments;
    uint32_t HtileTileSizeBytes;
    uint32_t DccBlockSizeBytes;
    uint32_t CmaskTileWidthPixels;
    uint32_t CmaskTileHeightPixels;
};

struct XG_METADATA_PLANE_INFO {
    uint32_t Enabled;
    uint32_t PipeAligned;
    uint32_t MetaBlockWidth;
    uint32_t MetaBlockHeight;
    uint64_t OffsetBytes;
    uint64_t SizeBytes;
    uint64_t AlignmentBytes;
    uint64_t SlicePitchBytes;
};

struct XG_RESOURCE_METADATA_LAYOUT {
    uint64_t BaseSurfaceSizeBytes;
    uint64_t TotalSizeWithMetadataBytes;
    uint64_t BaseAlignmentBytes;
    XG_SCARLETT_PIPE_CONFIG PipeConfig;
    XG_METADATA_PLANE_INFO Htile;
    XG_METADATA_PLANE_INFO Dcc;
    XG_METADATA_PLANE_INFO Cmask;
};

struct XG_FORMAT_BLOCK_INFO {
    uint32_t dxgi_format;
    uint32_t block_width;
    uint32_t block_height;
    uint32_t bytes_per_block;
    uint32_t is_compressed;
};

struct XG_TEXTURE2D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;          // DXGI_FORMAT
    uint32_t SampleCount;
    uint32_t BindFlags;       // XG_BIND_FLAGS
    XG_TILE_MODE TileMode;
};

// AMD GFX10 / RDNA2 DCC metadata block states & encoding parameters.
enum XG_DCC_BLOCK_STATE : uint32_t {
    XG_DCC_STATE_UNCOMPRESSED = 0u,
    XG_DCC_STATE_CLEAR_0000 = 1u,
    XG_DCC_STATE_CLEAR_0001 = 2u,
    XG_DCC_STATE_CLEAR_1110 = 3u,
    XG_DCC_STATE_CLEAR_1111 = 4u,
    XG_DCC_STATE_CLEAR_REG = 5u,
    XG_DCC_STATE_COMPRESSED_2TO1 = 6u,
    XG_DCC_STATE_COMPRESSED_4TO1 = 7u,
};

struct XG_DCC_ENCODE_PARAMS {
    XG_TEXTURE2D_DESC Desc;
    XG_CONSOLE_TYPE ConsoleType;
    uint32_t MipLevel;
    uint32_t ArraySlice;
    XG_DCC_BLOCK_STATE BlockState;
    uint32_t Independent64BBlocks;
    uint32_t Independent128BBlocks;
    uint32_t MaxCompressedBlockSize;  // 0 (default 256), 64, 128, or 256
    uint32_t PipeAligned;
};

struct XG_DCC_ENCODE_RESULT {
    uint64_t DccOffsetBytes;
    uint64_t DccSizeBytes;
    uint64_t EncodedByteCount;
    uint32_t BlockCountX;
    uint32_t BlockCountY;
    uint32_t TotalBlocks;
    uint8_t EncodedKeyByte;
    uint8_t Reserved[3];
};

struct XG_TEXTURE3D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
    uint32_t MipLevels;
    uint32_t Format;          // DXGI_FORMAT
    uint32_t BindFlags;       // XG_BIND_FLAGS
    XG_TILE_MODE TileMode;
};

struct XG_MIPLEVEL_LAYOUT {
    uint32_t WidthElements;
    uint32_t HeightElements;
    uint32_t PaddedWidthElements;
    uint32_t PaddedHeightElements;
    uint32_t TileWidthElements;
    uint32_t TileHeightElements;
    uint32_t TilesX;
    uint32_t TilesY;
    uint32_t TileSizeBytes;
    uint64_t OffsetBytes;
    uint64_t SizeBytes;
    uint64_t SlicePitchBytes;
    uint32_t RowPitchBytes;   // Meaningful for linear layouts

    // Extended 3D & GFX10 mip-tail fields
    uint32_t DepthElements;
    uint32_t PaddedDepthElements;
    uint32_t TileDepthElements;
    uint32_t TilesZ;
    uint32_t InMipTail;
    uint32_t MipTailSubTileOffsetBytes;
    uint32_t MipTailSubBlockBytes;
    uint32_t MipTailPaddedWidthElements;
    uint32_t MipTailPaddedHeightElements;
    uint32_t MipTailPaddedDepthElements;
    uint64_t DepthPitchBytes; // Linear 3D slice pitch per Z plane
};

constexpr uint32_t XG_MAX_MIP_LEVELS = 15u;
constexpr uint32_t XG_MAX_PLANES = 2u;

struct XG_RESOURCE_LAYOUT {
    uint64_t SizeBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t MipLevels;
    uint32_t ArraySize;
    XG_TILE_MODE TileMode;
    XG_FORMAT_BLOCK_INFO FormatInfo;
    XG_MIPLEVEL_LAYOUT Mips[XG_MAX_MIP_LEVELS];

    // Extended 3D, mip-tail, cube-map & multi-plane depth/stencil layout fields
    uint32_t Dimension;
    uint32_t Depth;
    uint32_t IsCubeMap;
    uint32_t PlaneCount;
    uint32_t MipTailStartLevel;
    uint64_t MipTailOffsetBytes;
    uint64_t MipTailSlicePitchBytes;
    uint64_t PlaneOffsets[XG_MAX_PLANES];
    uint64_t PlaneSizes[XG_MAX_PLANES];
    XG_FORMAT_BLOCK_INFO PlaneFormatInfo[XG_MAX_PLANES];
    XG_MIPLEVEL_LAYOUT Plane1Mips[XG_MAX_MIP_LEVELS];
    uint32_t Plane1MipTailStartLevel;
    uint64_t Plane1MipTailOffsetBytes;
    uint64_t Plane1MipTailSlicePitchBytes;
};

struct IXGTextureAddressComputer {
    virtual uint32_t __stdcall AddRef() = 0;
    virtual uint32_t __stdcall Release() = 0;
    virtual int32_t __stdcall GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) const = 0;
    virtual uint64_t __stdcall GetTexelElementOffsetBytes(
        uint32_t plane,
        uint32_t mipLevel,
        uint32_t xElement,
        uint32_t yElement,
        uint32_t zOrSlice,
        uint32_t sample) const = 0;
    virtual int32_t __stdcall CopyIntoSubresource(
        void* pDstTiledResource,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcLinearData,
        uint32_t srcRowPitchBytes,
        uint32_t srcSlicePitchBytes) const = 0;
    virtual int32_t __stdcall CopyFromSubresource(
        void* pDstLinearData,
        uint32_t plane,
        uint32_t subresource,
        const void* pSrcTiledResource,
        uint32_t dstRowPitchBytes,
        uint32_t dstSlicePitchBytes) const = 0;
    virtual ~IXGTextureAddressComputer() = default;
};

namespace openscarlett::xg {

bool IsTileMode3D(XG_TILE_MODE mode) noexcept;

uint32_t GetFormatPlaneCount(uint32_t dxgi_format) noexcept;

bool GetFormatBlockInfo(uint32_t dxgi_format, XG_FORMAT_BLOCK_INFO& out_info) noexcept;

bool GetFormatPlaneBlockInfo(
    uint32_t dxgi_format,
    uint32_t plane,
    XG_FORMAT_BLOCK_INFO& out_info) noexcept;

XG_TILE_MODE ComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags) noexcept;

XG_TEXTURE2D_DESC MakeCubeMapDesc(
    uint32_t face_size,
    uint32_t mip_levels,
    uint32_t num_cubes,
    uint32_t dxgi_format,
    XG_TILE_MODE tile_mode,
    uint32_t bind_flags = XG_BIND_SHADER_RESOURCE) noexcept;

bool ComputeTexture2DLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept;

bool ComputeTexture3DLayout(const XG_TEXTURE3D_DESC& desc, XG_RESOURCE_LAYOUT& out_layout) noexcept;

uint64_t ComputeElementByteOffset(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept;

uint64_t ComputeElementByteOffsetPlane(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    uint32_t element_x,
    uint32_t element_y) noexcept;

uint64_t ComputeElementByteOffset3D(
    const XG_RESOURCE_LAYOUT& layout,
    uint32_t mip_level,
    uint32_t element_x,
    uint32_t element_y,
    uint32_t element_z) noexcept;

bool TileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileSurface2D(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept;

bool TileSurface2DPlane(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileSurface2DPlane(
    const XG_TEXTURE2D_DESC& desc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes) noexcept;

bool TileSurface3D(
    const XG_TEXTURE3D_DESC& desc,
    uint32_t mip_level,
    const void* src_linear,
    uint32_t src_row_pitch_bytes,
    uint32_t src_slice_pitch_bytes,
    void* dst_tiled,
    uint64_t dst_tiled_size_bytes) noexcept;

bool UntileSurface3D(
    const XG_TEXTURE3D_DESC& desc,
    uint32_t mip_level,
    const void* src_tiled,
    uint64_t src_tiled_size_bytes,
    void* dst_linear,
    uint32_t dst_row_pitch_bytes,
    uint32_t dst_slice_pitch_bytes) noexcept;

bool QueryScarlettPipeConfig(
    XG_CONSOLE_TYPE console_type,
    XG_SCARLETT_PIPE_CONFIG& out_config) noexcept;

bool ComputeTexture2DMetadataLayout(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    XG_RESOURCE_METADATA_LAYOUT& out_meta) noexcept;

uint8_t EncodeDccKeyByte(
    XG_DCC_BLOCK_STATE state,
    uint32_t max_compressed_block_size,
    uint32_t independent_64b,
    uint32_t independent_128b,
    uint32_t sample_count) noexcept;

bool DecodeDccKeyByte(
    uint8_t key_byte,
    XG_DCC_BLOCK_STATE& out_state) noexcept;

bool EncodeDccMetadata(
    const XG_DCC_ENCODE_PARAMS& params,
    void* dst_dcc_buffer,
    uint64_t dst_dcc_size_bytes,
    XG_DCC_ENCODE_RESULT* out_result) noexcept;

bool VerifyDccMetadata(
    const XG_DCC_ENCODE_PARAMS& params,
    const void* dcc_buffer,
    uint64_t dcc_size_bytes,
    XG_DCC_ENCODE_RESULT* out_result) noexcept;

}  // namespace openscarlett::xg

extern "C" {

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer);

OPENSCARLETT_XG_API int32_t __stdcall XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer);

OPENSCARLETT_XG_API XG_TILE_MODE __stdcall XGComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags);

OPENSCARLETT_XG_API int32_t __stdcall XGGetFormatBlockInfo(
    uint32_t dxgi_format,
    XG_FORMAT_BLOCK_INFO* pOutInfo);

OPENSCARLETT_XG_API uint32_t __stdcall XGGetFormatPlaneCount(uint32_t dxgi_format);

OPENSCARLETT_XG_API int32_t __stdcall XGGetFormatPlaneBlockInfo(
    uint32_t dxgi_format,
    uint32_t plane,
    XG_FORMAT_BLOCK_INFO* pOutInfo);

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface2DPlane(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface2DPlane(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t plane,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGTileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    uint32_t srcSlicePitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGUntileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes,
    uint32_t dstSlicePitchBytes);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeMipLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeMipLayout3D(
    const XG_TEXTURE3D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout);

OPENSCARLETT_XG_API void __stdcall OpenScarlettXgResetTelemetry();

OPENSCARLETT_XG_API int __stdcall OpenScarlettXgGetTelemetryEntry(
    const char* symbol_name,
    uint64_t* out_total_calls);

OPENSCARLETT_XG_API int __stdcall OpenScarlettXgGetTelemetryDetail(
    const char* symbol_name,
    uint64_t* out_total_calls,
    uint64_t* out_not_impl_calls,
    int32_t* out_last_hr);

OPENSCARLETT_XG_API int32_t __stdcall XGQueryScarlettPipeConfig(
    XG_CONSOLE_TYPE console_type,
    XG_SCARLETT_PIPE_CONFIG* pOutConfig);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeTexture2DMetadataLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_RESOURCE_METADATA_LAYOUT* pOutMeta);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeHtileLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_METADATA_PLANE_INFO* pOutHtile);

OPENSCARLETT_XG_API int32_t __stdcall XGComputeCmaskLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_CONSOLE_TYPE console_type,
    XG_METADATA_PLANE_INFO* pOutCmask);

OPENSCARLETT_XG_API int32_t __stdcall XGEncodeDCC(
    const XG_DCC_ENCODE_PARAMS* pParams,
    void* pDstDccBuffer,
    uint64_t dstDccSizeBytes,
    XG_DCC_ENCODE_RESULT* pOutResult);

OPENSCARLETT_XG_API int32_t __stdcall XGDecodeDCCKey(
    uint8_t dccKeyByte,
    uint32_t* pOutBlockState);

OPENSCARLETT_XG_API int32_t __stdcall XGVerifyDCC(
    const XG_DCC_ENCODE_PARAMS* pParams,
    const void* pDccBuffer,
    uint64_t dccSizeBytes,
    XG_DCC_ENCODE_RESULT* pOutResult);

}  // extern "C"
