#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <dxgiformat.h>
#  include <windows.h>
#endif

#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/io/dstorage_abi.h"
#include "os_check.h"

namespace {

void FillPattern(std::vector<uint8_t>& buf, uint32_t seed) {
    uint32_t s = seed ^ 0x9E3779B9u;
    for (size_t i = 0u; i < buf.size(); ++i) {
        s = s * 1664525u + 1013904223u;
        buf[i] = static_cast<uint8_t>((s >> 24u) ^ ((i * 31u + seed) & 0xFFu));
    }
}

#if defined(_WIN32)

// ===========================================================================
// 1. ALL 15 GFX10 2D Tiled Modes + Linear (across BPE 1, 2, 4, 8, 16)
// ===========================================================================
void TestXgAll2dModes() {
    std::printf("[Test 1] xg_xs.dll ALL 15 GFX10 2D Tiled Modes + Linear...\n");

    struct ModeEntry {
        XG_TILE_MODE mode;
        const char* name;
    };
    const std::array<ModeEntry, 16> modes = {{
        {XG_TILE_MODE_LINEAR, "LINEAR"},
        {XG_TILE_MODE_2D_THIN_256B_S, "2D_THIN_256B_S"},
        {XG_TILE_MODE_2D_THIN_256B_D, "2D_THIN_256B_D"},
        {XG_TILE_MODE_2D_THIN_256B_R, "2D_THIN_256B_R"},
        {XG_TILE_MODE_2D_THIN_4KB_Z, "2D_THIN_4KB_Z"},
        {XG_TILE_MODE_2D_THIN_4KB_S, "2D_THIN_4KB_S"},
        {XG_TILE_MODE_2D_THIN_4KB_D, "2D_THIN_4KB_D"},
        {XG_TILE_MODE_2D_THIN_4KB_R, "2D_THIN_4KB_R"},
        {XG_TILE_MODE_2D_THIN_64KB_Z, "2D_THIN_64KB_Z"},
        {XG_TILE_MODE_2D_THIN_64KB_S, "2D_THIN_64KB_S"},
        {XG_TILE_MODE_2D_THIN_64KB_D, "2D_THIN_64KB_D"},
        {XG_TILE_MODE_2D_THIN_64KB_R, "2D_THIN_64KB_R"},
        {XG_TILE_MODE_2D_THIN_64KB_Z_X, "2D_THIN_64KB_Z_X"},
        {XG_TILE_MODE_2D_THIN_64KB_S_X, "2D_THIN_64KB_S_X"},
        {XG_TILE_MODE_2D_THIN_64KB_D_X, "2D_THIN_64KB_D_X"},
        {XG_TILE_MODE_2D_THIN_64KB_R_X, "2D_THIN_64KB_R_X"},
    }};

    const std::array<uint32_t, 5> bpe_formats = {
        DXGI_FORMAT_R8_UNORM,             // 1 B
        DXGI_FORMAT_R16_FLOAT,            // 2 B
        DXGI_FORMAT_R8G8B8A8_UNORM,       // 4 B
        DXGI_FORMAT_R16G16B16A16_FLOAT,   // 8 B
        DXGI_FORMAT_R32G32B32A32_FLOAT,   // 16 B
    };

    uint32_t roundtrips = 0u;
    std::vector<uint8_t> tiled_64kb_s;
    std::vector<uint8_t> tiled_64kb_s_x;
    std::vector<uint8_t> tiled_64kb_d;
    std::vector<uint8_t> tiled_64kb_z;
    std::vector<uint8_t> tiled_64kb_r;

    for (const auto& me : modes) {
        for (uint32_t fmt : bpe_formats) {
            XG_FORMAT_BLOCK_INFO fi{};
            OS_REQUIRE(XGGetFormatBlockInfo(fmt, &fi) == 0);

            // Multi-tile NPOT size so every 256B/4KB/64KB mode exercises multiple tiles & XOR bits.
            const uint32_t w = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? 260u : 137u;
            const uint32_t h = (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? 140u : 93u;

            XG_TEXTURE2D_DESC desc{};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = 1u;
            desc.ArraySize = 1u;
            desc.Format = fmt;
            desc.SampleCount = 1u;
            desc.TileMode = me.mode;

            XG_RESOURCE_LAYOUT layout{};
            OS_REQUIRE(XGComputeMipLayout(&desc, &layout) == 0);
            OS_REQUIRE(layout.SizeBytes > 0ull);
            OS_REQUIRE(layout.Dimension == XG_RESOURCE_DIMENSION_TEXTURE2D);

            const uint32_t row_pitch = w * fi.bytes_per_block;
            std::vector<uint8_t> src(static_cast<size_t>(row_pitch) * h);
            FillPattern(src, static_cast<uint32_t>(me.mode) * 100u + fmt);

            std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xCCu);
            OS_REQUIRE(XGTileSurface2D(&desc, 0u, 0u, src.data(), row_pitch, tiled.data(), tiled.size()) == 0);

            if (me.mode != XG_TILE_MODE_LINEAR) {
                size_t diffs = 0u;
                const size_t cmp = std::min(src.size(), tiled.size());
                for (size_t i = 0u; i < cmp; ++i) {
                    if (src[i] != tiled[i]) ++diffs;
                }
                OS_REQUIRE(diffs > cmp / 4u);
            }

            std::vector<uint8_t> dst(src.size(), 0x33u);
            OS_REQUIRE(XGUntileSurface2D(&desc, 0u, 0u, tiled.data(), tiled.size(), dst.data(), row_pitch) == 0);
            OS_REQUIRE(std::memcmp(src.data(), dst.data(), src.size()) == 0);
            ++roundtrips;

            // Capture RGBA8 64KB mode variants with identical input to prove distinct swizzles.
            if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                std::vector<uint8_t> common_src(static_cast<size_t>(row_pitch) * h);
                FillPattern(common_src, 0x12345678u);
                std::vector<uint8_t> common_tiled(static_cast<size_t>(layout.SizeBytes), 0u);
                OS_REQUIRE(XGTileSurface2D(&desc, 0u, 0u, common_src.data(), row_pitch,
                                           common_tiled.data(), common_tiled.size()) == 0);
                if (me.mode == XG_TILE_MODE_2D_THIN_64KB_S) tiled_64kb_s = common_tiled;
                if (me.mode == XG_TILE_MODE_2D_THIN_64KB_S_X) tiled_64kb_s_x = common_tiled;
                if (me.mode == XG_TILE_MODE_2D_THIN_64KB_D) tiled_64kb_d = common_tiled;
                if (me.mode == XG_TILE_MODE_2D_THIN_64KB_Z) tiled_64kb_z = common_tiled;
                if (me.mode == XG_TILE_MODE_2D_THIN_64KB_R) tiled_64kb_r = common_tiled;
            }
        }
    }

    // Prove S, D, Z, R, S_X all produce genuinely distinct swizzled layouts.
    OS_REQUIRE(tiled_64kb_s != tiled_64kb_s_x);
    OS_REQUIRE(tiled_64kb_s != tiled_64kb_d);
    OS_REQUIRE(tiled_64kb_s != tiled_64kb_z);
    OS_REQUIRE(tiled_64kb_s != tiled_64kb_r);
    OS_REQUIRE(tiled_64kb_d != tiled_64kb_z);
    OS_REQUIRE(tiled_64kb_d != tiled_64kb_r);
    OS_REQUIRE(tiled_64kb_z != tiled_64kb_r);

    std::printf("  Verified all 16 2D modes x 5 BPE classes = %u bit-exact roundtrips!\n", roundtrips);
}

// ===========================================================================
// 2. ALL 6 GFX10 3D Thick Modes + Linear 3D Volumes + 3D Mip Tails
// ===========================================================================
void TestXgAll3dModesAndVolumes() {
    std::printf("[Test 2] xg_xs.dll ALL 6 GFX10 3D Thick Modes + 3D Volumes + Mip Tails...\n");

    struct Mode3D {
        XG_TILE_MODE mode;
        const char* name;
    };
    const std::array<Mode3D, 7> modes3d = {{
        {XG_TILE_MODE_LINEAR, "LINEAR_3D"},
        {XG_TILE_MODE_3D_THICK_4KB_S, "3D_THICK_4KB_S"},
        {XG_TILE_MODE_3D_THICK_64KB_Z, "3D_THICK_64KB_Z"},
        {XG_TILE_MODE_3D_THICK_64KB_S, "3D_THICK_64KB_S"},
        {XG_TILE_MODE_3D_THICK_64KB_Z_X, "3D_THICK_64KB_Z_X"},
        {XG_TILE_MODE_3D_THICK_64KB_S_X, "3D_THICK_64KB_S_X"},
        {XG_TILE_MODE_3D_THICK_64KB_R_X, "3D_THICK_64KB_R_X"},
    }};

    for (const auto& m3 : modes3d) {
        XG_TEXTURE3D_DESC desc{};
        desc.Width = 32u;
        desc.Height = 32u;
        desc.Depth = 16u;
        desc.MipLevels = 6u;  // 32x32x16 -> 16x16x8 -> 8x8x4 -> 4x4x2 -> 2x2x1 -> 1x1x1
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = XG_BIND_SHADER_RESOURCE;
        desc.TileMode = m3.mode;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(XGComputeMipLayout3D(&desc, &layout) == 0);
        OS_REQUIRE(layout.Dimension == XG_RESOURCE_DIMENSION_TEXTURE3D);
        OS_REQUIRE(layout.MipLevels == 6u);
        if (m3.mode != XG_TILE_MODE_LINEAR) {
            OS_REQUIRE(layout.MipTailStartLevel < layout.MipLevels);
            OS_REQUIRE(layout.Mips[layout.MipLevels - 1u].InMipTail == 1u);
        }

        // Create IXGTextureAddressComputer for 3D and verify all 6 mips coexist in one buffer!
        IXGTextureAddressComputer* comp = nullptr;
        OS_REQUIRE(XGCreateTexture3DComputer(&desc, &comp) == 0 && comp != nullptr);

        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xA5u);
        std::vector<std::vector<uint8_t>> mip_srcs(desc.MipLevels);

        for (uint32_t mip = 0u; mip < desc.MipLevels; ++mip) {
            const uint32_t mw = std::max(1u, desc.Width >> mip);
            const uint32_t mh = std::max(1u, desc.Height >> mip);
            const uint32_t md = std::max(1u, desc.Depth >> mip);
            const uint32_t row_pitch = mw * 4u;
            const uint32_t slice_pitch = row_pitch * mh;
            mip_srcs[mip].resize(static_cast<size_t>(slice_pitch) * md);
            FillPattern(mip_srcs[mip], 0x3D00u + static_cast<uint32_t>(m3.mode) * 16u + mip);

            OS_REQUIRE(comp->CopyIntoSubresource(
                           tiled.data(), 0u, mip, mip_srcs[mip].data(), row_pitch, slice_pitch) == 0);
        }

        // Verify mip 0 genuinely swizzled for non-linear 3D modes
        if (m3.mode != XG_TILE_MODE_LINEAR) {
            size_t diffs = 0u;
            const size_t cmp = std::min(mip_srcs[0].size(), tiled.size());
            for (size_t i = 0u; i < cmp; ++i) {
                if (mip_srcs[0][i] != tiled[i]) ++diffs;
            }
            OS_REQUIRE(diffs > cmp / 4u);
        }

        // Untile ALL 6 mips back and verify bit-exact match for every mip (including mip tail!)
        for (uint32_t mip = 0u; mip < desc.MipLevels; ++mip) {
            const uint32_t mw = std::max(1u, desc.Width >> mip);
            const uint32_t mh = std::max(1u, desc.Height >> mip);
            const uint32_t row_pitch = mw * 4u;
            const uint32_t slice_pitch = row_pitch * mh;
            std::vector<uint8_t> dst(mip_srcs[mip].size(), 0x11u);
            OS_REQUIRE(comp->CopyFromSubresource(
                           dst.data(), 0u, mip, tiled.data(), row_pitch, slice_pitch) == 0);
            OS_REQUIRE(std::memcmp(mip_srcs[mip].data(), dst.data(), dst.size()) == 0);
        }

        // Also verify direct XGTileSurface3D / XGUntileSurface3D C exports on mip 0
        {
            const uint32_t row_pitch = desc.Width * 4u;
            const uint32_t slice_pitch = row_pitch * desc.Height;
            std::vector<uint8_t> tiled_c(static_cast<size_t>(layout.SizeBytes), 0x00u);
            OS_REQUIRE(XGTileSurface3D(&desc, 0u, mip_srcs[0].data(), row_pitch, slice_pitch,
                                       tiled_c.data(), tiled_c.size()) == 0);
            std::vector<uint8_t> dst_c(mip_srcs[0].size(), 0x00u);
            OS_REQUIRE(XGUntileSurface3D(&desc, 0u, tiled_c.data(), tiled_c.size(),
                                         dst_c.data(), row_pitch, slice_pitch) == 0);
            OS_REQUIRE(std::memcmp(mip_srcs[0].data(), dst_c.data(), dst_c.size()) == 0);
        }

        comp->Release();
        std::printf("  3D %-20s | 32x32x16 6 mips | tailStart=%u | size=%llu B | BIT-EXACT\n",
                    m3.name, layout.MipTailStartLevel, static_cast<unsigned long long>(layout.SizeBytes));
    }

    // Verify NPOT 3D volume across all BPE classes (1, 2, 4, 8, 16) for both 4KB and 64KB 3D modes
    const std::array<uint32_t, 5> bpe3d_fmts = {
        DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
    };
    for (XG_TILE_MODE m : {XG_TILE_MODE_3D_THICK_4KB_S, XG_TILE_MODE_3D_THICK_64KB_S_X}) {
        for (uint32_t fmt : bpe3d_fmts) {
            XG_FORMAT_BLOCK_INFO fi{};
            OS_REQUIRE(XGGetFormatBlockInfo(fmt, &fi) == 0);
            XG_TEXTURE3D_DESC d{19u, 17u, 11u, 1u, fmt, 0u, m};
            XG_RESOURCE_LAYOUT l{};
            OS_REQUIRE(XGComputeMipLayout3D(&d, &l) == 0);
            const uint32_t rp = d.Width * fi.bytes_per_block;
            const uint32_t sp = rp * d.Height;
            std::vector<uint8_t> src(static_cast<size_t>(sp) * d.Depth);
            FillPattern(src, static_cast<uint32_t>(m) * 50u + fmt);
            std::vector<uint8_t> tiled(static_cast<size_t>(l.SizeBytes), 0u);
            std::vector<uint8_t> dst(src.size(), 0u);
            OS_REQUIRE(XGTileSurface3D(&d, 0u, src.data(), rp, sp, tiled.data(), tiled.size()) == 0);
            OS_REQUIRE(XGUntileSurface3D(&d, 0u, tiled.data(), tiled.size(), dst.data(), rp, sp) == 0);
            OS_REQUIRE(std::memcmp(src.data(), dst.data(), src.size()) == 0);
        }
    }

    // Verify 3D rejection of 2D-only modes and 2D rejection of 3D-only modes
    {
        XG_TEXTURE3D_DESC bad3d{16u, 16u, 16u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 0u, XG_TILE_MODE_2D_THIN_64KB_S};
        XG_RESOURCE_LAYOUT l{};
        OS_REQUIRE(XGComputeMipLayout3D(&bad3d, &l) != 0);

        XG_TEXTURE2D_DESC bad2d{16u, 16u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 1u, 0u, XG_TILE_MODE_3D_THICK_64KB_S};
        OS_REQUIRE(XGComputeMipLayout(&bad2d, &l) != 0);
    }
}

