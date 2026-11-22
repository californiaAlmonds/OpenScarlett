#pragma once

// Opus (0x704F via native Windows MSOpusDecoder.dll MFT)
// and IMA-ADPCM (0x0011 WAV / 0x0069 Xbox ADPCM) audio decoders for OpenScarlett.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"

namespace openscarlett {

// ===========================================================================
// 1. IMA-ADPCM Codec (0x0011 Microsoft WAV & 0x0069 Xbox ADPCM)
// ===========================================================================

struct ImaAdpcmConfig {
    uint32_t sample_rate = 48000u;
    uint16_t channels = 1u;             // 1 (mono) or 2 (stereo)
    uint16_t block_align = 0u;          // 0 => default (256*ch for 0x0011, 36*ch for 0x0069)
    uint16_t samples_per_block = 0u;    // 0 => derived from block_align
    bool xbox_adpcm = false;            // true for wFormatTag == 0x0069
};

struct ImaAdpcmDecodeStats {
    uint64_t blocks_decoded = 0;
    uint64_t pcm_frames_output = 0;
};

// Computes default samples-per-channel per block for Microsoft WAV / Xbox IMA-ADPCM.
uint16_t ImaAdpcmComputeSamplesPerBlock(uint16_t channels, uint16_t block_align) noexcept;

// IMA-ADPCM encoder (mono or stereo, interleaved float32 PCM in [-1, 1]).
// Encodes into standard 4-byte-per-channel header + 4-byte-per-channel 8-sample chunk blocks.
Status ImaAdpcmEncodePcmToBlocks(
    const float* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const ImaAdpcmConfig& config,
    std::vector<uint8_t>& out_blocks,
    uint16_t* out_block_align = nullptr,
    uint16_t* out_samples_per_block = nullptr);

// IMA-ADPCM encoder from interleaved int16 PCM.
Status ImaAdpcmEncodeInt16ToBlocks(
    const int16_t* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const ImaAdpcmConfig& config,
    std::vector<uint8_t>& out_blocks,
    uint16_t* out_block_align = nullptr,
    uint16_t* out_samples_per_block = nullptr);

class ImaAdpcmDecoder {
public:
    ImaAdpcmDecoder() = default;

    Status Initialize(const ImaAdpcmConfig& config);
    void Reset();
    bool IsInitialized() const noexcept { return initialized_; }
    const ImaAdpcmConfig& Config() const noexcept { return config_; }
    const ImaAdpcmDecodeStats& Stats() const noexcept { return stats_; }

    // Decodes one or more IMA-ADPCM blocks into interleaved float32 PCM (in [-1, 1]).
    Status DecodeBlocks(
        const uint8_t* block_data,
        size_t block_bytes,
        std::vector<float>& out_interleaved_pcm);

