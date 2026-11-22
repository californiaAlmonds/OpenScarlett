#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "os_check.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

double ComputeSnrDb(const float* ref, const float* test, size_t count, size_t stride, size_t offset) {
    double sig_pow = 0.0;
    double err_pow = 0.0;
    for (size_t i = 0u; i < count; ++i) {
        const double r = static_cast<double>(ref[i * stride + offset]);
        const double t = static_cast<double>(test[i * stride + offset]);
        const double e = r - t;
        sig_pow += r * r;
        err_pow += e * e;
    }
    if (err_pow <= 1e-12) return 120.0;
    return 10.0 * std::log10(sig_pow / err_pow);
}

void SetNullAudioEnv(bool enabled) {
#if defined(_WIN32)
    if (enabled) {
        SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", "1");
    } else {
        SetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", nullptr);
    }
#else
    (void)enabled;
#endif
}

bool HostSupportsNativeOpusMft() {
#if defined(_WIN32)
    // Require BOTH MSOpusDecoder.dll AND a functional Media Foundation runtime (mfplat.dll).
    // Headless Windows Server CI runners may omit Media Foundation even if MSOpusDecoder.dll is absent/stubbed.
    HMODULE opus_mod = LoadLibraryW(L"MSOpusDecoder.dll");
    if (!opus_mod) {
        return false;
    }
    const bool has_class_factory = (GetProcAddress(opus_mod, "DllGetClassObject") != nullptr);
    FreeLibrary(opus_mod);
    if (!has_class_factory) {
        return false;
    }

    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    if (!mfplat) {
        return false;
    }
    using PfnMfStartup = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
    using PfnMfShutdown = HRESULT(STDAPICALLTYPE*)();
    auto fn_start = reinterpret_cast<PfnMfStartup>(
        reinterpret_cast<void*>(GetProcAddress(mfplat, "MFStartup")));
    auto fn_stop = reinterpret_cast<PfnMfShutdown>(
        reinterpret_cast<void*>(GetProcAddress(mfplat, "MFShutdown")));
    bool mf_ok = false;
    if (fn_start && fn_stop) {
        constexpr ULONG kMfVer = (0x0002u << 16u) | 0x0070u;
        if (SUCCEEDED(fn_start(kMfVer, 0x1u))) {
            mf_ok = true;
            fn_stop();
        }
    }
    FreeLibrary(mfplat);
    return mf_ok;
#else
    return false;
#endif
}