// ===========================================================================
// 3. 2D Mip Chains + GFX10 Mip Tails + Texture Arrays + Cube Maps
// ===========================================================================
void TestXgMipTailsArraysAndCubes() {
    std::printf("[Test 3] xg_xs.dll Mip Chains + Mip Tails + Texture Arrays + Cube Maps...\n");

    // 3A. 2D Texture Array (256x256, 9 mips, 4 array slices, 64KB_S_X)
    {
        XG_TEXTURE2D_DESC desc{};
        desc.Width = 256u;
        desc.Height = 256u;
        desc.MipLevels = 9u;
        desc.ArraySize = 4u;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleCount = 1u;
        desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S_X;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(XGComputeMipLayout(&desc, &layout) == 0);
        OS_REQUIRE(layout.MipLevels == 9u && layout.ArraySize == 4u);
        OS_REQUIRE(layout.MipTailStartLevel == 2u);  // 64x64 <= 128/2 enters mip tail!
        OS_REQUIRE(layout.MipTailSlicePitchBytes == 65536ull);
        for (uint32_t m = 0u; m < 2u; ++m) OS_REQUIRE(layout.Mips[m].InMipTail == 0u);
        for (uint32_t m = 2u; m < 9u; ++m) OS_REQUIRE(layout.Mips[m].InMipTail == 1u);

        // Tile all 4 * 9 = 36 subresources into ONE shared buffer!
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xEEu);
        std::vector<std::vector<uint8_t>> srcs(static_cast<size_t>(desc.ArraySize) * desc.MipLevels);

        for (uint32_t s = 0u; s < desc.ArraySize; ++s) {
            for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                const uint32_t mw = std::max(1u, desc.Width >> m);
                const uint32_t mh = std::max(1u, desc.Height >> m);
                const uint32_t rp = mw * 4u;
                auto& src = srcs[static_cast<size_t>(s) * desc.MipLevels + m];
                src.resize(static_cast<size_t>(rp) * mh);
                FillPattern(src, 0xA000u + s * 100u + m);
                OS_REQUIRE(XGTileSurface2D(&desc, m, s, src.data(), rp, tiled.data(), tiled.size()) == 0);
            }
        }

        // Untile ALL 36 subresources and verify bit-exact equality!
        for (uint32_t s = 0u; s < desc.ArraySize; ++s) {
            for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                const uint32_t mw = std::max(1u, desc.Width >> m);
                const uint32_t rp = mw * 4u;
                const auto& src = srcs[static_cast<size_t>(s) * desc.MipLevels + m];
                std::vector<uint8_t> dst(src.size(), 0x00u);
                OS_REQUIRE(XGUntileSurface2D(&desc, m, s, tiled.data(), tiled.size(), dst.data(), rp) == 0);
                OS_REQUIRE(std::memcmp(src.data(), dst.data(), src.size()) == 0);
            }
        }
        std::printf("  2D Array (256x256, 9 mips, 4 slices, 64KB_S_X): tailStart=%u | size=%llu B | 36/36 BIT-EXACT\n",
                    layout.MipTailStartLevel, static_cast<unsigned long long>(layout.SizeBytes));
    }

    // 3B. Cube Map Array (128x128 BC7, 8 mips, 6 faces x 1 cube = 48 subresources, 64KB_S)
    {
        const XG_TEXTURE2D_DESC cube = openscarlett::xg::MakeCubeMapDesc(
            128u, 8u, 1u, DXGI_FORMAT_BC7_UNORM, XG_TILE_MODE_2D_THIN_64KB_S);
        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(XGComputeMipLayout(&cube, &layout) == 0);
        OS_REQUIRE(layout.IsCubeMap == 1u && layout.ArraySize == 6u && layout.MipLevels == 8u);
        OS_REQUIRE(layout.MipTailStartLevel < layout.MipLevels);

        IXGTextureAddressComputer* comp = nullptr;
        OS_REQUIRE(XGCreateTexture2DComputer(&cube, &comp) == 0 && comp != nullptr);

        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0u);
        std::vector<std::vector<uint8_t>> srcs(static_cast<size_t>(cube.ArraySize) * cube.MipLevels);

        for (uint32_t face = 0u; face < cube.ArraySize; ++face) {
            for (uint32_t m = 0u; m < cube.MipLevels; ++m) {
                const uint32_t subres = face * cube.MipLevels + m;
                const uint32_t mw = std::max(1u, cube.Width >> m);
                const uint32_t mh = std::max(1u, cube.Height >> m);
                const uint32_t bx = (mw + 3u) / 4u;
                const uint32_t by = (mh + 3u) / 4u;
                const uint32_t rp = bx * 16u;
                srcs[subres].resize(static_cast<size_t>(rp) * by);
                FillPattern(srcs[subres], 0xC0BEu + subres);
                OS_REQUIRE(comp->CopyIntoSubresource(tiled.data(), 0u, subres, srcs[subres].data(), rp, 0u) == 0);
            }
        }
        for (uint32_t face = 0u; face < cube.ArraySize; ++face) {
            for (uint32_t m = 0u; m < cube.MipLevels; ++m) {
                const uint32_t subres = face * cube.MipLevels + m;
                const uint32_t mw = std::max(1u, cube.Width >> m);
                const uint32_t bx = (mw + 3u) / 4u;
                const uint32_t rp = bx * 16u;
                std::vector<uint8_t> dst(srcs[subres].size(), 0u);
                OS_REQUIRE(comp->CopyFromSubresource(dst.data(), 0u, subres, tiled.data(), rp, 0u) == 0);
                OS_REQUIRE(std::memcmp(srcs[subres].data(), dst.data(), dst.size()) == 0);
            }
        }
        comp->Release();
        std::printf("  Cube Map (128x128 BC7, 8 mips x 6 faces, 64KB_S): tailStart=%u | size=%llu B | 48/48 BIT-EXACT\n",
                    layout.MipTailStartLevel, static_cast<unsigned long long>(layout.SizeBytes));
    }
}