    // Decodes one or more IMA-ADPCM blocks into interleaved int16 PCM.
    Status DecodeBlocksInt16(
        const uint8_t* block_data,
        size_t block_bytes,
        std::vector<int16_t>& out_interleaved_pcm);

private:
    ImaAdpcmConfig config_{};
    ImaAdpcmDecodeStats stats_{};
    bool initialized_ = false;
};

// ===========================================================================
// 2. Opus (0x704F) Decoder via Native Windows MSOpusDecoder.dll MFT
// ===========================================================================

enum class OpusPacketFraming : uint32_t {
    Auto = 0u,              // Auto-detect U16/U32 length-prefixed multi-packet stream or single raw packet
    RawSinglePacket = 1u,   // Single raw RFC 6716 packet
    LengthPrefixedU16 = 2u, // Sequence of [uint16_t le_len][packet bytes]
    LengthPrefixedU32 = 3u, // Sequence of [uint32_t le_len][packet bytes]
};

enum class OpusBackendKind : uint32_t {
    None = 0u,
    NativeMsOpusMft = 1u,       // Windows native C:\Windows\System32\MSOpusDecoder.dll IMFTransform
    FallbackTocDecoder = 2u,    // Graceful RFC 6716 TOC fallback when MSOpusDecoder.dll is absent
};

struct OpusDecoderConfig {
    uint32_t sample_rate = 48000u;  // 8000, 12000, 16000, 24000, or 48000 (Opus native = 48000)
    uint16_t channels = 2u;         // 1 (mono) or 2 (stereo)
    uint16_t pre_skip = 0u;         // Optional OpusHead pre-skip samples
    bool allow_fallback = true;     // Fall back gracefully if MSOpusDecoder.dll is unavailable
};

struct OpusDecodeStats {
    uint64_t packets_fed = 0;
    uint64_t mft_outputs_drained = 0;
    uint64_t pcm_frames_output = 0;
    uint32_t output_sample_rate = 48000u;
    uint16_t output_channels = 2u;
    OpusBackendKind backend = OpusBackendKind::None;
    int32_t last_hresult = 0;
    std::string backend_detail;
};

struct OpusTocInfo {
    uint8_t config = 0;
    uint8_t stereo = 0;
    uint8_t frame_code = 0;
    uint32_t frames_in_packet = 1;
    uint32_t samples_per_frame_48k = 960;  // 120, 240, 480, 960, 1920, or 2880
    uint32_t total_samples_48k = 960;
};

// Parses RFC 6716 Section 3.1/3.2 Opus TOC header and frame count.
bool ParseOpusToc(const uint8_t* packet, size_t packet_bytes, OpusTocInfo& out_info) noexcept;

// Splits a length-prefixed or raw Opus buffer into packet slices.
struct OpusPacketSlice {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

bool ExtractOpusPackets(
    const uint8_t* stream_data,
    size_t stream_bytes,
    OpusPacketFraming framing,
    std::vector<OpusPacketSlice>& out_packets);

// RFC 6716 CELT fullband (48 kHz, 20 ms = 960 samples/channel) valid test packet
// builder. Produces genuine RFC 6716 CELT packets that Windows MSOpusDecoder.dll decodes into
// non-empty, finite, bounded float32 PCM frames.
std::vector<uint8_t> BuildOpusTestPacket(
    uint16_t channels,
    uint32_t frame_index = 0u,
    size_t payload_bytes = 64u);

// Packs a sequence of raw RFC 6716 packets with 2-byte or 4-byte little-endian length headers.
std::vector<uint8_t> PackOpusLengthPrefixedStream(
    const std::vector<std::vector<uint8_t>>& packets,
    bool use_u32_lengths = false);

class OpusMftDecoder {
public:
    OpusMftDecoder();
    ~OpusMftDecoder();

    OpusMftDecoder(const OpusMftDecoder&) = delete;
    OpusMftDecoder& operator=(const OpusMftDecoder&) = delete;

    Status Initialize(const OpusDecoderConfig& config);
    void Shutdown();
    void Reset();

    bool IsInitialized() const noexcept;
    bool IsNativeMftActive() const noexcept;
    const OpusDecoderConfig& Config() const noexcept;
    const OpusDecodeStats& Stats() const noexcept;

    // Decodes a single raw RFC 6716 Opus packet and appends interleaved float32 PCM.
    Status DecodePacket(
        const uint8_t* packet_data,
        size_t packet_bytes,
        std::vector<float>& out_interleaved_pcm);

    // Decodes a stream buffer (auto-detecting U16/U32 length-prefixed streams or single raw packet)
    // and appends interleaved float32 PCM.
    Status DecodeStream(
        const uint8_t* stream_data,
        size_t stream_bytes,
        std::vector<float>& out_interleaved_pcm,
        OpusPacketFraming framing = OpusPacketFraming::Auto);

    // Drains any buffered decoder output samples (e.g., at end of stream).
    Status Drain(std::vector<float>& out_interleaved_pcm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett
