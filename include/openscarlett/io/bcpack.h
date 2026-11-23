#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

static constexpr uint32_t kBcPackMagic = 0x4B504342u;          // 'BCPK' little-endian
static constexpr uint32_t kBcPackMipChainMagic = 0x4D504342u;  // 'BCPM' little-endian
static constexpr uint16_t kBcPackVersion = 1u;
static constexpr uint32_t kBcPackMaxDimension = 16384u;
static constexpr uint32_t kBcPackMaxPlanes = 8u;
static constexpr uint32_t kBcPackMaxMipLevels = 15u;
static constexpr uint32_t kBcPackEndpointRingCapacity = 64u;
static constexpr uint32_t kBcPackMaxSelectorTableEntries = 256u;

enum class BcFormat : uint32_t {
    Unknown = 0,
    BC1 = 1,   // 8 bytes / 4x4 block (RGB565 endpoints + 16x2-bit indices)
    BC2 = 2,   // 16 bytes / 4x4 block (explicit 4-bit alpha + BC1 color)
    BC3 = 3,   // 16 bytes / 4x4 block (interpolated alpha + BC1 color)
    BC4 = 4,   // 8 bytes / 4x4 block (single-channel interpolated endpoints + 16x3-bit indices)
    BC5 = 5,   // 16 bytes / 4x4 block (two BC4 channels)
    BC6H = 6,  // 16 bytes / 4x4 block (HDR half-float block)
    BC7 = 7,   // 16 bytes / 4x4 block (high-precision RGBA block modes 0..7)
};

enum class BcPlaneKind : uint8_t {
    Raw = 0,
    Color0 = 1,
    Color1 = 2,
    ColorIndices = 3,
    Alpha0 = 4,
    Alpha1 = 5,
    AlphaIndices = 6,
    AlphaExplicit = 7,
    Red0 = 8,
    Red1 = 9,
    RedIndices = 10,
    Green0 = 11,
    Green1 = 12,
    GreenIndices = 13,
    ModeHeader = 14,
    Endpoints = 15,
    SecondaryEndpoints = 16,
    PrimaryIndices = 17,
};

enum class BcPlaneTransform : uint8_t {
    None = 0,
    Delta8 = 1,
    Xor8 = 2,
    Delta16 = 3,
    Rgb565Delta = 4,
    TransposeDelta8 = 5,
};

enum class BcPlaneCodec : uint8_t {
    Raw = 0,
    LzRle = 1,
    CanonicalHuffman = 2,
    LzRleHuffman = 3,
    EndpointRingWindow = 4,
    SelectorIndexTable = 5,
};

struct BcPackPlaneDescriptor {
    BcPlaneKind kind = BcPlaneKind::Raw;
    BcPlaneTransform transform = BcPlaneTransform::None;
    BcPlaneCodec codec = BcPlaneCodec::Raw;
    uint8_t element_bytes = 1;
    uint32_t raw_size = 0;
    uint32_t compressed_size = 0;
    uint32_t data_offset = 0;
    uint32_t plane_crc32 = 0;
};

struct BcPackHeader {
    uint32_t magic = kBcPackMagic;
    uint16_t version = kBcPackVersion;
    uint16_t flags = 0;
    BcFormat format = BcFormat::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t block_count = 0;
    uint32_t original_size = 0;
    uint32_t plane_count = 0;
    uint32_t uncompressed_crc32 = 0;
    std::vector<BcPackPlaneDescriptor> planes;
};

struct BcPackStats {
    uint32_t original_size = 0;
    uint32_t compressed_size = 0;
    uint32_t block_count = 0;
    uint32_t plane_count = 0;
    double compression_ratio = 1.0;  // compressed_size / original_size
};

struct BcPackCompressOptions {
    bool force_transform = false;
    BcPlaneTransform forced_transform = BcPlaneTransform::None;
    bool force_codec = false;
    BcPlaneCodec forced_codec = BcPlaneCodec::Raw;
    bool enable_specialized_codecs = true;
};

struct BcPackMipLevelInfo {
    uint32_t mip_level = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t block_width = 0;
    uint32_t block_height = 0;
    uint32_t block_count = 0;
    uint64_t byte_size = 0;
};

struct BcPackMipChainHeader {
    uint32_t magic = kBcPackMipChainMagic;
    uint16_t version = kBcPackVersion;
    uint16_t flags = 0;
    BcFormat format = BcFormat::Unknown;
    uint32_t base_width = 0;
    uint32_t base_height = 0;
    uint32_t mip_count = 0;
    std::vector<uint32_t> mip_offsets;
    std::vector<uint32_t> mip_compressed_sizes;
};

