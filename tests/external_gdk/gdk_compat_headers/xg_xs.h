#pragma once

// MIT Xbox Scarlett Texture Address Computer (<xg_xs.h>) public ABI header.

#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <windows.h>

#define XGXS_COMPAT_API __declspec(dllimport)

enum XG_TILE_MODE : uint32_t {
    XG_TILE_MODE_LINEAR = 0u,
    XG_TILE_MODE_2D_THIN_4KB_S = 1u,
    XG_TILE_MODE_2D_THIN_64KB_S = 2u,
    XG_TILE_MODE_2D_THIN_64KB_D = 3u,
    XG_TILE_MODE_2D_THIN_64KB_R_X = 4u,
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
    uint32_t Format;
    uint32_t SampleCount;
    uint32_t BindFlags;
    XG_TILE_MODE TileMode;
};

struct XG_TEXTURE3D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
    uint32_t MipLevels;
    uint32_t Format;
    uint32_t BindFlags;
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
    uint32_t RowPitchBytes;
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
    uint64_t DepthPitchBytes;
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

extern "C" {

XGXS_COMPAT_API int32_t __stdcall XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer);

XGXS_COMPAT_API int32_t __stdcall XGCreateTexture3DComputer(
    const XG_TEXTURE3D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer);

XGXS_COMPAT_API XG_TILE_MODE __stdcall XGComputeOptimalTileMode(
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    uint32_t bind_flags);

XGXS_COMPAT_API int32_t __stdcall XGGetFormatBlockInfo(
    uint32_t dxgi_format,
    XG_FORMAT_BLOCK_INFO* pOutInfo);

XGXS_COMPAT_API int32_t __stdcall XGTileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

XGXS_COMPAT_API int32_t __stdcall XGUntileSurface2D(
    const XG_TEXTURE2D_DESC* pDesc,
    uint32_t mip_level,
    uint32_t slice_index,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes);

XGXS_COMPAT_API int32_t __stdcall XGTileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcLinear,
    uint32_t srcRowPitchBytes,
    uint32_t srcSlicePitchBytes,
    void* pDstTiled,
    uint64_t dstTiledSizeBytes);

XGXS_COMPAT_API int32_t __stdcall XGUntileSurface3D(
    const XG_TEXTURE3D_DESC* pDesc,
    uint32_t mip_level,
    const void* pSrcTiled,
    uint64_t srcTiledSizeBytes,
    void* pDstLinear,
    uint32_t dstRowPitchBytes,
    uint32_t dstSlicePitchBytes);

XGXS_COMPAT_API int32_t __stdcall XGComputeMipLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout);

XGXS_COMPAT_API int32_t __stdcall XGComputeMipLayout3D(
    const XG_TEXTURE3D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pOutLayout);

}  // extern "C"
