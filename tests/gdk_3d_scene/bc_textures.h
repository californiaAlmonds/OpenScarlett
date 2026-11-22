#pragma once

// OpenScarlett MIT Procedural BC1 (DXT1) & BC3 (DXT5) Block Compression Encoder.
// Generates genuine 4x4 block-compressed textures in memory for direct D3D12X upload & sampling.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gdk3d {

struct BcTextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t block_bytes = 0;   // 8 for BC1, 16 for BC3
    uint32_t row_pitch = 0;     // Bytes per 4-pixel-tall block row
    std::vector<uint8_t> bytes;
};

namespace detail {

inline uint16_t PackRgb565(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return static_cast<uint16_t>(
        ((static_cast<uint32_t>(r) >> 3) << 11) |
        ((static_cast<uint32_t>(g) >> 2) << 5) |
        (static_cast<uint32_t>(b) >> 3));
}

inline void UnpackRgb565(uint16_t c, int& r, int& g, int& b) noexcept {
    const int r5 = (c >> 11) & 0x1F;
    const int g6 = (c >> 5) & 0x3F;
    const int b5 = c & 0x1F;
    r = (r5 << 3) | (r5 >> 2);
    g = (g6 << 2) | (g6 >> 4);
    b = (b5 << 3) | (b5 >> 2);
}

inline void EncodeBc1ColorBlock(const uint8_t block_rgba[64], uint8_t out_8[8]) noexcept {
    int min_r = 255, min_g = 255, min_b = 255;
    int max_r = 0, max_g = 0, max_b = 0;
    for (int i = 0; i < 16; ++i) {
        const int r = block_rgba[i * 4 + 0];
        const int g = block_rgba[i * 4 + 1];
        const int b = block_rgba[i * 4 + 2];
        if (r < min_r) min_r = r;
        if (g < min_g) min_g = g;
        if (b < min_b) min_b = b;
        if (r > max_r) max_r = r;
        if (g > max_g) max_g = g;
        if (b > max_b) max_b = b;
    }

    // Mild bounding-box inset improves perceptual fidelity on smooth gradients.
    const int inset_r = (max_r - min_r) >> 4;
    const int inset_g = (max_g - min_g) >> 4;
    const int inset_b = (max_b - min_b) >> 4;
    min_r = std::min(255, min_r + inset_r);
    min_g = std::min(255, min_g + inset_g);
    min_b = std::min(255, min_b + inset_b);
    max_r = std::max(0, max_r - inset_r);
    max_g = std::max(0, max_g - inset_g);
    max_b = std::max(0, max_b - inset_b);

    uint16_t c0 = PackRgb565(static_cast<uint8_t>(max_r),
                             static_cast<uint8_t>(max_g),
                             static_cast<uint8_t>(max_b));
    uint16_t c1 = PackRgb565(static_cast<uint8_t>(min_r),
                             static_cast<uint8_t>(min_g),
                             static_cast<uint8_t>(min_b));
    if (c0 < c1) {
        std::swap(c0, c1);
    }

    int pal[4][3]{};
    UnpackRgb565(c0, pal[0][0], pal[0][1], pal[0][2]);
    UnpackRgb565(c1, pal[1][0], pal[1][1], pal[1][2]);
    for (int ch = 0; ch < 3; ++ch) {
        pal[2][ch] = (2 * pal[0][ch] + pal[1][ch] + 1) / 3;
        pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch] + 1) / 3;
    }

    uint32_t indices = 0u;
    if (c0 != c1) {
        for (int i = 0; i < 16; ++i) {
            const int r = block_rgba[i * 4 + 0];
            const int g = block_rgba[i * 4 + 1];
            const int b = block_rgba[i * 4 + 2];
            uint32_t best_idx = 0u;
            int best_dist = 0x7FFFFFFF;
            for (uint32_t k = 0; k < 4u; ++k) {
                const int dr = r - pal[k][0];
                const int dg = g - pal[k][1];
                const int db = b - pal[k][2];
                const int dist = dr * dr + dg * dg + db * db;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = k;
                }
            }
            indices |= (best_idx << (i * 2));
        }
    }

    out_8[0] = static_cast<uint8_t>(c0 & 0xFFu);
    out_8[1] = static_cast<uint8_t>((c0 >> 8) & 0xFFu);
    out_8[2] = static_cast<uint8_t>(c1 & 0xFFu);
    out_8[3] = static_cast<uint8_t>((c1 >> 8) & 0xFFu);
    out_8[4] = static_cast<uint8_t>(indices & 0xFFu);
    out_8[5] = static_cast<uint8_t>((indices >> 8) & 0xFFu);
    out_8[6] = static_cast<uint8_t>((indices >> 16) & 0xFFu);
    out_8[7] = static_cast<uint8_t>((indices >> 24) & 0xFFu);
}

