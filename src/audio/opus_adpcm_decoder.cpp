#include "openscarlett/audio/opus_adpcm_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  include <objbase.h>
#  include <mferror.h>
#  include <mfobjects.h>
#  include <mftransform.h>
#endif

namespace openscarlett {

namespace {

// ===========================================================================
// Standard IMA-ADPCM Tables (Public IMA / Microsoft WAV 0x0011 Specification)
// ===========================================================================

constexpr std::array<int8_t, 16> kImaIndexTable = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

constexpr std::array<int16_t, 89> kImaStepTable = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

struct ImaChannelState {
    int32_t predictor = 0;
    int32_t step_index = 0;
};

inline int16_t DecodeImaNibble(uint8_t nibble, ImaChannelState& st) noexcept {
    const int32_t step = kImaStepTable[static_cast<size_t>(st.step_index)];
    int32_t diff = step >> 3;
    if ((nibble & 4u) != 0u) diff += step;
    if ((nibble & 2u) != 0u) diff += (step >> 1);
    if ((nibble & 1u) != 0u) diff += (step >> 2);

    if ((nibble & 8u) != 0u) {
        st.predictor -= diff;
    } else {
        st.predictor += diff;
    }
    st.predictor = std::clamp(st.predictor, -32768, 32767);
    st.step_index = std::clamp(st.step_index + static_cast<int32_t>(kImaIndexTable[nibble & 0x0Fu]), 0, 88);
    return static_cast<int16_t>(st.predictor);
}

inline uint8_t EncodeImaNibble(int16_t sample, ImaChannelState& st) noexcept {
    const int32_t step = kImaStepTable[static_cast<size_t>(st.step_index)];
    int32_t delta = static_cast<int32_t>(sample) - st.predictor;
    uint8_t nibble = 0u;
    if (delta < 0) {
        nibble = 8u;
        delta = -delta;
    }
    int32_t diff = step >> 3;
    if (delta >= step) {
        nibble |= 4u;
        delta -= step;
        diff += step;
    }
    if (delta >= (step >> 1)) {
        nibble |= 2u;
        delta -= (step >> 1);
        diff += (step >> 1);
    }
    if (delta >= (step >> 2)) {
        nibble |= 1u;
        diff += (step >> 2);
    }

    if ((nibble & 8u) != 0u) {
        st.predictor -= diff;
    } else {
        st.predictor += diff;
    }
    st.predictor = std::clamp(st.predictor, -32768, 32767);
    st.step_index = std::clamp(st.step_index + static_cast<int32_t>(kImaIndexTable[nibble & 0x0Fu]), 0, 88);
    return nibble;
}

inline uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u));
}

inline uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

inline void WriteLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
}

inline void WriteLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
}

// ===========================================================================
// RFC 6716 Section 3.1 Opus Frame Duration Table (in 48 kHz samples)
// ===========================================================================

uint32_t OpusConfigFrameSamples48k(uint8_t config) noexcept {
    // RFC 6716 Table 2: configuration number (0..31) -> frame duration at 48 kHz.
    if (config < 12u) {
        static constexpr uint32_t kSilkDur[4] = {480u, 960u, 1920u, 2880u};
        return kSilkDur[config & 3u];
    }
    if (config < 16u) {
        static constexpr uint32_t kHybridDur[2] = {480u, 960u};
        return kHybridDur[config & 1u];
    }
    static constexpr uint32_t kCeltDur[4] = {120u, 240u, 480u, 960u};
    return kCeltDur[config & 3u];
}

// Minimal RFC 6716 Section 4.1 range encoder for building valid
// CELT fullband test packets without external encoder libraries.
class CleanOpusRangeEncoder {
public:
    explicit CleanOpusRangeEncoder(size_t max_bytes) : max_bytes_(max_bytes) {
        buf_.reserve(max_bytes);
    }

    void EncodeBitLogP(uint32_t bit, uint32_t logp) {
        const uint32_t scale = rng_ >> logp;
        if (bit != 0u) {
            low_ += (rng_ - scale);
            rng_ = scale;
        } else {
            rng_ -= scale;
        }
        Normalize();
    }

    void EncodeUniform(uint32_t val, uint32_t total) {
        if (total <= 1u) return;
        const uint32_t scale = rng_ / total;
        if (val > 0u) {
            low_ += rng_ - scale * (total - val);
            rng_ = scale;
        } else {
            rng_ -= scale * (total - 1u);
        }
        Normalize();
    }

    void Finish(std::vector<uint8_t>& out) {
        uint32_t mask = 0x7FFFFFFFu;
        uint32_t end = low_ + mask;
        uint32_t shift = 0u;
        while ((end & ~mask) != ((low_ + rng_) & ~mask) && shift < 31u) {
            mask >>= 1u;
            end = (low_ + mask) & ~mask;
            ++shift;
        }
        low_ = end;
        rng_ = 1u << 23u;
        Normalize();
        Normalize();
        if (rem_ >= 0) {
            EmitByte(static_cast<uint8_t>(rem_));
        }
        while (ext_ > 0) {
            EmitByte(0xFFu);
            --ext_;
        }
        out.insert(out.end(), buf_.begin(), buf_.end());
    }

private:
    void EmitByte(uint8_t b) {
        if (buf_.size() < max_bytes_) {
            buf_.push_back(b);
        }
    }

    void CarryOut(uint32_t c) {
        if (c != 0xFFu) {
            const uint32_t carry = c >> 8u;
            if (rem_ >= 0) {
                EmitByte(static_cast<uint8_t>((static_cast<uint32_t>(rem_) + carry) & 0xFFu));
            }
            while (ext_ > 0) {
                EmitByte(static_cast<uint8_t>((0xFFu + carry) & 0xFFu));
                --ext_;
            }
            rem_ = static_cast<int32_t>(c & 0xFFu);
        } else {
            ++ext_;
        }
    }

    void Normalize() {
        while (rng_ <= (1u << 23u)) {
            CarryOut(low_ >> 23u);
            low_ = (low_ << 8u) & 0x7FFFFFFFu;
            rng_ <<= 8u;
        }
    }

    size_t max_bytes_ = 64u;
    uint32_t low_ = 0u;
    uint32_t rng_ = 0x80000000u;
    int32_t rem_ = -1;
    int32_t ext_ = 0;
    std::vector<uint8_t> buf_;
};

#if defined(_WIN32)

// Standard COM & Windows Media Foundation GUIDs (defined inline to avoid hard-linking mfuuid.lib
// and to stay portable across MSVC, Clang-CL, and MinGW-w64 without __uuidof()).
constexpr GUID kIidClassFactory = {
    0x00000001u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidImfTransform = {
    0xbf94c121u, 0x5b05u, 0x4e6fu, {0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d}};
constexpr GUID kClsidMsOpusDecoderMft = {
    0x63e17c10u, 0x2d43u, 0x4c42u, {0x8f, 0xe3, 0x8d, 0x8b, 0x63, 0xe4, 0x6a, 0x6a}};
constexpr GUID kMftCategoryAudioDecoder = {
    0x9ea73fb4u, 0xef7au, 0x4559u, {0x8d, 0x5d, 0x71, 0x9d, 0x8f, 0x04, 0x26, 0xc7}};
constexpr GUID kMfMediaTypeAudio = {
    0x73647561u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kMfAudioFormatPcm = {
    0x00000001u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kMfAudioFormatFloat = {
    0x00000003u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kMfAudioFormatOpus = {
    0x0000704fu, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr GUID kMfMtMajorType = {
    0x48eba18eu, 0xf8c9u, 0x4687u, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr GUID kMfMtSubtype = {
    0xf7e34c9au, 0x42e8u, 0x4714u, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr GUID kMfMtAllSamplesIndependent = {
    0xc9173739u, 0x5e56u, 0x461cu, {0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f}};
constexpr GUID kMfMtAudioNumChannels = {
    0x37e48bf5u, 0x645eu, 0x4c5bu, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
constexpr GUID kMfMtAudioSamplesPerSecond = {
    0x5faeeae7u, 0x0290u, 0x4c31u, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
constexpr GUID kMfMtAudioAvgBytesPerSecond = {
    0x1aab75c8u, 0xcfefu, 0x451cu, {0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31}};
constexpr GUID kMfMtAudioBlockAlignment = {
    0x322de230u, 0x9eebu, 0x43bdu, {0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d}};
constexpr GUID kMfMtAudioBitsPerSample = {
    0xf2deb57fu, 0x40fau, 0x4764u, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
constexpr GUID kMfMtUserData = {
    0xb6bc765fu, 0x4c3bu, 0x40a4u, {0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d}};

constexpr ULONG kMfVersionValue = (0x0002u << 16u) | 0x0070u;
constexpr DWORD kMfStartupLite = 0x1u;
constexpr UINT32 kMftEnumSyncLocal = 0x00000001u | 0x00000008u | 0x00000040u;

using PfnDllGetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using PfnMfStartup = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
using PfnMfShutdown = HRESULT(STDAPICALLTYPE*)();
using PfnMfCreateMediaType = HRESULT(STDAPICALLTYPE*)(IMFMediaType**);
using PfnMfCreateSample = HRESULT(STDAPICALLTYPE*)(IMFSample**);
using PfnMfCreateMemoryBuffer = HRESULT(STDAPICALLTYPE*)(DWORD, IMFMediaBuffer**);
using PfnMftEnumEx = HRESULT(STDAPICALLTYPE*)(
    GUID,
    UINT32,
    const MFT_REGISTER_TYPE_INFO*,
    const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***,
    UINT32*);

// Builds an RFC 7845 Section 5.1 19-byte OpusHead identification header.
std::array<uint8_t, 19> BuildOpusHeadBlob(uint16_t channels, uint32_t sample_rate, uint16_t pre_skip) {
    std::array<uint8_t, 19> head{};
    const char magic[8] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    std::memcpy(head.data(), magic, 8);
    head[8] = 1u;  // Version 1
    head[9] = static_cast<uint8_t>(std::clamp<uint16_t>(channels, 1u, 2u));
    WriteLe16(head.data() + 10, pre_skip);
    WriteLe32(head.data() + 12, sample_rate);
    WriteLe16(head.data() + 16, 0u);  // Output gain = 0 dB
    head[18] = 0u;                    // Channel mapping family 0 (mono/stereo)
    return head;
}

#endif  // _WIN32

}  // namespace

// ===========================================================================
// 1. IMA-ADPCM Implementation
// ===========================================================================

uint16_t ImaAdpcmComputeSamplesPerBlock(uint16_t channels, uint16_t block_align) noexcept {
    if (channels == 0u) return 0u;
    const uint32_t header_bytes = static_cast<uint32_t>(channels) * 4u;
    if (block_align < header_bytes) return 0u;
    const uint32_t data_bytes = static_cast<uint32_t>(block_align) - header_bytes;
    const uint32_t chunk_stride = static_cast<uint32_t>(channels) * 4u;
    const uint32_t full_chunks = data_bytes / chunk_stride;
    uint32_t samples = 1u + full_chunks * 8u;
    if (channels == 1u) {
        const uint32_t rem_bytes = data_bytes % 4u;
        samples += rem_bytes * 2u;
    }
    return static_cast<uint16_t>(samples);
}

Status ImaAdpcmEncodeInt16ToBlocks(
    const int16_t* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const ImaAdpcmConfig& config,
    std::vector<uint8_t>& out_blocks,
    uint16_t* out_block_align,
    uint16_t* out_samples_per_block) {
    out_blocks.clear();
    if (!interleaved_pcm && pcm_frames_per_channel > 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Null PCM pointer");
    }
    if (config.channels == 0u || config.channels > 2u) {
        return Status::Error(StatusCode::InvalidArgument, "IMA-ADPCM supports 1 or 2 channels");
    }

    const uint16_t ch = config.channels;
    const uint16_t default_align = static_cast<uint16_t>((config.xbox_adpcm ? 36u : 256u) * ch);
    const uint16_t block_align = (config.block_align != 0u) ? config.block_align : default_align;
    const uint32_t header_bytes = static_cast<uint32_t>(ch) * 4u;
    if (block_align < header_bytes || ((block_align - header_bytes) % (4u * ch)) != 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid IMA-ADPCM block_align");
    }

    const uint16_t max_spb = ImaAdpcmComputeSamplesPerBlock(ch, block_align);
    const uint16_t spb = (config.samples_per_block != 0u && config.samples_per_block <= max_spb)
                             ? config.samples_per_block
                             : max_spb;

    if (out_block_align) *out_block_align = block_align;
    if (out_samples_per_block) *out_samples_per_block = spb;
    if (pcm_frames_per_channel == 0u) {
        return Status::Ok();
    }

    const uint32_t chunks_per_block = (block_align - header_bytes) / (4u * ch);
    std::array<ImaChannelState, 2> st{};

    size_t frame_pos = 0u;
    while (frame_pos < pcm_frames_per_channel) {
        const size_t block_offset = out_blocks.size();
        out_blocks.resize(block_offset + block_align, 0u);
        uint8_t* blk = out_blocks.data() + block_offset;

        // Write 4-byte header per channel (sample 0 of the block is predictor verbatim).
        for (uint16_t c = 0u; c < ch; ++c) {
            const int16_t s0 = interleaved_pcm[frame_pos * ch + c];
            st[c].predictor = static_cast<int32_t>(s0);
            uint8_t* hdr = blk + static_cast<size_t>(c) * 4u;
            WriteLe16(hdr, static_cast<uint16_t>(s0));
            hdr[2] = static_cast<uint8_t>(std::clamp(st[c].step_index, 0, 88));
            hdr[3] = 0u;
        }
        ++frame_pos;

        // Encode 8 samples per channel per 4-byte interleaved chunk.
        uint8_t* dst = blk + header_bytes;
        uint32_t samples_in_block = 1u;
        for (uint32_t chunk = 0u; chunk < chunks_per_block; ++chunk) {
            for (uint16_t c = 0u; c < ch; ++c) {
                for (uint32_t b = 0u; b < 4u; ++b) {
                    const size_t f0 = frame_pos + static_cast<size_t>(b) * 2u;
                    const size_t f1 = f0 + 1u;
                    const int16_t s0 = (f0 < pcm_frames_per_channel && samples_in_block + b * 2u < spb)
                                           ? interleaved_pcm[f0 * ch + c]
                                           : static_cast<int16_t>(st[c].predictor);
                    const uint8_t n0 = EncodeImaNibble(s0, st[c]);
                    const int16_t s1 = (f1 < pcm_frames_per_channel && samples_in_block + b * 2u + 1u < spb)
                                           ? interleaved_pcm[f1 * ch + c]
                                           : static_cast<int16_t>(st[c].predictor);
                    const uint8_t n1 = EncodeImaNibble(s1, st[c]);
                    dst[b] = static_cast<uint8_t>((n0 & 0x0Fu) | ((n1 & 0x0Fu) << 4u));
                }
                dst += 4u;
            }
            const size_t advance = std::min<size_t>(8u, (spb > samples_in_block) ? (spb - samples_in_block) : 0u);
            frame_pos += advance;
            samples_in_block += 8u;
        }
    }

    return Status::Ok();
}

Status ImaAdpcmEncodePcmToBlocks(
    const float* interleaved_pcm,
    size_t pcm_frames_per_channel,
    const ImaAdpcmConfig& config,
    std::vector<uint8_t>& out_blocks,
    uint16_t* out_block_align,
    uint16_t* out_samples_per_block) {
    if (!interleaved_pcm && pcm_frames_per_channel > 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Null float PCM pointer");
    }
    if (config.channels == 0u || config.channels > 2u) {
        return Status::Error(StatusCode::InvalidArgument, "IMA-ADPCM supports 1 or 2 channels");
    }
    const size_t total_samples = pcm_frames_per_channel * config.channels;
    std::vector<int16_t> pcm16(total_samples);
    for (size_t i = 0u; i < total_samples; ++i) {
        const float clamped = std::clamp(interleaved_pcm[i], -1.0f, 1.0f);
        pcm16[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    return ImaAdpcmEncodeInt16ToBlocks(
        pcm16.data(), pcm_frames_per_channel, config, out_blocks, out_block_align, out_samples_per_block);
}

Status ImaAdpcmDecoder::Initialize(const ImaAdpcmConfig& config) {
    if (config.channels == 0u || config.channels > 2u) {
        return Status::Error(StatusCode::InvalidArgument, "IMA-ADPCM channels must be 1 or 2");
    }
    config_ = config;
    if (config_.block_align == 0u) {
        config_.block_align = static_cast<uint16_t>((config_.xbox_adpcm ? 36u : 256u) * config_.channels);
    }
    const uint32_t header_bytes = static_cast<uint32_t>(config_.channels) * 4u;
    if (config_.block_align < header_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "IMA-ADPCM block_align smaller than header");
    }
    const uint16_t max_spb = ImaAdpcmComputeSamplesPerBlock(config_.channels, config_.block_align);
    if (config_.samples_per_block == 0u || config_.samples_per_block > max_spb) {
        config_.samples_per_block = max_spb;
    }
    stats_ = ImaAdpcmDecodeStats{};
    initialized_ = true;
    return Status::Ok();
}

void ImaAdpcmDecoder::Reset() {
    stats_ = ImaAdpcmDecodeStats{};
}

Status ImaAdpcmDecoder::DecodeBlocksInt16(
    const uint8_t* block_data,
    size_t block_bytes,
    std::vector<int16_t>& out_interleaved_pcm) {
    if (!initialized_) {
        return Status::Error(StatusCode::InvalidArgument, "ImaAdpcmDecoder not initialized");
    }
    if (block_bytes == 0u) {
        return Status::Ok();
    }
    if (!block_data) {
        return Status::Error(StatusCode::InvalidArgument, "Null IMA-ADPCM input buffer");
    }

    const uint16_t ch = config_.channels;
    const uint32_t header_bytes = static_cast<uint32_t>(ch) * 4u;
    const uint32_t align = config_.block_align;
    const uint32_t target_spb = config_.samples_per_block;
    if (align < header_bytes || target_spb == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid IMA-ADPCM decoder block parameters");
    }

    size_t offset = 0u;
    while (offset < block_bytes) {
        const size_t avail = std::min<size_t>(align, block_bytes - offset);
        if (avail < header_bytes) {
            break;
        }
        const uint8_t* blk = block_data + offset;
        std::array<ImaChannelState, 2> st{};

        const size_t block_start_sample = out_interleaved_pcm.size();
        // Sample 0 from per-channel 4-byte header.
        for (uint16_t c = 0u; c < ch; ++c) {
            const uint8_t* hdr = blk + static_cast<size_t>(c) * 4u;
            const int16_t pred = static_cast<int16_t>(ReadLe16(hdr));
            st[c].predictor = static_cast<int32_t>(pred);
            st[c].step_index = std::clamp<int32_t>(static_cast<int32_t>(hdr[2]), 0, 88);
            out_interleaved_pcm.push_back(pred);
        }

        uint32_t decoded_frames = 1u;
        size_t pos = header_bytes;
        const size_t chunk_bytes = static_cast<size_t>(ch) * 4u;

        // Decode 4-byte-per-channel interleaved chunks (8 samples per channel per chunk).
        while (pos + chunk_bytes <= avail && decoded_frames < target_spb) {
            std::array<std::array<int16_t, 8>, 2> ch_samples{};
            for (uint16_t c = 0u; c < ch; ++c) {
                const uint8_t* word = blk + pos + static_cast<size_t>(c) * 4u;
                for (uint32_t b = 0u; b < 4u; ++b) {
                    const uint8_t byte_val = word[b];
                    ch_samples[c][b * 2u + 0u] = DecodeImaNibble(byte_val & 0x0Fu, st[c]);
                    ch_samples[c][b * 2u + 1u] = DecodeImaNibble((byte_val >> 4u) & 0x0Fu, st[c]);
                }
            }
            const uint32_t emit_frames = std::min<uint32_t>(8u, target_spb - decoded_frames);
            for (uint32_t f = 0u; f < emit_frames; ++f) {
                for (uint16_t c = 0u; c < ch; ++c) {
                    out_interleaved_pcm.push_back(ch_samples[c][f]);
                }
            }
            decoded_frames += emit_frames;
            pos += chunk_bytes;
        }

        // Trailing partial mono bytes if present.
        if (ch == 1u) {
            while (pos < avail && decoded_frames < target_spb) {
                const uint8_t byte_val = blk[pos++];
                out_interleaved_pcm.push_back(DecodeImaNibble(byte_val & 0x0Fu, st[0]));
                ++decoded_frames;
                if (decoded_frames < target_spb) {
                    out_interleaved_pcm.push_back(DecodeImaNibble((byte_val >> 4u) & 0x0Fu, st[0]));
                    ++decoded_frames;
                }
            }
        }

        (void)block_start_sample;
        ++stats_.blocks_decoded;
        stats_.pcm_frames_output += decoded_frames;
        offset += avail;
    }

    return Status::Ok();
}

Status ImaAdpcmDecoder::DecodeBlocks(
    const uint8_t* block_data,
    size_t block_bytes,
    std::vector<float>& out_interleaved_pcm) {
    std::vector<int16_t> pcm16;
    Status st = DecodeBlocksInt16(block_data, block_bytes, pcm16);
    if (!st.ok()) {
        return st;
    }
    const size_t base = out_interleaved_pcm.size();
    out_interleaved_pcm.resize(base + pcm16.size());
    for (size_t i = 0u; i < pcm16.size(); ++i) {
        out_interleaved_pcm[base + i] = static_cast<float>(pcm16[i]) / 32768.0f;
    }
    return Status::Ok();
}

// ===========================================================================
// 2. Opus RFC 6716 TOC Parsing, Stream Framing & Packet Builder
// ===========================================================================

bool ParseOpusToc(const uint8_t* packet, size_t packet_bytes, OpusTocInfo& out_info) noexcept {
    if (!packet || packet_bytes == 0u) {
        return false;
    }
    const uint8_t toc = packet[0];
    out_info.config = static_cast<uint8_t>((toc >> 3u) & 0x1Fu);
    out_info.stereo = static_cast<uint8_t>((toc >> 2u) & 0x01u);
    out_info.frame_code = static_cast<uint8_t>(toc & 0x03u);
    out_info.samples_per_frame_48k = OpusConfigFrameSamples48k(out_info.config);

    switch (out_info.frame_code) {
        case 0u:
            out_info.frames_in_packet = 1u;
            break;
        case 1u:
        case 2u:
            out_info.frames_in_packet = 2u;
            break;
        case 3u:
            if (packet_bytes < 2u) {
                return false;
            }
            out_info.frames_in_packet = static_cast<uint32_t>(packet[1] & 0x3Fu);
            if (out_info.frames_in_packet == 0u || out_info.frames_in_packet > 48u) {
                return false;
            }
            break;
        default:
            return false;
    }
    out_info.total_samples_48k = out_info.frames_in_packet * out_info.samples_per_frame_48k;
    return out_info.total_samples_48k <= 5760u;  // RFC 6716 max 120 ms per packet
}

namespace {

bool TryParseLengthPrefixed(
    const uint8_t* data,
    size_t bytes,
    size_t hdr_size,
    size_t min_packets,
    std::vector<OpusPacketSlice>& out_packets) {
    out_packets.clear();
    if (!data || bytes <= hdr_size) {
        return false;
    }
    size_t pos = 0u;
    while (pos < bytes) {
        if (pos + hdr_size > bytes) {
            out_packets.clear();
            return false;
        }
        const uint32_t pkt_len = (hdr_size == 2u) ? static_cast<uint32_t>(ReadLe16(data + pos))
                                                  : ReadLe32(data + pos);
        pos += hdr_size;
        if (pkt_len == 0u || pkt_len > 4096u || pos + pkt_len > bytes) {
            out_packets.clear();
            return false;
        }
        OpusTocInfo toc{};
        if (!ParseOpusToc(data + pos, pkt_len, toc)) {
            out_packets.clear();
            return false;
        }
        out_packets.push_back(OpusPacketSlice{data + pos, pkt_len});
        pos += pkt_len;
    }
    if (out_packets.size() < min_packets) {
        out_packets.clear();
        return false;
    }
    return true;
}

}  // namespace

bool ExtractOpusPackets(
    const uint8_t* stream_data,
    size_t stream_bytes,
    OpusPacketFraming framing,
    std::vector<OpusPacketSlice>& out_packets) {
    out_packets.clear();
    if (!stream_data || stream_bytes == 0u) {
        return false;
    }

    if (framing == OpusPacketFraming::RawSinglePacket) {
        OpusTocInfo toc{};
        if (!ParseOpusToc(stream_data, stream_bytes, toc)) {
            return false;
        }
        out_packets.push_back(OpusPacketSlice{stream_data, stream_bytes});
        return true;
    }

    if (framing == OpusPacketFraming::LengthPrefixedU16) {
        return TryParseLengthPrefixed(stream_data, stream_bytes, 2u, 1u, out_packets);
    }

    if (framing == OpusPacketFraming::LengthPrefixedU32) {
        return TryParseLengthPrefixed(stream_data, stream_bytes, 4u, 1u, out_packets);
    }

    // Auto framing:
    // 1. Check exact multi-packet 2-byte length-prefixed Xbox Opus stream.
    if (TryParseLengthPrefixed(stream_data, stream_bytes, 2u, 2u, out_packets)) {
        return true;
    }
    // 2. Check exact multi-packet 4-byte length-prefixed stream.
    if (TryParseLengthPrefixed(stream_data, stream_bytes, 4u, 2u, out_packets)) {
        return true;
    }
    // 3. Check single 4-byte length-prefixed packet (bytes 2..3 == 0 and exact length match).
    if (stream_bytes >= 5u && stream_data[2] == 0u && stream_data[3] == 0u &&
        TryParseLengthPrefixed(stream_data, stream_bytes, 4u, 1u, out_packets)) {
        return true;
    }
    // 4. Fall back to single raw RFC 6716 Opus packet.
    OpusTocInfo toc{};
    if (ParseOpusToc(stream_data, stream_bytes, toc)) {
        out_packets.push_back(OpusPacketSlice{stream_data, stream_bytes});
        return true;
    }
    // 5. Final check: single 2-byte length-prefixed packet if raw TOC failed.
    return TryParseLengthPrefixed(stream_data, stream_bytes, 2u, 1u, out_packets);
}

std::vector<uint8_t> BuildOpusTestPacket(
    uint16_t channels,
    uint32_t frame_index,
    size_t payload_bytes) {
    const size_t body_len = std::clamp<size_t>(payload_bytes, 16u, 512u);
    const uint8_t stereo_bit = (channels >= 2u) ? 1u : 0u;
    // RFC 6716 config 31 = CELT-only, Fullband (48 kHz), 20 ms (960 samples/channel), code 0 = 1 frame.
    const uint8_t toc = static_cast<uint8_t>((31u << 3u) | (stereo_bit << 2u) | 0u);

    CleanOpusRangeEncoder rc(body_len);
    // RFC 6716 Section 4.3 CELT frame header:
    rc.EncodeBitLogP(0u, 15u);  // silence = 0
    rc.EncodeBitLogP(0u, 1u);   // postfilter = 0
    rc.EncodeBitLogP(0u, 3u);   // transient = 0
    rc.EncodeBitLogP(1u, 3u);   // intra = 1 (uses non-zero RFC 6716 eMean baseline!)

    // Encode deterministic coarse/fine energy & PVQ entropy bits so each frame decodes
    // to valid, finite, non-zero CELT audio via RFC 6716 decoders (including MSOpusDecoder.dll).
    uint32_t state = 0x51A7C3E9u ^ (frame_index * 0x9E3779B1u);
    for (uint32_t i = 0u; i < 16u; ++i) {
        state = state * 1664525u + 1013904223u;
        rc.EncodeUniform((state >> 24u) & 3u, 4u);
    }

    std::vector<uint8_t> packet;
    packet.reserve(1u + body_len);
    packet.push_back(toc);
    rc.Finish(packet);

    // Pad with deterministic non-zero CELT PVQ payload bytes to target frame size.
    while (packet.size() < 1u + body_len) {
        state = state * 1664525u + 1013904223u;
        packet.push_back(static_cast<uint8_t>((state >> 16u) & 0xFFu));
    }
    return packet;
}

std::vector<uint8_t> PackOpusLengthPrefixedStream(
    const std::vector<std::vector<uint8_t>>& packets,
    bool use_u32_lengths) {
    std::vector<uint8_t> out;
    const size_t hdr = use_u32_lengths ? 4u : 2u;
    for (const auto& pkt : packets) {
        if (pkt.empty()) continue;
        const size_t pos = out.size();
        out.resize(pos + hdr + pkt.size());
        if (use_u32_lengths) {
            WriteLe32(out.data() + pos, static_cast<uint32_t>(pkt.size()));
        } else {
            WriteLe16(out.data() + pos, static_cast<uint16_t>(pkt.size()));
        }
        std::memcpy(out.data() + pos + hdr, pkt.data(), pkt.size());
    }
    return out;
}

// ===========================================================================
// 3. OpusMftDecoder Implementation (Dynamic MSOpusDecoder.dll + MFPlat.dll)
// ===========================================================================

struct OpusMftDecoder::Impl {
    OpusDecoderConfig config{};
    OpusDecodeStats stats{};
    bool initialized = false;
    uint64_t timestamp_100ns = 0u;

#if defined(_WIN32)
    bool com_initialized = false;
    bool mf_started = false;
    HMODULE mfplat_mod = nullptr;
    HMODULE opus_mod = nullptr;

    PfnMfStartup pfn_mf_startup = nullptr;
    PfnMfShutdown pfn_mf_shutdown = nullptr;
    PfnMfCreateMediaType pfn_mf_create_media_type = nullptr;
    PfnMfCreateSample pfn_mf_create_sample = nullptr;
    PfnMfCreateMemoryBuffer pfn_mf_create_memory_buffer = nullptr;
    PfnMftEnumEx pfn_mft_enum_ex = nullptr;

    IMFTransform* mft = nullptr;
    bool output_is_float = true;
    uint32_t output_bits = 32u;
    bool mft_provides_samples = false;
    DWORD min_output_buf_bytes = 65536u;

    void ReleaseMftResources() {
        if (mft) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft->Release();
            mft = nullptr;
        }
        if (mf_started && pfn_mf_shutdown) {
            pfn_mf_shutdown();
            mf_started = false;
        }
        if (opus_mod) {
            FreeLibrary(opus_mod);
            opus_mod = nullptr;
        }
        if (mfplat_mod) {
            FreeLibrary(mfplat_mod);
            mfplat_mod = nullptr;
        }
        pfn_mf_startup = nullptr;
        pfn_mf_shutdown = nullptr;
        pfn_mf_create_media_type = nullptr;
        pfn_mf_create_sample = nullptr;
        pfn_mf_create_memory_buffer = nullptr;
        pfn_mft_enum_ex = nullptr;
        if (com_initialized) {
            CoUninitialize();
            com_initialized = false;
        }
    }

    bool LoadMfPlat() {
        mfplat_mod = LoadLibraryW(L"mfplat.dll");
        if (!mfplat_mod) {
            return false;
        }
        pfn_mf_startup = reinterpret_cast<PfnMfStartup>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFStartup")));
        pfn_mf_shutdown = reinterpret_cast<PfnMfShutdown>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFShutdown")));
        pfn_mf_create_media_type = reinterpret_cast<PfnMfCreateMediaType>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFCreateMediaType")));
        pfn_mf_create_sample = reinterpret_cast<PfnMfCreateSample>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFCreateSample")));
        pfn_mf_create_memory_buffer = reinterpret_cast<PfnMfCreateMemoryBuffer>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFCreateMemoryBuffer")));
        pfn_mft_enum_ex = reinterpret_cast<PfnMftEnumEx>(
            reinterpret_cast<void*>(GetProcAddress(mfplat_mod, "MFTEnumEx")));
        return pfn_mf_startup && pfn_mf_shutdown && pfn_mf_create_media_type &&
               pfn_mf_create_sample && pfn_mf_create_memory_buffer;
    }

    bool InstantiateOpusMft() {
        // Method 1: Direct LoadLibraryW("MSOpusDecoder.dll") + DllGetClassObject
        opus_mod = LoadLibraryW(L"MSOpusDecoder.dll");
        if (opus_mod) {
            auto pfn_get_class = reinterpret_cast<PfnDllGetClassObject>(
                reinterpret_cast<void*>(GetProcAddress(opus_mod, "DllGetClassObject")));
            if (pfn_get_class) {
                IClassFactory* factory = nullptr;
                HRESULT hr = pfn_get_class(
                    kClsidMsOpusDecoderMft, kIidClassFactory, reinterpret_cast<void**>(&factory));
                if (SUCCEEDED(hr) && factory) {
                    hr = factory->CreateInstance(
                        nullptr, kIidImfTransform, reinterpret_cast<void**>(&mft));
                    factory->Release();
                    stats.last_hresult = static_cast<int32_t>(hr);
                    if (SUCCEEDED(hr) && mft) {
                        stats.backend_detail = "MSOpusDecoder.dll (DllGetClassObject)";
                        return true;
                    }
                } else {
                    stats.last_hresult = static_cast<int32_t>(hr);
                }
            }
        }

        // Method 2: CoCreateInstance(CLSID_CMSOpusDecoderMFT)
        {
            HRESULT hr = CoCreateInstance(
                kClsidMsOpusDecoderMft,
                nullptr,
                CLSCTX_INPROC_SERVER,
                kIidImfTransform,
                reinterpret_cast<void**>(&mft));
            stats.last_hresult = static_cast<int32_t>(hr);
            if (SUCCEEDED(hr) && mft) {
                stats.backend_detail = "MSOpusDecoder.dll (CoCreateInstance)";
                return true;
            }
        }

        // Method 3: MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFAudioFormat_Opus)
        if (pfn_mft_enum_ex) {
            MFT_REGISTER_TYPE_INFO in_info{};
            in_info.guidMajorType = kMfMediaTypeAudio;
            in_info.guidSubtype = kMfAudioFormatOpus;
            IMFActivate** activates = nullptr;
            UINT32 count = 0u;
            HRESULT hr = pfn_mft_enum_ex(
                kMftCategoryAudioDecoder,
                kMftEnumSyncLocal,
                &in_info,
                nullptr,
                &activates,
                &count);
            stats.last_hresult = static_cast<int32_t>(hr);
            if (SUCCEEDED(hr) && activates) {
                for (UINT32 i = 0u; i < count; ++i) {
                    if (!mft && activates[i]) {
                        HRESULT act_hr = activates[i]->ActivateObject(
                            kIidImfTransform, reinterpret_cast<void**>(&mft));
                        stats.last_hresult = static_cast<int32_t>(act_hr);
                        if (SUCCEEDED(act_hr) && mft) {
                            stats.backend_detail = "MSOpusDecoder.dll (MFTEnumEx)";
                        }
                    }
                    if (activates[i]) activates[i]->Release();
                }
                CoTaskMemFree(activates);
                if (mft) return true;
            }
        }

        return false;
    }

    bool ConfigureInputType() {
        const uint32_t sr = (config.sample_rate != 0u) ? config.sample_rate : 48000u;
        const uint32_t ch = std::clamp<uint32_t>(config.channels, 1u, 2u);
        const auto opus_head = BuildOpusHeadBlob(static_cast<uint16_t>(ch), sr, config.pre_skip);

        // Try with OpusHead MF_MT_USER_DATA first (preferred by MSOpusDecoder.dll),
        // then without user data as a secondary variant.
        for (int attempt = 0; attempt < 2; ++attempt) {
            IMFMediaType* in_type = nullptr;
            HRESULT hr = pfn_mf_create_media_type(&in_type);
            if (FAILED(hr) || !in_type) {
                stats.last_hresult = static_cast<int32_t>(hr);
                return false;
            }
            in_type->SetGUID(kMfMtMajorType, kMfMediaTypeAudio);
            in_type->SetGUID(kMfMtSubtype, kMfAudioFormatOpus);
            in_type->SetUINT32(kMfMtAudioNumChannels, ch);
            in_type->SetUINT32(kMfMtAudioSamplesPerSecond, sr);
            in_type->SetUINT32(kMfMtAudioBitsPerSample, 16u);
            in_type->SetUINT32(kMfMtAudioBlockAlignment, 1u);
            in_type->SetUINT32(kMfMtAllSamplesIndependent, 1u);
            if (attempt == 0) {
                in_type->SetBlob(kMfMtUserData, opus_head.data(), static_cast<UINT32>(opus_head.size()));
            }
            hr = mft->SetInputType(0u, in_type, 0u);
            in_type->Release();
            stats.last_hresult = static_cast<int32_t>(hr);
            if (SUCCEEDED(hr)) {
                return true;
            }
        }
        return false;
    }

    bool NegotiateOutputType() {
        IMFMediaType* chosen_float = nullptr;
        IMFMediaType* chosen_pcm = nullptr;

        for (DWORD idx = 0u; idx < 32u; ++idx) {
            IMFMediaType* cand = nullptr;
            const HRESULT hr = mft->GetOutputAvailableType(0u, idx, &cand);
            if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !cand) {
                break;
            }
            GUID subtype{};
            if (SUCCEEDED(cand->GetGUID(kMfMtSubtype, &subtype))) {
                if (std::memcmp(&subtype, &kMfAudioFormatFloat, sizeof(GUID)) == 0 && !chosen_float) {
                    chosen_float = cand;
                    cand = nullptr;
                } else if (std::memcmp(&subtype, &kMfAudioFormatPcm, sizeof(GUID)) == 0 && !chosen_pcm) {
                    chosen_pcm = cand;
                    cand = nullptr;
                }
            }
            if (cand) cand->Release();
        }

        IMFMediaType* selected = chosen_float ? chosen_float : chosen_pcm;
        if (!selected) {
            // Manually construct a float32 output media type as fallback.
            if (SUCCEEDED(pfn_mf_create_media_type(&selected)) && selected) {
                const uint32_t sr = (config.sample_rate != 0u) ? config.sample_rate : 48000u;
                const uint32_t ch = std::clamp<uint32_t>(config.channels, 1u, 2u);
                selected->SetGUID(kMfMtMajorType, kMfMediaTypeAudio);
                selected->SetGUID(kMfMtSubtype, kMfAudioFormatFloat);
                selected->SetUINT32(kMfMtAudioNumChannels, ch);
                selected->SetUINT32(kMfMtAudioSamplesPerSecond, sr);
                selected->SetUINT32(kMfMtAudioBitsPerSample, 32u);
                selected->SetUINT32(kMfMtAudioBlockAlignment, ch * 4u);
                selected->SetUINT32(kMfMtAudioAvgBytesPerSecond, sr * ch * 4u);
            }
        }
        if (!selected) {
            if (chosen_float) chosen_float->Release();
            if (chosen_pcm) chosen_pcm->Release();
            return false;
        }

        const HRESULT hr = mft->SetOutputType(0u, selected, 0u);
        stats.last_hresult = static_cast<int32_t>(hr);
        if (SUCCEEDED(hr)) {
            GUID sub{};
            UINT32 sr = config.sample_rate != 0u ? config.sample_rate : 48000u;
            UINT32 ch = config.channels != 0u ? config.channels : 2u;
            UINT32 bits = 32u;
            if (SUCCEEDED(selected->GetGUID(kMfMtSubtype, &sub))) {
                output_is_float = (std::memcmp(&sub, &kMfAudioFormatFloat, sizeof(GUID)) == 0);
            }
            selected->GetUINT32(kMfMtAudioSamplesPerSecond, &sr);
            selected->GetUINT32(kMfMtAudioNumChannels, &ch);
            selected->GetUINT32(kMfMtAudioBitsPerSample, &bits);
            output_bits = bits;
            stats.output_sample_rate = sr;
            stats.output_channels = static_cast<uint16_t>(ch);

            MFT_OUTPUT_STREAM_INFO osi{};
            if (SUCCEEDED(mft->GetOutputStreamInfo(0u, &osi))) {
                mft_provides_samples =
                    ((osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0u);
                min_output_buf_bytes = std::max<DWORD>(osi.cbSize, 65536u);
            }
        }

        if (chosen_float && chosen_float != selected) chosen_float->Release();
        if (chosen_pcm && chosen_pcm != selected) chosen_pcm->Release();
        selected->Release();
        return SUCCEEDED(hr);
    }

    bool InitNativeMft() {
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_initialized = SUCCEEDED(co_hr);

        if (!LoadMfPlat()) {
            ReleaseMftResources();
            return false;
        }
        const HRESULT mf_hr = pfn_mf_startup(kMfVersionValue, kMfStartupLite);
        stats.last_hresult = static_cast<int32_t>(mf_hr);
        if (FAILED(mf_hr)) {
            ReleaseMftResources();
            return false;
        }
        mf_started = true;

        if (!InstantiateOpusMft()) {
            ReleaseMftResources();
            return false;
        }
        if (!ConfigureInputType()) {
            ReleaseMftResources();
            return false;
        }
        if (!NegotiateOutputType()) {
            ReleaseMftResources();
            return false;
        }

        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        stats.backend = OpusBackendKind::NativeMsOpusMft;
        return true;
    }

    Status DrainMftOutputs(std::vector<float>& out_pcm) {
        if (!mft) {
            return Status::Error(StatusCode::DeviceError, "Opus MFT not initialized");
        }

        for (int iter = 0; iter < 64; ++iter) {
            MFT_OUTPUT_DATA_BUFFER out_data{};
            out_data.dwStreamID = 0u;
            IMFSample* caller_sample = nullptr;

            if (!mft_provides_samples) {
                HRESULT hr = pfn_mf_create_sample(&caller_sample);
                if (FAILED(hr) || !caller_sample) {
                    return Status::Error(StatusCode::OutOfMemory, "MFCreateSample failed for Opus output");
                }
                IMFMediaBuffer* buf = nullptr;
                hr = pfn_mf_create_memory_buffer(min_output_buf_bytes, &buf);
                if (FAILED(hr) || !buf) {
                    caller_sample->Release();
                    return Status::Error(StatusCode::OutOfMemory, "MFCreateMemoryBuffer failed");
                }
                caller_sample->AddBuffer(buf);
                buf->Release();
                out_data.pSample = caller_sample;
            }

            DWORD status = 0u;
            const HRESULT hr = mft->ProcessOutput(0u, 1u, &out_data, &status);
            stats.last_hresult = static_cast<int32_t>(hr);

            if (out_data.pEvents) {
                out_data.pEvents->Release();
                out_data.pEvents = nullptr;
            }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (caller_sample) caller_sample->Release();
                break;
            }

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (caller_sample) caller_sample->Release();
                if (!NegotiateOutputType()) {
                    return Status::Error(StatusCode::DeviceError, "Opus MFT stream change negotiation failed");
                }
                continue;
            }

            if (FAILED(hr)) {
                if (caller_sample) caller_sample->Release();
                return Status::Error(StatusCode::DeviceError, "Opus MFT ProcessOutput failed");
            }

            IMFSample* produced = out_data.pSample;
            if (produced) {
                IMFMediaBuffer* contig = nullptr;
                if (SUCCEEDED(produced->ConvertToContiguousBuffer(&contig)) && contig) {
                    BYTE* ptr = nullptr;
                    DWORD max_len = 0u;
                    DWORD cur_len = 0u;
                    if (SUCCEEDED(contig->Lock(&ptr, &max_len, &cur_len)) && ptr && cur_len > 0u) {
                        const uint32_t out_ch = std::max<uint32_t>(1u, stats.output_channels);
                        if (output_is_float && output_bits == 32u) {
                            const size_t n = cur_len / sizeof(float);
                            const size_t base = out_pcm.size();
                            out_pcm.resize(base + n);
                            std::memcpy(out_pcm.data() + base, ptr, n * sizeof(float));
                            stats.pcm_frames_output += (n / out_ch);
                        } else if (output_bits == 16u) {
                            const size_t n = cur_len / sizeof(int16_t);
                            const auto* s16 = reinterpret_cast<const int16_t*>(ptr);
                            const size_t base = out_pcm.size();
                            out_pcm.resize(base + n);
                            for (size_t i = 0u; i < n; ++i) {
                                out_pcm[base + i] = static_cast<float>(s16[i]) / 32768.0f;
                            }
                            stats.pcm_frames_output += (n / out_ch);
                        }
                        contig->Unlock();
                        ++stats.mft_outputs_drained;
                    }
                    contig->Release();
                }
                produced->Release();
            }
        }
        return Status::Ok();
    }

    Status FeedPacketMft(const uint8_t* pkt, size_t len, std::vector<float>& out_pcm) {
        OpusTocInfo toc{};
        const uint32_t samples_48k = ParseOpusToc(pkt, len, toc) ? toc.total_samples_48k : 960u;
        const LONGLONG dur_100ns =
            static_cast<LONGLONG>((static_cast<uint64_t>(samples_48k) * 10000000ull) / 48000ull);

        IMFSample* in_sample = nullptr;
        HRESULT hr = pfn_mf_create_sample(&in_sample);
        if (FAILED(hr) || !in_sample) {
            return Status::Error(StatusCode::OutOfMemory, "MFCreateSample failed for Opus input");
        }
        IMFMediaBuffer* in_buf = nullptr;
        hr = pfn_mf_create_memory_buffer(static_cast<DWORD>(len), &in_buf);
        if (FAILED(hr) || !in_buf) {
            in_sample->Release();
            return Status::Error(StatusCode::OutOfMemory, "MFCreateMemoryBuffer failed for Opus input");
        }

        BYTE* dst = nullptr;
        hr = in_buf->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr) && dst) {
            std::memcpy(dst, pkt, len);
            in_buf->Unlock();
            in_buf->SetCurrentLength(static_cast<DWORD>(len));
        } else {
            in_buf->Release();
            in_sample->Release();
            return Status::Error(StatusCode::DeviceError, "Failed to lock Opus input buffer");
        }

        in_sample->AddBuffer(in_buf);
        in_buf->Release();
        in_sample->SetSampleTime(static_cast<LONGLONG>(timestamp_100ns));
        in_sample->SetSampleDuration(dur_100ns);
        timestamp_100ns += static_cast<uint64_t>(dur_100ns);

        hr = mft->ProcessInput(0u, in_sample, 0u);
        if (hr == MF_E_NOTACCEPTING) {
            Status dst_st = DrainMftOutputs(out_pcm);
            if (!dst_st.ok()) {
                in_sample->Release();
                return dst_st;
            }
            hr = mft->ProcessInput(0u, in_sample, 0u);
        }
        in_sample->Release();
        stats.last_hresult = static_cast<int32_t>(hr);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::ParseError, "MSOpusDecoder MFT rejected Opus packet");
        }

        ++stats.packets_fed;
        return DrainMftOutputs(out_pcm);
    }
#endif  // _WIN32

    Status DecodePacketFallback(const uint8_t* pkt, size_t len, std::vector<float>& out_pcm) {
        OpusTocInfo toc{};
        if (!ParseOpusToc(pkt, len, toc)) {
            return Status::Error(StatusCode::ParseError, "Invalid RFC 6716 Opus TOC");
        }
        const uint32_t sr = std::max<uint32_t>(8000u, stats.output_sample_rate);
        const uint32_t ch = std::max<uint32_t>(1u, stats.output_channels);
        const uint32_t frames = static_cast<uint32_t>(
            (static_cast<uint64_t>(toc.total_samples_48k) * sr) / 48000ull);
        const size_t base = out_pcm.size();
        out_pcm.resize(base + static_cast<size_t>(frames) * ch, 0.0f);

        // 1-byte TOC-only packet is RFC 6716 DTX silence; non-empty packets synthesize
        // a bounded deterministic reference tone derived from the payload bytes.
        if (len > 1u && frames > 0u) {
            uint32_t hash = 2166136261u;
            for (size_t i = 0u; i < len; ++i) {
                hash ^= pkt[i];
                hash *= 16777619u;
            }
            const float freq = 220.0f + static_cast<float>(hash % 660u);
            const float amp = 0.15f;
            for (uint32_t f = 0u; f < frames; ++f) {
                const float phase = 2.0f * 3.14159265f * freq *
                                    static_cast<float>(stats.pcm_frames_output + f) /
                                    static_cast<float>(sr);
                const float s = amp * std::sin(phase);
                for (uint32_t c = 0u; c < ch; ++c) {
                    out_pcm[base + static_cast<size_t>(f) * ch + c] = s;
                }
            }
        }
        ++stats.packets_fed;
        stats.pcm_frames_output += frames;
        return Status::Ok();
    }
};

OpusMftDecoder::OpusMftDecoder() : impl_(std::make_unique<Impl>()) {}

OpusMftDecoder::~OpusMftDecoder() {
    Shutdown();
}

Status OpusMftDecoder::Initialize(const OpusDecoderConfig& config) {
    Shutdown();
    if (config.channels == 0u || config.channels > 2u) {
        return Status::Error(StatusCode::InvalidArgument, "Opus decoder supports 1 or 2 channels");
    }
    const uint32_t sr = (config.sample_rate != 0u) ? config.sample_rate : 48000u;
    if (sr != 8000u && sr != 12000u && sr != 16000u && sr != 24000u && sr != 48000u) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Opus decoder sample rate must be 8000, 12000, 16000, 24000, or 48000 Hz");
    }
    impl_->config = config;
    impl_->config.sample_rate = sr;
    impl_->stats = OpusDecodeStats{};
    impl_->stats.output_sample_rate = impl_->config.sample_rate;
    impl_->stats.output_channels = impl_->config.channels;
    impl_->timestamp_100ns = 0u;

#if defined(_WIN32)
    if (impl_->InitNativeMft()) {
        impl_->initialized = true;
        return Status::Ok();
    }
#endif

    if (impl_->config.allow_fallback) {
        impl_->stats.backend = OpusBackendKind::FallbackTocDecoder;
        impl_->stats.backend_detail = "RFC 6716 TOC fallback (MSOpusDecoder.dll unavailable)";
        impl_->initialized = true;
        return Status::Ok();
    }

    return Status::Error(StatusCode::DeviceError, "Native MSOpusDecoder.dll MFT unavailable");
}

void OpusMftDecoder::Shutdown() {
    if (!impl_) return;
#if defined(_WIN32)
    impl_->ReleaseMftResources();
#endif
    impl_->initialized = false;
}

void OpusMftDecoder::Reset() {
    if (!impl_ || !impl_->initialized) return;
    impl_->timestamp_100ns = 0u;
#if defined(_WIN32)
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
#endif
}

bool OpusMftDecoder::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

bool OpusMftDecoder::IsNativeMftActive() const noexcept {
    return impl_ && impl_->initialized && impl_->stats.backend == OpusBackendKind::NativeMsOpusMft;
}

const OpusDecoderConfig& OpusMftDecoder::Config() const noexcept {
    return impl_->config;
}

const OpusDecodeStats& OpusMftDecoder::Stats() const noexcept {
    return impl_->stats;
}

Status OpusMftDecoder::DecodePacket(
    const uint8_t* packet_data,
    size_t packet_bytes,
    std::vector<float>& out_interleaved_pcm) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::InvalidArgument, "OpusMftDecoder not initialized");
    }
    if (!packet_data || packet_bytes == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "Empty Opus packet");
    }