// Returns exact byte size of a single 4x4 block for the given BC format (0 if invalid).
uint32_t BcFormatBlockBytes(BcFormat format) noexcept;

// Computes expected uncompressed byte size for a BC texture surface (0 on invalid/overflow).
uint64_t BcCalculateSurfaceSize(BcFormat format, uint32_t width, uint32_t height) noexcept;

// Computes maximum valid mip count (down to 1x1) for a surface (0 if dimensions invalid).
uint32_t BcCalculateMaxMipLevels(uint32_t base_width, uint32_t base_height) noexcept;

// Computes block dimensions and byte size for a specific mip level (handles NPOT and < 4x4 mips).
bool BcCalculateMipLevelInfo(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    uint32_t mip_level,
    BcPackMipLevelInfo& out_info) noexcept;

// Computes IEEE 802.3 CRC32 over a byte range.
uint32_t BcPackCrc32(const uint8_t* data, size_t size) noexcept;

// Parses and validates a BCPack container header and its plane descriptor table.
Status BcPackParseHeader(const uint8_t* src, size_t src_size, BcPackHeader& out_header);

// Compresses a BC1/BC2/BC3/BC4/BC5/BC6H/BC7 GPU texture surface losslessly using
// BC-block plane separation, spatial delta/XOR decorrelation, and entropy coding.
Status BcPackCompress(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats = nullptr);

// Compresses a BC surface with explicit codec/transform options (for deterministic testing).
Status BcPackCompressWithOptions(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    const uint8_t* src_blocks,
    size_t src_size,
    const BcPackCompressOptions& options,
    std::vector<uint8_t>& out_compressed,
    BcPackStats* out_stats = nullptr);

// Decompresses a BCPack stream back to the exact original BC texture surface bytes,
// verifying header consistency, plane CRCs, codec bounds, and final uncompressed CRC32.
Status BcPackDecompress(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_blocks,
    BcPackHeader* out_header = nullptr);

// Decompresses a BCPack stream into a caller-provided fixed buffer, strictly rejecting
// undersized output buffers before writing.
Status BcPackDecompressInto(
    const uint8_t* src,
    size_t src_size,
    uint8_t* dst_blocks,
    size_t dst_capacity,
    size_t* out_written = nullptr,
    BcPackHeader* out_header = nullptr);

// Multi-mip BCPack container helpers (supports NPOT dimensions and sub-4x4 tail mips).
Status BcPackCompressMipChain(
    BcFormat format,
    uint32_t base_width,
    uint32_t base_height,
    const std::vector<std::vector<uint8_t>>& mip_surfaces,
    std::vector<uint8_t>& out_compressed);

Status BcPackParseMipChainHeader(
    const uint8_t* src,
    size_t src_size,
    BcPackMipChainHeader& out_header);

Status BcPackDecompressMipChain(
    const uint8_t* src,
    size_t src_size,
    std::vector<std::vector<uint8_t>>& out_mip_surfaces,
    BcPackMipChainHeader* out_header = nullptr);

// Generates a deterministic synthetic BC1..BC7 surface for round-trip testing.
Status BcGenerateSyntheticSurface(
    BcFormat format,
    uint32_t width,
    uint32_t height,
    uint32_t seed,
    std::vector<uint8_t>& out_blocks);

// ---------------------------------------------------------------------------
// Public Xbox Hardware BCPack Raw Bitstream Codec (BC1..BC7)
// Used by DirectStorage Xbox DSTORAGE_REQUEST::Options.BcpackMode.
// Implements bit-level entropy coding with explicit flush bit, per-format
// BC1-BC7 block predictors, spatial delta coding, and endpoint/selector palettes.
// ---------------------------------------------------------------------------
Status BcPackHwEncodeBitstream(
    BcFormat format,
    const uint8_t* src_blocks,
    size_t src_size,
    std::vector<uint8_t>& out_bitstream,
    uint32_t tile_width_blocks = 0u);

Status BcPackHwDecodeBitstream(
    BcFormat format,
    const uint8_t* src_bitstream,
    size_t src_size,
    uint8_t* dst_blocks,
    size_t dst_size,
    size_t* out_written = nullptr);

bool BcPackHwIsValidBitstreamHeader(
    BcFormat expected_format,
    const uint8_t* src_bitstream,
    size_t src_size,
    uint32_t* out_block_count = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Sprint 15: Spec-Correct Bit-Exact BC6H Decoder (UF16/SF16), Signed-Zero Rule,
// BC6H BCPack Preset/Region Transition Helpers, 64-bit TTPF Bounds & rANS Codec
// (SixNG #15/#20/#21/#23/#24, bcpack-rs c51ec52)
// ---------------------------------------------------------------------------
struct Bc6hBlockInfo {
    uint8_t mode_index = 0xFFu;      // 0..13 for valid BC6H modes, 0xFF for reserved
    uint8_t region_count = 0u;       // 1 (modes 10..13) or 2 (modes 0..9), 0 if reserved
    uint8_t partition_index = 0u;    // 0..31 for 2-region modes, 0 for 1-region
    uint8_t endpoint_bits = 0u;      // Endpoint quantized bit precision (6..16)
    bool is_transformed = false;     // True for delta-encoded endpoints
    bool is_reserved_mode = true;
};

struct Bc6hBcpPresetInfo {
    uint32_t preset_index = 0u;      // Canonical preset table index [0..14] (14 = reserved/raw)
    uint8_t mode_index = 0xFFu;      // 0..13 or 0xFF
    uint8_t region_count = 0u;       // 1 or 2 (0 if reserved)
    bool is_valid = false;
};

// Exact DXGI BC6H FinishUnquantize integer final-scaling rule:
//   UF16: (comp * 31) >> 6
//   SF16: scale signed comp by 31/32, THEN select signed-zero sign bit AFTER final scaling
//         (so small negative values scaling to 0 emit +0.0 == 0x0000, NEVER -0.0 == 0x8000).
uint16_t Bc6hFinishUnquantize(int32_t comp, bool is_signed) noexcept;

// Inspects a 16-byte BC6H block's mode prefix, region count, partition shape, and transform flag.
bool Bc6hInspectBlockMode(const uint8_t block[16], Bc6hBlockInfo& out_info) noexcept;

// Resolves canonical BCPack BC6H preset metadata (handling 1-region <-> 2-region transitions safely).
bool Bc6hResolveBcpPreset(const uint8_t block[16], Bc6hBcpPresetInfo& out_preset) noexcept;

// Decodes a single 16-byte BC6H block (UF16 or SF16) using strict bit-exact integer
// unquantization, weight interpolation, and post-scaling signed-zero selection into
// 16 RGB half-floats (48 IEEE-754 binary16 uint16_t values, row-major 4x4).
void Bc6hDecodeBlock(
    const uint8_t block[16],
    bool is_signed,
    uint16_t out_rgb_f16[48]) noexcept;

// Decodes an entire BC6H surface into RGBA16F (width * height * 4 uint16_t values, A = 0x3C00).
Status Bc6hDecodeSurface(
    const uint8_t* blocks,
    size_t blocks_size,
    uint32_t width,
    uint32_t height,
    bool is_signed,
    std::vector<uint16_t>& out_rgba_f16);

// 64-bit TTPF (Tile/Texture Payload Format) codec-mask & >4 GiB bounds helpers (bcpack-rs c51ec52).
static constexpr uint32_t kTtpfMagic = 0x46505454u;  // 'TTPF'
static constexpr uint32_t kTtpfMaxCodecBits = 64u;

struct TtpfCodecMaskInfo {
    uint64_t codec_mask = 0ull;
    uint64_t logical_size_bytes = 0ull;
    uint64_t tile_offset_bytes = 0ull;
    uint64_t tile_size_bytes = 0ull;
    uint32_t active_codec_count = 0u;
    bool exceeds_4gib = false;
};

// Computes 64-bit codec mask bit safely without 32-bit shift UB/truncation (returns 0 if codec_index >= 64).
uint64_t TtpfComputeCodecBitMask(uint32_t codec_index) noexcept;

// Validates 64-bit TTPF payload/tile ranges and codec bitmasks safely above 4 GiB.
bool TtpfValidatePayloadBounds(
    uint64_t logical_size_bytes,
    uint64_t tile_offset_bytes,
    uint64_t tile_size_bytes,
    uint64_t codec_mask,
    TtpfCodecMaskInfo* out_info = nullptr) noexcept;

// Strict bounds-checked 32-bit rANS entropy codec (12-bit probability scale, 0 OOB reads on malformed input).
Status BcPackRansEncode(
    const uint8_t* src,
    size_t src_size,
    std::vector<uint8_t>& out_encoded);

Status BcPackRansDecode(
    const uint8_t* src,
    size_t src_size,
    uint64_t expected_output_size,
    std::vector<uint8_t>& out_decoded);

}  // namespace openscarlett