inline void EncodeBc3AlphaBlock(const uint8_t block_rgba[64], uint8_t out_8[8]) noexcept {
    uint8_t min_a = 255u;
    uint8_t max_a = 0u;
    for (int i = 0; i < 16; ++i) {
        const uint8_t a = block_rgba[i * 4 + 3];
        if (a < min_a) min_a = a;
        if (a > max_a) max_a = a;
    }

    const uint8_t a0 = max_a;
    const uint8_t a1 = min_a;
    out_8[0] = a0;
    out_8[1] = a1;

    uint64_t idx_bits = 0ull;
    if (a0 > a1) {
        int pal[8]{};
        pal[0] = a0;
        pal[1] = a1;
        for (int k = 2; k < 8; ++k) {
            pal[k] = ((8 - k) * static_cast<int>(a0) + (k - 1) * static_cast<int>(a1) + 3) / 7;
        }
        for (int i = 0; i < 16; ++i) {
            const int a = block_rgba[i * 4 + 3];
            uint64_t best_k = 0ull;
            int best_err = 0x7FFFFFFF;
            for (int k = 0; k < 8; ++k) {
                const int err = std::abs(a - pal[k]);
                if (err < best_err) {
                    best_err = err;
                    best_k = static_cast<uint64_t>(k);
                }
            }
            idx_bits |= (best_k << (i * 3));
        }
    }

    for (int b = 0; b < 6; ++b) {
        out_8[2 + b] = static_cast<uint8_t>((idx_bits >> (b * 8)) & 0xFFull);
    }
}

}  // namespace detail

inline BcTextureData CompressRgbaToBc1(const std::vector<uint8_t>& rgba,
                                       uint32_t width,
                                       uint32_t height) {
    BcTextureData out{};
    out.width = width;
    out.height = height;
    out.block_bytes = 8u;
    const uint32_t blocks_x = width / 4u;
    const uint32_t blocks_y = height / 4u;
    out.row_pitch = blocks_x * out.block_bytes;
    out.bytes.resize(static_cast<size_t>(out.row_pitch) * blocks_y);

    uint8_t blk[64]{};
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            for (uint32_t py = 0; py < 4u; ++py) {
                for (uint32_t px = 0; px < 4u; ++px) {
                    const uint32_t x = bx * 4u + px;
                    const uint32_t y = by * 4u + py;
                    const size_t src = (static_cast<size_t>(y) * width + x) * 4u;
                    const size_t dst = (static_cast<size_t>(py) * 4u + px) * 4u;
                    blk[dst + 0] = rgba[src + 0];
                    blk[dst + 1] = rgba[src + 1];
                    blk[dst + 2] = rgba[src + 2];
                    blk[dst + 3] = 255u;
                }
            }
            const size_t out_off = static_cast<size_t>(by) * out.row_pitch +
                                   static_cast<size_t>(bx) * 8u;
            detail::EncodeBc1ColorBlock(blk, out.bytes.data() + out_off);
        }
    }
    return out;
}

inline BcTextureData CompressRgbaToBc3(const std::vector<uint8_t>& rgba,
                                       uint32_t width,
                                       uint32_t height) {
    BcTextureData out{};
    out.width = width;
    out.height = height;
    out.block_bytes = 16u;
    const uint32_t blocks_x = width / 4u;
    const uint32_t blocks_y = height / 4u;
    out.row_pitch = blocks_x * out.block_bytes;
    out.bytes.resize(static_cast<size_t>(out.row_pitch) * blocks_y);

    uint8_t blk[64]{};
    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            for (uint32_t py = 0; py < 4u; ++py) {
                for (uint32_t px = 0; px < 4u; ++px) {
                    const uint32_t x = bx * 4u + px;
                    const uint32_t y = by * 4u + py;
                    const size_t src = (static_cast<size_t>(y) * width + x) * 4u;
                    const size_t dst = (static_cast<size_t>(py) * 4u + px) * 4u;
                    blk[dst + 0] = rgba[src + 0];
                    blk[dst + 1] = rgba[src + 1];
                    blk[dst + 2] = rgba[src + 2];
                    blk[dst + 3] = rgba[src + 3];
                }
            }
            const size_t out_off = static_cast<size_t>(by) * out.row_pitch +
                                   static_cast<size_t>(bx) * 16u;
            detail::EncodeBc3AlphaBlock(blk, out.bytes.data() + out_off);
            detail::EncodeBc1ColorBlock(blk, out.bytes.data() + out_off + 8u);
        }
    }
    return out;
}

// Procedural 256x256 stone-tile checkerboard albedo encoded as genuine BC1 (DXT1).
inline BcTextureData GenerateStoneFloorBc1Texture(uint32_t size = 256u) {
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4u, 255u);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t tile_x = (x * 4u) / size;
            const uint32_t tile_y = (y * 4u) / size;
            const bool light_tile = (((tile_x ^ tile_y) & 1u) == 0u);

            const uint32_t local_x = x % (size / 4u);
            const uint32_t local_y = y % (size / 4u);
            const uint32_t tile_dim = size / 4u;
            const bool grout = (local_x < 3u || local_y < 3u ||
                                local_x >= tile_dim - 2u || local_y >= tile_dim - 2u);
            const bool inlay = (local_x >= 10u && local_x <= 12u && local_y >= 10u && local_y <= tile_dim - 11u) ||
                               (local_y >= 10u && local_y <= 12u && local_x >= 10u && local_x <= tile_dim - 11u);

            // Deterministic stone micro-grain hash
            const uint32_t h = (x * 374761393u + y * 668265263u) ^ (x * 1274126177u);
            const int grain = static_cast<int>((h >> 16) & 31u) - 16;

            int r = 0, g = 0, b = 0;
            if (grout) {
                r = 56 + grain / 3;
                g = 62 + grain / 3;
                b = 72 + grain / 3;
            } else if (inlay) {
                r = 188 + grain / 2;
                g = 156 + grain / 2;
                b = 96 + grain / 2;
            } else if (light_tile) {
                // Warm architectural sandstone
                r = 196 + grain;
                g = 180 + grain;
                b = 152 + grain;
            } else {
                // Cool slate-teal stone
                r = 88 + grain;
                g = 114 + grain;
                b = 136 + grain;
            }

            const size_t idx = (static_cast<size_t>(y) * size + x) * 4u;
            rgba[idx + 0] = static_cast<uint8_t>(std::clamp(r, 12, 255));
            rgba[idx + 1] = static_cast<uint8_t>(std::clamp(g, 12, 255));
            rgba[idx + 2] = static_cast<uint8_t>(std::clamp(b, 12, 255));
            rgba[idx + 3] = 255u;
        }
    }
    return CompressRgbaToBc1(rgba, size, size);
}

// Procedural 256x256 brushed-metal + Scarlett emissive trim encoded as genuine BC3 (DXT5).
// RGB = brushed metallic albedo with tech-circuit trims; Alpha = emissive/specular mask.
inline BcTextureData GenerateTechMetalBc3Texture(uint32_t size = 256u) {
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4u, 255u);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);

            const bool border = (x < 8u || y < 8u || x >= size - 8u || y >= size - 8u);
            const bool center_band = (y >= size / 2u - 10u && y <= size / 2u + 10u);
            const bool diag_trim = (std::abs(static_cast<int>(x) - static_cast<int>(y)) < 8) ||
                                   (std::abs(static_cast<int>(x) + static_cast<int>(y) - static_cast<int>(size)) < 8);
            const int brushed = static_cast<int>(std::sin(v * 90.0f + u * 6.0f) * 18.0f);

            int r = 132 + brushed;
            int g = 148 + brushed;
            int b = 172 + brushed;
            int a = 48;  // Moderate specular, low emissive

            if (border) {
                r = 46;
                g = 56;
                b = 72;
                a = 20;
            } else if (center_band) {
                // Bright Scarlett emerald-cyan emissive strip
                r = 42;
                g = 236;
                b = 176;
                a = 240;
            } else if (diag_trim) {
                // Warm amber-gold metallic accent
                r = 238;
                g = 178;
                b = 64;
                a = 185;
            } else if (((x >> 5) ^ (y >> 5)) & 1u) {
                r = 162 + brushed / 2;
                g = 176 + brushed / 2;
                b = 198 + brushed / 2;
                a = 90;
            }

            const size_t idx = (static_cast<size_t>(y) * size + x) * 4u;
            rgba[idx + 0] = static_cast<uint8_t>(std::clamp(r, 8, 255));
            rgba[idx + 1] = static_cast<uint8_t>(std::clamp(g, 8, 255));
            rgba[idx + 2] = static_cast<uint8_t>(std::clamp(b, 8, 255));
            rgba[idx + 3] = static_cast<uint8_t>(std::clamp(a, 0, 255));
        }
    }
    return CompressRgbaToBc3(rgba, size, size);
}

}  // namespace gdk3d