// ===========================================================================
// 4. ALL Formats: BC1-BC7, RGB9E5_SHAREDEXP, and Depth/Stencil (Plane 0 & 1)
// ===========================================================================
void TestXgAllFormatsAndPlanarDepthStencil() {
    std::printf("[Test 4] xg_xs.dll BC1-BC7, RGB9E5, and Depth/Stencil (Plane 0 + Plane 1)...\n");

    struct FmtCase {
        uint32_t format;
        const char* name;
        uint32_t expected_bpe;
        uint32_t expected_planes;
        XG_TILE_MODE mode;
    };
    const std::array<FmtCase, 19> cases = {{
        {DXGI_FORMAT_BC1_UNORM, "BC1_UNORM", 8u, 1u, XG_TILE_MODE_2D_THIN_64KB_S},
        {DXGI_FORMAT_BC1_UNORM_SRGB, "BC1_UNORM_SRGB", 8u, 1u, XG_TILE_MODE_2D_THIN_4KB_S},
        {DXGI_FORMAT_BC2_UNORM, "BC2_UNORM", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_S},
        {DXGI_FORMAT_BC3_UNORM, "BC3_UNORM", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_S_X},
        {DXGI_FORMAT_BC4_UNORM, "BC4_UNORM", 8u, 1u, XG_TILE_MODE_2D_THIN_4KB_R},
        {DXGI_FORMAT_BC5_SNORM, "BC5_SNORM", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_R},
        {DXGI_FORMAT_BC6H_UF16, "BC6H_UF16", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_S},
        {DXGI_FORMAT_BC6H_SF16, "BC6H_SF16", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_D_X},
        {DXGI_FORMAT_BC7_UNORM, "BC7_UNORM", 16u, 1u, XG_TILE_MODE_2D_THIN_64KB_S},
        {DXGI_FORMAT_BC7_UNORM_SRGB, "BC7_UNORM_SRGB", 16u, 1u, XG_TILE_MODE_2D_THIN_4KB_D},
        {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, "R9G9B9E5_SHAREDEXP", 4u, 1u, XG_TILE_MODE_2D_THIN_64KB_S_X},
        {DXGI_FORMAT_D16_UNORM, "D16_UNORM", 2u, 1u, XG_TILE_MODE_2D_THIN_4KB_Z},
        {DXGI_FORMAT_D32_FLOAT, "D32_FLOAT", 4u, 1u, XG_TILE_MODE_2D_THIN_64KB_Z},
        {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT", 4u, 2u, XG_TILE_MODE_2D_THIN_4KB_Z},
        {DXGI_FORMAT_R24G8_TYPELESS, "R24G8_TYPELESS", 4u, 2u, XG_TILE_MODE_2D_THIN_64KB_Z_X},
        {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32_FLOAT_S8X24_UINT", 4u, 2u, XG_TILE_MODE_2D_THIN_64KB_Z_X},
        {DXGI_FORMAT_R32G8X24_TYPELESS, "R32G8X24_TYPELESS", 4u, 2u, XG_TILE_MODE_2D_THIN_4KB_Z},
        {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "R32_FLOAT_X8X24_TYPELESS", 4u, 1u, XG_TILE_MODE_2D_THIN_64KB_Z},
        {DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, "X32_TYPELESS_G8X24_UINT", 1u, 1u, XG_TILE_MODE_2D_THIN_64KB_Z},
    }};

    for (const auto& c : cases) {
        XG_FORMAT_BLOCK_INFO fi{};
        OS_REQUIRE(XGGetFormatBlockInfo(c.format, &fi) == 0);
        OS_REQUIRE(fi.bytes_per_block == c.expected_bpe);
        OS_REQUIRE(XGGetFormatPlaneCount(c.format) == c.expected_planes);

        XG_TEXTURE2D_DESC desc{};
        desc.Width = 128u;
        desc.Height = 128u;
        desc.MipLevels = 4u;
        desc.ArraySize = 2u;
        desc.Format = c.format;
        desc.SampleCount = 1u;
        desc.BindFlags = (c.expected_planes == 2u) ? XG_BIND_DEPTH_STENCIL : XG_BIND_SHADER_RESOURCE;
        desc.TileMode = c.mode;

        XG_RESOURCE_LAYOUT layout{};
        OS_REQUIRE(XGComputeMipLayout(&desc, &layout) == 0);
        OS_REQUIRE(layout.PlaneCount == c.expected_planes);

        IXGTextureAddressComputer* comp = nullptr;
        OS_REQUIRE(XGCreateTexture2DComputer(&desc, &comp) == 0 && comp != nullptr);

        std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0x77u);

        // Tile ALL planes x slices x mips simultaneously into ONE tiled resource!
        std::vector<std::vector<uint8_t>> plane_srcs[2];
        for (uint32_t p = 0u; p < c.expected_planes; ++p) {
            XG_FORMAT_BLOCK_INFO pfi{};
            OS_REQUIRE(XGGetFormatPlaneBlockInfo(c.format, p, &pfi) == 0);
            if (p == 1u) OS_REQUIRE(pfi.bytes_per_block == 1u);

            plane_srcs[p].resize(static_cast<size_t>(desc.ArraySize) * desc.MipLevels);
            for (uint32_t s = 0u; s < desc.ArraySize; ++s) {
                for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                    const uint32_t subres = s * desc.MipLevels + m;
                    const uint32_t mw = std::max(1u, desc.Width >> m);
                    const uint32_t mh = std::max(1u, desc.Height >> m);
                    const uint32_t ex = (mw + pfi.block_width - 1u) / pfi.block_width;
                    const uint32_t ey = (mh + pfi.block_height - 1u) / pfi.block_height;
                    const uint32_t rp = ex * pfi.bytes_per_block;
                    plane_srcs[p][subres].resize(static_cast<size_t>(rp) * ey);
                    FillPattern(plane_srcs[p][subres], 0xF000u + c.format * 32u + p * 16u + subres);
                    OS_REQUIRE(comp->CopyIntoSubresource(
                                   tiled.data(), p, subres, plane_srcs[p][subres].data(), rp, 0u) == 0);
                }
            }
        }

        // Untile ALL planes x slices x mips back and verify bit-exact match!
        for (uint32_t p = 0u; p < c.expected_planes; ++p) {
            XG_FORMAT_BLOCK_INFO pfi{};
            OS_REQUIRE(XGGetFormatPlaneBlockInfo(c.format, p, &pfi) == 0);
            for (uint32_t s = 0u; s < desc.ArraySize; ++s) {
                for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                    const uint32_t subres = s * desc.MipLevels + m;
                    const uint32_t mw = std::max(1u, desc.Width >> m);
                    const uint32_t ex = (mw + pfi.block_width - 1u) / pfi.block_width;
                    const uint32_t rp = ex * pfi.bytes_per_block;
                    std::vector<uint8_t> dst(plane_srcs[p][subres].size(), 0u);
                    OS_REQUIRE(comp->CopyFromSubresource(
                                   dst.data(), p, subres, tiled.data(), rp, 0u) == 0);
                    OS_REQUIRE(std::memcmp(plane_srcs[p][subres].data(), dst.data(), dst.size()) == 0);
                }
            }
        }

        comp->Release();
    }

    std::printf("  Verified all 19 format suites (BC1-BC7, RGB9E5, D16/D24S8/D32F/D32FS8X24 Plane 0+1) BIT-EXACT!\n");
}