// ===========================================================================
// 1. IMA-ADPCM (0x0011 WAV & 0x0069 Xbox ADPCM) Roundtrip Tests
// ===========================================================================
void TestImaAdpcmCodecDirect() {
    std::printf("[Test 1] IMA-ADPCM (0x0011 Mono/Stereo + 0x0069 Xbox ADPCM)...\n");

    // 1A. Mono 48 kHz 440 Hz sine (WAV 0x0011, 256-byte blocks)
    {
        constexpr size_t kFrames = 4040u;  // 8 * 505 samples/block
        std::vector<float> src(kFrames);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i] = 0.65f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        }

        openscarlett::ImaAdpcmConfig cfg{};
        cfg.sample_rate = 48000u;
        cfg.channels = 1u;
        std::vector<uint8_t> encoded;
        uint16_t block_align = 0u;
        uint16_t spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, cfg, encoded, &block_align, &spb)
                       .ok());
        OS_REQUIRE(block_align == 256u && spb == 505u);
        OS_REQUIRE(encoded.size() == 8u * 256u);

        cfg.block_align = block_align;
        cfg.samples_per_block = spb;
        openscarlett::ImaAdpcmDecoder dec;
        OS_REQUIRE(dec.Initialize(cfg).ok());
        std::vector<float> decoded;
        OS_REQUIRE(dec.DecodeBlocks(encoded.data(), encoded.size(), decoded).ok());
        OS_REQUIRE(decoded.size() == kFrames);
        for (float s : decoded) {
            OS_REQUIRE(std::isfinite(s) && s >= -1.0f && s <= 1.0f);
        }
        const double snr = ComputeSnrDb(src.data(), decoded.data(), kFrames, 1u, 0u);
        OS_REQUIRE(snr > 30.0);
        std::printf("  IMA-ADPCM Mono   (0x0011): %zu frames | %zu B -> SNR=%.2f dB\n",
                    kFrames, encoded.size(), snr);
    }

    // 1B. Stereo 48 kHz dual-tone (L=440 Hz, R=660 Hz, 512-byte blocks)
    {
        constexpr size_t kFrames = 4040u;
        std::vector<float> src(kFrames * 2u);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i * 2u + 0u] = 0.60f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
            src[i * 2u + 1u] = 0.55f * std::sin(2.0f * kPi * 660.0f * static_cast<float>(i) / 48000.0f);
        }

        openscarlett::ImaAdpcmConfig cfg{};
        cfg.sample_rate = 48000u;
        cfg.channels = 2u;
        std::vector<uint8_t> encoded;
        uint16_t block_align = 0u;
        uint16_t spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, cfg, encoded, &block_align, &spb)
                       .ok());
        OS_REQUIRE(block_align == 512u && spb == 505u);

        cfg.block_align = block_align;
        cfg.samples_per_block = spb;
        openscarlett::ImaAdpcmDecoder dec;
        OS_REQUIRE(dec.Initialize(cfg).ok());
        std::vector<float> decoded;
        OS_REQUIRE(dec.DecodeBlocks(encoded.data(), encoded.size(), decoded).ok());
        OS_REQUIRE(decoded.size() == kFrames * 2u);

        const double snr_l = ComputeSnrDb(src.data(), decoded.data(), kFrames, 2u, 0u);
        const double snr_r = ComputeSnrDb(src.data(), decoded.data(), kFrames, 2u, 1u);
        OS_REQUIRE(snr_l > 30.0 && snr_r > 30.0);
        std::printf("  IMA-ADPCM Stereo (0x0011): %zu frames | %zu B -> SNR L=%.2f dB, R=%.2f dB\n",
                    kFrames, encoded.size(), snr_l, snr_r);
    }

    // 1C. Xbox ADPCM (0x0069, 36*ch byte blocks = 65 samples/block)
    {
        constexpr size_t kFrames = 65u * 32u;
        std::vector<float> src(kFrames * 2u);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i * 2u + 0u] = 0.50f * std::sin(2.0f * kPi * 523.25f * static_cast<float>(i) / 48000.0f);
            src[i * 2u + 1u] = 0.50f * std::cos(2.0f * kPi * 392.00f * static_cast<float>(i) / 48000.0f);
        }

        openscarlett::ImaAdpcmConfig cfg{};
        cfg.sample_rate = 48000u;
        cfg.channels = 2u;
        cfg.xbox_adpcm = true;
        std::vector<uint8_t> encoded;
        uint16_t block_align = 0u;
        uint16_t spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, cfg, encoded, &block_align, &spb)
                       .ok());
        OS_REQUIRE(block_align == 72u && spb == 65u);

        cfg.block_align = block_align;
        cfg.samples_per_block = spb;
        openscarlett::ImaAdpcmDecoder dec;
        OS_REQUIRE(dec.Initialize(cfg).ok());
        std::vector<float> decoded;
        OS_REQUIRE(dec.DecodeBlocks(encoded.data(), encoded.size(), decoded).ok());
        OS_REQUIRE(decoded.size() == kFrames * 2u);

        const double snr_l = ComputeSnrDb(src.data(), decoded.data(), kFrames, 2u, 0u);
        const double snr_r = ComputeSnrDb(src.data(), decoded.data(), kFrames, 2u, 1u);
        OS_REQUIRE(snr_l > 30.0 && snr_r > 30.0);
        std::printf("  Xbox ADPCM Stereo(0x0069): %zu frames | %zu B -> SNR L=%.2f dB, R=%.2f dB\n",
                    kFrames, encoded.size(), snr_l, snr_r);
    }
}