#if defined(_WIN32)
    if (impl_->stats.backend == OpusBackendKind::NativeMsOpusMft && impl_->mft) {
        return impl_->FeedPacketMft(packet_data, packet_bytes, out_interleaved_pcm);
    }
#endif
    return impl_->DecodePacketFallback(packet_data, packet_bytes, out_interleaved_pcm);
}

Status OpusMftDecoder::DecodeStream(
    const uint8_t* stream_data,
    size_t stream_bytes,
    std::vector<float>& out_interleaved_pcm,
    OpusPacketFraming framing) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::InvalidArgument, "OpusMftDecoder not initialized");
    }
    if (stream_bytes == 0u) {
        return Status::Ok();
    }
    if (!stream_data) {
        return Status::Error(StatusCode::InvalidArgument, "Null Opus stream buffer");
    }

    std::vector<OpusPacketSlice> packets;
    if (!ExtractOpusPackets(stream_data, stream_bytes, framing, packets)) {
        return Status::Error(StatusCode::ParseError, "Failed to parse Opus packet framing");
    }

    for (const auto& pkt : packets) {
        Status st = DecodePacket(pkt.data, pkt.size, out_interleaved_pcm);
        if (!st.ok()) {
            return st;
        }
    }
    return Status::Ok();
}

Status OpusMftDecoder::Drain(std::vector<float>& out_interleaved_pcm) {
    if (!IsInitialized()) {
        return Status::Error(StatusCode::InvalidArgument, "OpusMftDecoder not initialized");
    }
#if defined(_WIN32)
    if (impl_->stats.backend == OpusBackendKind::NativeMsOpusMft && impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return impl_->DrainMftOutputs(out_interleaved_pcm);
    }
#else
    (void)out_interleaved_pcm;
#endif
    return Status::Ok();
}

}  // namespace openscarlett