void TestXgNpotMipChainsAnd3dLut() {
    std::printf("[Test 4b] xg_xs.dll NPOT Mip Chains (300x180, 1920x1080) & 32x32x32 3D LUTs...\n");

    struct NpotCase {
        uint32_t format;
        uint32_t expected_planes;
        XG_TILE_MODE mode;
    };
    const std::array<NpotCase, 5> npot_cases = {{
        {DXGI_FORMAT_BC6H_UF16, 1u, XG_TILE_MODE_2D_THIN_64KB_S},
        {DXGI_FORMAT_BC6H_SF16, 1u, XG_TILE_MODE_2D_THIN_64KB_D_X},
        {DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 1u, XG_TILE_MODE_2D_THIN_64KB_S_X},
        {DXGI_FORMAT_D32_FLOAT, 1u, XG_TILE_MODE_2D_THIN_64KB_Z},
        {DXGI_FORMAT_D24_UNORM_S8_UINT, 2u, XG_TILE_MODE_2D_THIN_4KB_Z},
    }};
    const std::array<std::array<uint32_t, 3>, 2> npot_sizes = {{
        {{300u, 180u, 9u}},
        {{1920u, 1080u, 11u}},
    }};

    uint32_t npot_roundtrips = 0u;
    for (const auto& sz : npot_sizes) {
        for (const auto& c : npot_cases) {
            XG_TEXTURE2D_DESC desc{};
            desc.Width = sz[0];
            desc.Height = sz[1];
            desc.MipLevels = sz[2];
            desc.ArraySize = 1u;
            desc.Format = c.format;
            desc.SampleCount = 1u;
            desc.TileMode = c.mode;

            XG_RESOURCE_LAYOUT layout{};
            OS_REQUIRE(XGComputeMipLayout(&desc, &layout) == 0);
            OS_REQUIRE(layout.SizeBytes > 0ull);

            IXGTextureAddressComputer* comp = nullptr;
            OS_REQUIRE(XGCreateTexture2DComputer(&desc, &comp) == 0 && comp != nullptr);
            std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0x77u);
            std::vector<std::vector<uint8_t>> plane_srcs[2];

            for (uint32_t p = 0u; p < c.expected_planes; ++p) {
                XG_FORMAT_BLOCK_INFO pfi{};
                OS_REQUIRE(XGGetFormatPlaneBlockInfo(c.format, p, &pfi) == 0);
                plane_srcs[p].resize(desc.MipLevels);
                for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                    const uint32_t mw = std::max(1u, desc.Width >> m);
                    const uint32_t mh = std::max(1u, desc.Height >> m);
                    const uint32_t ex = (mw + pfi.block_width - 1u) / pfi.block_width;
                    const uint32_t ey = (mh + pfi.block_height - 1u) / pfi.block_height;
                    const uint32_t rp = ex * pfi.bytes_per_block;
                    plane_srcs[p][m].resize(static_cast<size_t>(rp) * ey);
                    FillPattern(plane_srcs[p][m], 0xA500u + sz[0] + c.format * 37u + p * 13u + m);
                    OS_REQUIRE(comp->CopyIntoSubresource(tiled.data(), p, m, plane_srcs[p][m].data(), rp, 0u) == 0);
                }
            }

            for (uint32_t p = 0u; p < c.expected_planes; ++p) {
                XG_FORMAT_BLOCK_INFO pfi{};
                OS_REQUIRE(XGGetFormatPlaneBlockInfo(c.format, p, &pfi) == 0);
                for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                    const uint32_t mw = std::max(1u, desc.Width >> m);
                    const uint32_t ex = (mw + pfi.block_width - 1u) / pfi.block_width;
                    const uint32_t rp = ex * pfi.bytes_per_block;
                    std::vector<uint8_t> dst(plane_srcs[p][m].size(), 0u);
                    OS_REQUIRE(comp->CopyFromSubresource(dst.data(), p, m, tiled.data(), rp, 0u) == 0);
                    OS_REQUIRE(std::memcmp(plane_srcs[p][m].data(), dst.data(), dst.size()) == 0);
                    ++npot_roundtrips;
                }
            }
            comp->Release();
        }
    }

    const std::array<uint32_t, 2> lut_formats = {
        DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };
    const std::array<XG_TILE_MODE, 2> lut_modes = {
        XG_TILE_MODE_3D_THICK_64KB_S,
        XG_TILE_MODE_3D_THICK_64KB_S_X,
    };
    uint32_t lut_roundtrips = 0u;
    for (uint32_t fmt : lut_formats) {
        XG_FORMAT_BLOCK_INFO fi{};
        OS_REQUIRE(XGGetFormatBlockInfo(fmt, &fi) == 0);
        for (XG_TILE_MODE mode : lut_modes) {
            XG_TEXTURE3D_DESC desc{};
            desc.Width = 32u;
            desc.Height = 32u;
            desc.Depth = 32u;
            desc.MipLevels = 6u;
            desc.Format = fmt;
            desc.TileMode = mode;

            XG_RESOURCE_LAYOUT layout{};
            OS_REQUIRE(XGComputeMipLayout3D(&desc, &layout) == 0);
            IXGTextureAddressComputer* comp = nullptr;
            OS_REQUIRE(XGCreateTexture3DComputer(&desc, &comp) == 0 && comp != nullptr);

            std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xA5u);
            std::vector<std::vector<uint8_t>> mip_srcs(desc.MipLevels);
            for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                const uint32_t mw = std::max(1u, desc.Width >> m);
                const uint32_t mh = std::max(1u, desc.Height >> m);
                const uint32_t md = std::max(1u, desc.Depth >> m);
                const uint32_t rp = mw * fi.bytes_per_block;
                const uint32_t sp = rp * mh;
                mip_srcs[m].resize(static_cast<size_t>(sp) * md);
                FillPattern(mip_srcs[m], 0x3D00u + fmt * 19u + static_cast<uint32_t>(mode) * 7u + m);
                OS_REQUIRE(comp->CopyIntoSubresource(tiled.data(), 0u, m, mip_srcs[m].data(), rp, sp) == 0);
            }
            for (uint32_t m = 0u; m < desc.MipLevels; ++m) {
                const uint32_t mw = std::max(1u, desc.Width >> m);
                const uint32_t mh = std::max(1u, desc.Height >> m);
                const uint32_t rp = mw * fi.bytes_per_block;
                const uint32_t sp = rp * mh;
                std::vector<uint8_t> dst(mip_srcs[m].size(), 0u);
                OS_REQUIRE(comp->CopyFromSubresource(dst.data(), 0u, m, tiled.data(), rp, sp) == 0);
                OS_REQUIRE(std::memcmp(mip_srcs[m].data(), dst.data(), dst.size()) == 0);
                ++lut_roundtrips;
            }
            comp->Release();
        }
    }

    std::printf("  Verified %u NPOT mip-chain roundtrips + %u 32x32x32 3D LUT roundtrips BIT-EXACT!\n",
                npot_roundtrips, lut_roundtrips);
}

// ===========================================================================
// 5. DirectStorage Queue Hardening Tests
// ===========================================================================
void TestDirectStorageHardening() {
    std::printf("[Test 5] dstorage_xs.dll Queue Hardening (Auto-Submit, Backpressure, Cancel)...\n");

    IDStorageFactoryX* factory = nullptr;
    OS_REQUIRE(DStorageGetFactory(&IID_IDStorageFactoryX, reinterpret_cast<void**>(&factory)) == OS_DSTORAGE_S_OK);
    OS_REQUIRE(factory != nullptr);

    // 5A. Capacity clamping (<128 -> 128, >2000 -> 2000)
    {
        DSTORAGE_QUEUE_DESC low_desc{};
        low_desc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        low_desc.Capacity = 16u;
        low_desc.Name = "clamp_low";
        IDStorageQueueX* q_low = nullptr;
        OS_REQUIRE(factory->CreateQueue(&low_desc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q_low)) == OS_DSTORAGE_S_OK);
        DSTORAGE_QUEUE_INFO qi{};
        q_low->Query(&qi);
        OS_REQUIRE(qi.Desc.Capacity == DSTORAGE_MIN_QUEUE_CAPACITY);
        OS_REQUIRE(qi.EmptySlotCount == DSTORAGE_MIN_QUEUE_CAPACITY);
        OS_REQUIRE(qi.RequestCountUntilAutoSubmit == DSTORAGE_MIN_QUEUE_CAPACITY / 2u);
        q_low->Close();
        q_low->Release();

        DSTORAGE_QUEUE_DESC hi_desc{};
        hi_desc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        hi_desc.Capacity = 9999u;
        hi_desc.Name = "clamp_hi";
        IDStorageQueueX* q_hi = nullptr;
        OS_REQUIRE(factory->CreateQueue(&hi_desc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q_hi)) == OS_DSTORAGE_S_OK);
        q_hi->Query(&qi);
        OS_REQUIRE(qi.Desc.Capacity == DSTORAGE_MAX_QUEUE_CAPACITY);
        OS_REQUIRE(qi.EmptySlotCount == DSTORAGE_MAX_QUEUE_CAPACITY);
        OS_REQUIRE(qi.RequestCountUntilAutoSubmit == DSTORAGE_MAX_QUEUE_CAPACITY / 2u);
        q_hi->Close();
        q_hi->Release();
        std::printf("  Capacity clamping : 16 -> %u, 9999 -> %u OK\n",
                    DSTORAGE_MIN_QUEUE_CAPACITY, DSTORAGE_MAX_QUEUE_CAPACITY);
    }

    // 5B. Auto-submit half-full queue WITHOUT calling Submit()
    {
        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;  // 128 -> threshold = 64
        qdesc.Name = "auto_submit_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);

        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(2u, "as_status", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

        constexpr uint32_t kThreshold = DSTORAGE_MIN_QUEUE_CAPACITY / 2u;  // 64
        std::vector<std::array<uint8_t, 64>> src_bufs(kThreshold);
        std::vector<std::array<uint8_t, 64>> dst_bufs(kThreshold);
        for (uint32_t i = 0u; i < kThreshold; ++i) {
            for (size_t k = 0u; k < 64u; ++k) {
                src_bufs[i][k] = static_cast<uint8_t>((i * 13u + k * 7u + 3u) & 0xFFu);
            }
            dst_bufs[i].fill(0u);
        }

        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);

        // Enqueue 63 requests + status + event (still 1 short of half-full auto-submit!)
        for (uint32_t i = 0u; i < kThreshold - 1u; ++i) {
            DSTORAGE_REQUEST r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = src_bufs[i].data();
            r.Source.Memory.Size = 64u;
            r.Destination.Memory.Buffer = dst_bufs[i].data();
            r.Destination.Memory.Size = 64u;
            r.UncompressedSize = 64u;
            q->EnqueueRequest(&r);
        }
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);

        DSTORAGE_QUEUE_INFO qi{};
        q->Query(&qi);
        OS_REQUIRE(qi.RequestCountUntilAutoSubmit == 1u);
        OS_REQUIRE(WaitForSingleObject(ev, 20u) == WAIT_TIMEOUT);

        // Enqueue 64th request WITHOUT calling Submit() -> triggers automatic flush!
        {
            DSTORAGE_REQUEST r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = src_bufs[kThreshold - 1u].data();
            r.Source.Memory.Size = 64u;
            r.Destination.Memory.Buffer = dst_bufs[kThreshold - 1u].data();
            r.Destination.Memory.Size = 64u;
            r.UncompressedSize = 64u;
            q->EnqueueRequest(&r);
        }

        // Wait on event (no Submit() was ever called!)
        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);

        // Wait briefly for the 64th request (queued right after ev) to finish draining
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            q->Query(&qi);
            if (qi.EmptySlotCount == DSTORAGE_MIN_QUEUE_CAPACITY) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        OS_REQUIRE(qi.RequestCountUntilAutoSubmit == kThreshold);

        for (uint32_t i = 0u; i < kThreshold - 1u; ++i) {
            OS_REQUIRE(dst_bufs[i] == src_bufs[i]);
        }

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        std::printf("  Half-full auto-submit: 64 requests flushed automatically without Submit() OK\n");
    }

    // 5C. Capacity Backpressure under 4x burst load (512 items into 128-slot queue, no drops/deadlock)
    {
        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;  // 128
        qdesc.Name = "burst_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);

        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(1u, "burst_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

        constexpr uint32_t kBurst = 512u;
        std::vector<std::array<uint8_t, 128>> src_bufs(kBurst);
        std::vector<std::array<uint8_t, 128>> dst_bufs(kBurst);
        for (uint32_t i = 0u; i < kBurst; ++i) {
            for (size_t k = 0u; k < 128u; ++k) {
                src_bufs[i][k] = static_cast<uint8_t>(((i * 37u) ^ (k * 11u) ^ 0x5Au) & 0xFFu);
            }
            dst_bufs[i].fill(0u);
        }

        for (uint32_t i = 0u; i < kBurst; ++i) {
            DSTORAGE_REQUEST r{};
            r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
            r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            r.Source.Memory.Source = src_bufs[i].data();
            r.Source.Memory.Size = 128u;
            r.Destination.Memory.Buffer = dst_bufs[i].data();
            r.Destination.Memory.Size = 128u;
            r.UncompressedSize = 128u;
            q->EnqueueRequest(&r);

            if ((i & 31u) == 0u) {
                DSTORAGE_QUEUE_INFO qi{};
                q->Query(&qi);
                OS_REQUIRE(qi.EmptySlotCount <= DSTORAGE_MIN_QUEUE_CAPACITY);
            }
        }

        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);

        for (uint32_t i = 0u; i < kBurst; ++i) {
            OS_REQUIRE(dst_bufs[i] == src_bufs[i]);
        }

        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 0u);

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        std::printf("  Capacity backpressure: %u/128 burst completed bit-exact with 0 drops & 0 deadlocks\n", kBurst);
    }

    // 5D. Cancelled failing requests excluded from error counters, error_event & batch_hr
    {
        DSTORAGE_QUEUE_DESC qdesc{};
        qdesc.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        qdesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        qdesc.Name = "cancel_q";

        IDStorageQueueX* q = nullptr;
        OS_REQUIRE(factory->CreateQueue(&qdesc, &IID_IDStorageQueueX, reinterpret_cast<void**>(&q)) == OS_DSTORAGE_S_OK);

        IDStorageStatusArrayX* st = nullptr;
        OS_REQUIRE(factory->CreateStatusArray(2u, "cancel_st", &IID_IDStorageStatusArrayX,
                                              reinterpret_cast<void**>(&st)) == OS_DSTORAGE_S_OK);

        HANDLE err_ev = static_cast<HANDLE>(q->GetErrorEvent());
        OS_REQUIRE(err_ev != nullptr);
        OS_REQUIRE(WaitForSingleObject(err_ev, 0u) == WAIT_TIMEOUT);

        std::array<uint8_t, 32> good_src{};
        std::array<uint8_t, 32> good_dst{};
        for (size_t i = 0u; i < good_src.size(); ++i) good_src[i] = static_cast<uint8_t>(i + 1u);
        good_dst.fill(0u);

        const uint8_t bad_gdef[16] = {0xFF, 0xEE, 0xDD, 0xCC, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        std::array<uint8_t, 32> unused_dst{};

        // Enqueue 1 valid request
        DSTORAGE_REQUEST r_ok{};
        r_ok.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r_ok.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_ok.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_ok.Source.Memory.Source = good_src.data();
        r_ok.Source.Memory.Size = 32u;
        r_ok.Destination.Memory.Buffer = good_dst.data();
        r_ok.Destination.Memory.Size = 32u;
        r_ok.UncompressedSize = 32u;
        r_ok.CancellationTag = 0x11110001ull;
        q->EnqueueRequest(&r_ok);

        // Enqueue 3 malformed / guaranteed-to-fail requests tagged 0xCAFE00xx
        DSTORAGE_REQUEST r_bad1{};
        r_bad1.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        r_bad1.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_bad1.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_bad1.Source.Memory.Source = nullptr;
        r_bad1.Source.Memory.Size = 32u;
        r_bad1.Destination.Memory.Buffer = nullptr;
        r_bad1.Destination.Memory.Size = 0u;
        r_bad1.CancellationTag = 0xCAFE0001ull;
        q->EnqueueRequest(&r_bad1);

        DSTORAGE_REQUEST r_bad2{};
        r_bad2.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
        r_bad2.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r_bad2.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        r_bad2.Source.Memory.Source = bad_gdef;
        r_bad2.Source.Memory.Size = sizeof(bad_gdef);
        r_bad2.Destination.Memory.Buffer = unused_dst.data();
        r_bad2.Destination.Memory.Size = 32u;
        r_bad2.UncompressedSize = 32u;
        r_bad2.CancellationTag = 0xCAFE0002ull;
        q->EnqueueRequest(&r_bad2);

        DSTORAGE_REQUEST r_bad3 = r_bad2;
        r_bad3.CancellationTag = 0xCAFE00FFull;
        q->EnqueueRequest(&r_bad3);

        // Cancel all 0xCAFE____ requests BEFORE Submit()
        q->CancelRequestsWithTag(0xFFFF0000ull, 0xCAFE0000ull);

        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);
        q->EnqueueStatus(st, 0u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(good_dst == good_src);
        OS_REQUIRE(st->IsComplete(0u) && st->GetHResult(0u) == OS_DSTORAGE_S_OK);
        OS_REQUIRE(WaitForSingleObject(err_ev, 0u) == WAIT_TIMEOUT);

        DSTORAGE_ERROR_RECORD err{};
        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 0u);

        // Contrast: enqueue 1 UN-cancelled failing request and verify error reporting still works!
        ResetEvent(ev);
        r_bad2.CancellationTag = 0x22220001ull;
        q->EnqueueRequest(&r_bad2);
        q->EnqueueStatus(st, 1u);
        q->EnqueueSetEvent(ev);
        q->Submit();

        OS_REQUIRE(WaitForSingleObject(ev, 5000u) == WAIT_OBJECT_0);
        OS_REQUIRE(st->IsComplete(1u) && st->GetHResult(1u) == OS_DSTORAGE_E_DECOMPRESSION_ERROR);
        OS_REQUIRE(WaitForSingleObject(err_ev, 1000u) == WAIT_OBJECT_0);

        q->RetrieveErrorRecord(&err);
        OS_REQUIRE(err.FailureCount == 1u);
        OS_REQUIRE(err.FirstFailure.HResult == OS_DSTORAGE_E_DECOMPRESSION_ERROR);

        CloseHandle(ev);
        st->Release();
        q->Close();
        q->Release();
        std::printf("  Cancelled failures: FailureCount=0, error_event unsignaled, status=S_OK (uncancelled=1 OK)\n");
    }

    factory->Release();
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    TestXgAll2dModes();
    TestXgAll3dModesAndVolumes();
    TestXgMipTailsArraysAndCubes();
    TestXgAllFormatsAndPlanarDepthStencil();
    TestXgNpotMipChainsAnd3dLut();
    TestDirectStorageHardening();
    std::printf("ALL XG_XS GFX10 (2D/3D/MIP-TAIL/FORMATS) & DIRECTSTORAGE HARDENING TESTS PASSED!\n");
    return 0;
#else
    std::printf("Skipping Windows XG/DirectStorage test on non-Windows host.\n");
    return 0;
#endif
}