// ===========================================================================
// 2. Opus (0x704F) Native Windows MSOpusDecoder.dll MFT Direct Test
// ===========================================================================
void TestOpusMftDecoderDirect() {
    std::printf("[Test 2] Opus (0x704F) Native MSOpusDecoder.dll MFT Decoding...\n");

    const bool expect_native_mft = HostSupportsNativeOpusMft();

    for (uint16_t ch : {uint16_t{1u}, uint16_t{2u}}) {
        openscarlett::OpusMftDecoder dec;
        openscarlett::OpusDecoderConfig cfg{};
        cfg.sample_rate = 48000u;
        cfg.channels = ch;
        cfg.pre_skip = 0u;
        cfg.allow_fallback = true;
        OS_REQUIRE(dec.Initialize(cfg).ok());

        if (expect_native_mft) {
            OS_REQUIRE(dec.IsNativeMftActive());
        }

        // Build 8 valid RFC 6716 CELT fullband 20 ms packets (8 * 960 = 7680 frames)
        std::vector<std::vector<uint8_t>> packets;
        for (uint32_t i = 0u; i < 8u; ++i) {
            auto pkt = openscarlett::BuildOpusTestPacket(ch, i, 64u);
            openscarlett::OpusTocInfo toc{};
            OS_REQUIRE(openscarlett::ParseOpusToc(pkt.data(), pkt.size(), toc));
            OS_REQUIRE(toc.samples_per_frame_48k == 960u && toc.frames_in_packet == 1u);
            packets.push_back(std::move(pkt));
        }

        // Test single raw packet decode first
        std::vector<float> single_pcm;
        OS_REQUIRE(dec.DecodePacket(packets[0].data(), packets[0].size(), single_pcm).ok());
        OS_REQUIRE(!single_pcm.empty());

        // Test 2-byte length-prefixed Xbox Opus stream via Auto framing
        dec.Reset();
        const auto stream_u16 = openscarlett::PackOpusLengthPrefixedStream(packets, false);
        std::vector<float> pcm_u16;
        OS_REQUIRE(dec.DecodeStream(
                       stream_u16.data(), stream_u16.size(), pcm_u16,
                       openscarlett::OpusPacketFraming::Auto)
                       .ok());
        OS_REQUIRE(dec.Drain(pcm_u16).ok());

        const auto& st = dec.Stats();
        const uint32_t out_ch = std::max<uint32_t>(1u, st.output_channels);
        const size_t decoded_frames = pcm_u16.size() / out_ch;
        OS_REQUIRE(decoded_frames >= 6u * 960u);

        double energy = 0.0;
        float peak = 0.0f;
        for (float s : pcm_u16) {
            OS_REQUIRE(std::isfinite(s));
            energy += static_cast<double>(s) * static_cast<double>(s);
            peak = std::max(peak, std::fabs(s));
        }
        const double rms = std::sqrt(energy / static_cast<double>(pcm_u16.size()));
        OS_REQUIRE(energy > 0.001 && peak > 0.001f);

        // Also verify 4-byte length-prefixed framing
        dec.Reset();
        const auto stream_u32 = openscarlett::PackOpusLengthPrefixedStream(packets, true);
        std::vector<float> pcm_u32;
        OS_REQUIRE(dec.DecodeStream(
                       stream_u32.data(), stream_u32.size(), pcm_u32,
                       openscarlett::OpusPacketFraming::Auto)
                       .ok());
        OS_REQUIRE(dec.Drain(pcm_u32).ok());
        OS_REQUIRE(pcm_u32.size() >= 6u * 960u * out_ch);

        std::printf(
            "  Opus %uch | backend=%s | out=%uHz %uch | frames=%zu | peak=%.4f | rms=%.4f\n",
            static_cast<unsigned>(ch),
            st.backend_detail.c_str(),
            st.output_sample_rate,
            static_cast<unsigned>(st.output_channels),
            decoded_frames,
            peak,
            rms);
    }
}

// ===========================================================================
// 3. XAudio2_9.dll Headless Deterministic Mix (IMA-ADPCM + Xbox ADPCM + Opus)
// ===========================================================================
void TestXAudio2CodecsDeterministicNullMode() {
    std::printf("[Test 3] XAudio2_9.dll Deterministic Mix (IMA-ADPCM + Xbox ADPCM + Opus 0x704F)...\n");

    SetNullAudioEnv(true);

    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, 48000u) == OS_XAUDIO2_S_OK && master != nullptr);

    OS_XAUDIO2_BACKEND_INFO binfo{};
    OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(binfo.null_audio_forced == 1u);
    OS_REQUIRE(binfo.wasapi_active == 0u);

    // 3A. IMA-ADPCM (0x0011) Stereo Source Voice
    {
        constexpr size_t kFrames = 2020u;
        std::vector<float> src(kFrames * 2u);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i * 2u + 0u] = 0.5f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
            src[i * 2u + 1u] = 0.5f * std::sin(2.0f * kPi * 554.37f * static_cast<float>(i) / 48000.0f);
        }
        openscarlett::ImaAdpcmConfig icfg{};
        icfg.sample_rate = 48000u;
        icfg.channels = 2u;
        std::vector<uint8_t> blocks;
        uint16_t align = 0u;
        uint16_t spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, icfg, blocks, &align, &spb)
                       .ok());

        OS_IMAADPCMWAVEFORMAT fmt{};
        fmt.wfx.wFormatTag = OS_WAVE_FORMAT_IMA_ADPCM;
        fmt.wfx.nChannels = 2u;
        fmt.wfx.nSamplesPerSec = 48000u;
        fmt.wfx.nBlockAlign = align;
        fmt.wfx.wBitsPerSample = 4u;
        fmt.wfx.cbSize = 2u;
        fmt.wSamplesPerBlock = spb;

        IXAudio2SourceVoice* v = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt.wfx) == OS_XAUDIO2_S_OK && v != nullptr);

        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(blocks.size());
        buf.pAudioData = blocks.data();
        OS_REQUIRE(v->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(v->Start() == OS_XAUDIO2_S_OK);

        std::vector<float> mix(kFrames * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), static_cast<uint32_t>(kFrames)) ==
                   OS_XAUDIO2_S_OK);
        const double snr_l = ComputeSnrDb(src.data(), mix.data(), kFrames, 2u, 0u);
        const double snr_r = ComputeSnrDb(src.data(), mix.data(), kFrames, 2u, 1u);
        OS_REQUIRE(snr_l > 30.0 && snr_r > 30.0);

        OS_XAUDIO2_VOICE_STATE vst{};
        v->GetState(&vst);
        OS_REQUIRE(vst.SamplesPlayed == kFrames);
        v->DestroyVoice();
        std::printf("  XAudio2 IMA-ADPCM 0x0011 voice mix: SNR L=%.2f dB, R=%.2f dB\n", snr_l, snr_r);
    }

    // 3A-2. Xbox ADPCM (0x0069) Mono Source Voice
    {
        constexpr size_t kFrames = 65u * 24u;
        std::vector<float> src(kFrames);
        for (size_t i = 0u; i < kFrames; ++i) {
            src[i] = 0.55f * std::sin(2.0f * kPi * 520.0f * static_cast<float>(i) / 48000.0f);
        }
        openscarlett::ImaAdpcmConfig xcfg{};
        xcfg.sample_rate = 48000u;
        xcfg.channels = 1u;
        xcfg.xbox_adpcm = true;
        std::vector<uint8_t> blocks;
        uint16_t align = 0u;
        uint16_t spb = 0u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       src.data(), kFrames, xcfg, blocks, &align, &spb)
                       .ok());

        OS_IMAADPCMWAVEFORMAT fmt{};
        fmt.wfx.wFormatTag = OS_WAVE_FORMAT_XBOX_ADPCM;
        fmt.wfx.nChannels = 1u;
        fmt.wfx.nSamplesPerSec = 48000u;
        fmt.wfx.nBlockAlign = align;
        fmt.wfx.wBitsPerSample = 4u;
        fmt.wfx.cbSize = 2u;
        fmt.wSamplesPerBlock = spb;

        IXAudio2SourceVoice* xv = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&xv, &fmt.wfx) == OS_XAUDIO2_S_OK && xv != nullptr);

        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(blocks.size());
        buf.pAudioData = blocks.data();
        OS_REQUIRE(xv->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(xv->Start() == OS_XAUDIO2_S_OK);

        std::vector<float> mix(kFrames * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), static_cast<uint32_t>(kFrames)) ==
                   OS_XAUDIO2_S_OK);
        std::vector<float> mono_out(kFrames);
        for (size_t i = 0u; i < kFrames; ++i) mono_out[i] = mix[i * 2u];
        const double snr = ComputeSnrDb(src.data(), mono_out.data(), kFrames, 1u, 0u);
        OS_REQUIRE(snr > 30.0);
        xv->DestroyVoice();
        std::printf("  XAudio2 XboxADPCM 0x0069 voice mix: SNR=%.2f dB\n", snr);
    }

    // 3B. Opus (0x704F) Stereo Source Voice (length-prefixed Xbox stream)
    {
        std::vector<std::vector<uint8_t>> packets;
        for (uint32_t i = 0u; i < 6u; ++i) {
            packets.push_back(openscarlett::BuildOpusTestPacket(2u, i + 10u, 64u));
        }
        const auto stream = openscarlett::PackOpusLengthPrefixedStream(packets, false);

        OS_OPUSWAVEFORMAT ofmt{};
        ofmt.wfx.wFormatTag = OS_WAVE_FORMAT_OPUS;
        ofmt.wfx.nChannels = 2u;
        ofmt.wfx.nSamplesPerSec = 48000u;
        ofmt.wfx.wBitsPerSample = 16u;
        ofmt.wfx.cbSize = sizeof(OS_OPUSWAVEFORMAT) - sizeof(OS_WAVEFORMATEX);
        ofmt.PreSkip = 0u;
        ofmt.Version = 1u;

        IXAudio2SourceVoice* ov = nullptr;
        OS_REQUIRE(xa->CreateSourceVoice(&ov, &ofmt.wfx) == OS_XAUDIO2_S_OK && ov != nullptr);

        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(stream.size());
        buf.pAudioData = stream.data();
        OS_REQUIRE(ov->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(ov->Start() == OS_XAUDIO2_S_OK);

        OS_XAUDIO2_BACKEND_INFO binfo2{};
        OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo2) == OS_XAUDIO2_S_OK);
        if (HostSupportsNativeOpusMft()) {
            OS_REQUIRE(binfo2.opus_mft_active_voices == 1u);
        }

        constexpr uint32_t kMixFrames = 4096u;
        std::vector<float> mix(kMixFrames * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix.data(), kMixFrames) == OS_XAUDIO2_S_OK);
        double energy = 0.0;
        float peak = 0.0f;
        for (float s : mix) {
            OS_REQUIRE(std::isfinite(s) && s >= -1.0f && s <= 1.0f);
            energy += static_cast<double>(s) * static_cast<double>(s);
            peak = std::max(peak, std::fabs(s));
        }
        OS_REQUIRE(energy > 0.001 && peak > 0.001f);

        OS_XAUDIO2_VOICE_STATE vst{};
        ov->GetState(&vst);
        OS_REQUIRE(vst.SamplesPlayed == kMixFrames);
        ov->DestroyVoice();
        std::printf("  XAudio2 Opus 0x704F voice mix     : frames=%u | energy=%.3f | peak=%.4f | MFT=%u\n",
                    kMixFrames, energy, peak, binfo2.opus_mft_active_voices);
    }

    master->DestroyVoice();
    xa->Release();
}

// ===========================================================================
// 4. XAudio2_9.dll Live Host WASAPI Playback (Graceful CI Fallback)
// ===========================================================================
void TestXAudio2LiveWasapi() {
    std::printf("[Test 4] XAudio2_9.dll Live Host WASAPI Playback (quiet 40 ms burst)...\n");

    SetNullAudioEnv(false);

    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, 48000u) == OS_XAUDIO2_S_OK && master != nullptr);
    OS_REQUIRE(master->SetVolume(0.02f) == OS_XAUDIO2_S_OK);  // Very quiet so test is unobtrusive

    // Prepare a smooth 40 ms 440 Hz tone with raised-cosine fade in/out.
    constexpr uint32_t kToneFrames = 1920u;  // 40 ms @ 48 kHz
    std::vector<float> tone(kToneFrames * 2u);
    for (uint32_t i = 0u; i < kToneFrames; ++i) {
        const float env = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kToneFrames)));
        const float s = env * 0.25f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        tone[i * 2u + 0u] = s;
        tone[i * 2u + 1u] = s;
    }

    OS_WAVEFORMATEX fmt{};
    fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2u;
    fmt.nSamplesPerSec = 48000u;
    fmt.wBitsPerSample = 32u;
    fmt.nBlockAlign = 8u;
    fmt.nAvgBytesPerSec = 48000u * 8u;

    IXAudio2SourceVoice* v = nullptr;
    OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt) == OS_XAUDIO2_S_OK && v != nullptr);

    OS_XAUDIO2_BUFFER buf{};
    buf.Flags = OS_XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = static_cast<uint32_t>(tone.size() * sizeof(float));
    buf.pAudioData = reinterpret_cast<const uint8_t*>(tone.data());
    OS_REQUIRE(v->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(v->Start() == OS_XAUDIO2_S_OK);

    // Wait ~80 ms so WASAPI renders multiple shared-mode periods.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    OS_XAUDIO2_BACKEND_INFO binfo{};
    OS_REQUIRE(OpenScarlettXAudio2GetBackendInfo(xa, &binfo) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(binfo.null_audio_forced == 0u);

    if (binfo.wasapi_active != 0u) {
        OS_REQUIRE(binfo.wasapi_sample_rate >= 8000u);
        OS_REQUIRE(binfo.wasapi_channels >= 1u);
        OS_REQUIRE(binfo.wasapi_buffer_frames > 0u);
        OS_REQUIRE(binfo.wasapi_frames_rendered > 0ull);
        std::printf(
            "  WASAPI LIVE : active=1 | mode=%s | %uHz %uch %u-bit | buf=%u frames | rendered=%llu frames\n",
            binfo.wasapi_event_driven ? "event-driven" : "polling",
            binfo.wasapi_sample_rate,
            binfo.wasapi_channels,
            binfo.wasapi_bits_per_sample,
            binfo.wasapi_buffer_frames,
            static_cast<unsigned long long>(binfo.wasapi_frames_rendered));
    } else {
        std::printf(
            "  WASAPI INFO : no host render endpoint (hr=0x%08X); verified graceful silent-mixer fallback\n",
            static_cast<unsigned>(binfo.wasapi_last_hresult));
        std::vector<float> fallback_mix(512u * 2u, 0.0f);
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, fallback_mix.data(), 512u) == OS_XAUDIO2_S_OK);
    }

    const auto t_close0 = std::chrono::steady_clock::now();
    v->DestroyVoice();
    master->DestroyVoice();
    xa->Release();
    const auto teardown_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t_close0)
                                 .count();
    OS_REQUIRE(teardown_ms < 1000);
    std::printf("  WASAPI Clean teardown: %lld ms\n", static_cast<long long>(teardown_ms));
}

}  // namespace

int main() {
    TestImaAdpcmCodecDirect();
    TestOpusMftDecoderDirect();
    TestXAudio2CodecsDeterministicNullMode();
    TestXAudio2LiveWasapi();
    std::printf("ALL OPUS / IMA-ADPCM / LIVE WASAPI TESTS PASSED!\n");
    return 0;
}
