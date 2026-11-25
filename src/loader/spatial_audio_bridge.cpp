#include "spatial_audio_bridge.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <share.h>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace openscarlett::loader {
namespace {

constexpr uint32_t kDefaultSampleRate = 48000u;
constexpr uint32_t kDefaultFramesPerQuantum = 480u;  // 10 ms @ 48 kHz

bool IsNullAudioEnabled() noexcept {
    char buf[16]{};
    const DWORD n = ::GetEnvironmentVariableA("OPENSCARLETT_AUDIO_NULL", buf, sizeof(buf));
    return n > 0u && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T' || buf[0] == 'y');
}

bool IsAudioTraceEnabled() noexcept {
    static const bool s_en = []() {
        char buf[16]{};
        if (::GetEnvironmentVariableA("OPENSCARLETT_TRACE_AUDIO", buf, sizeof(buf)) > 0 && buf[0] != '0') {
            return true;
        }
        if (::GetEnvironmentVariableA("OPENSCARLETT_TRACE_RETAIL", buf, sizeof(buf)) > 0 && buf[0] != '0') {
            return true;
        }
        return false;
    }();
    return s_en;
}

std::filesystem::path GetRecordWavPath() noexcept {
    wchar_t buf[2048]{};
    const DWORD len = ::GetEnvironmentVariableW(
        L"OPENSCARLETT_RECORD_WAV", buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0u || len >= std::size(buf)) {
        return {};
    }
    std::wstring raw(buf, len);
    if (raw.size() >= 2u && raw.front() == L'"' && raw.back() == L'"') {
        raw = raw.substr(1u, raw.size() - 2u);
    }
    return raw.empty() ? std::filesystem::path{} : std::filesystem::path(raw);
}

class SpatialWavRecorder {
public:
    void AppendStereoFloat(
        const float* interleaved_lr,
        uint32_t frames,
        uint32_t sample_rate,
        bool had_stream_underrun = false) {
        if (!interleaved_lr || frames == 0u) return;
        const std::filesystem::path req_path = GetRecordWavPath();
        if (req_path.empty()) return;

        std::lock_guard<std::mutex> lk(mu_);
        if (!EnsureOpenLocked(req_path, sample_rate)) return;

        if (had_stream_underrun && seen_active_) {
            underrun_in_silent_run_ = true;
        }
        scratch_.resize(static_cast<size_t>(frames) * 2u);
        for (uint32_t f = 0u; f < frames; ++f) {
            const float l = std::clamp(interleaved_lr[f * 2u + 0u], -1.0f, 1.0f);
            const float r = std::clamp(interleaved_lr[f * 2u + 1u], -1.0f, 1.0f);
            const float pk = std::max(std::fabs(l), std::fabs(r));
            if (pk > peak_abs_) peak_abs_ = pk;
            if (pk > cur_win_peak_) cur_win_peak_ = pk;
            sum_sq_ += static_cast<double>(l) * l + static_cast<double>(r) * r;
            if (++cur_win_frames_ >= win_target_) {
                if (cur_win_peak_ > 1e-4f) {
                    if (seen_active_ && silent_run_ > 0u && silent_run_ <= 20u &&
                        underrun_in_silent_run_) {
                        silent_holes_in_bursts_ += silent_run_;
                    }
                    seen_active_ = true;
                    silent_run_ = 0u;
                    underrun_in_silent_run_ = false;
                    ++active_10ms_windows_;
                } else if (seen_active_) {
                    ++silent_run_;
                    if (silent_run_ > 20u) {
                        underrun_in_silent_run_ = false;
                    }
                }
                cur_win_frames_ = 0u;
                cur_win_peak_ = 0.0f;
            }
            scratch_[f * 2u + 0u] = static_cast<int16_t>(std::lround(l * 32767.0f));
            scratch_[f * 2u + 1u] = static_cast<int16_t>(std::lround(r * 32767.0f));
        }

        const size_t wr = std::fwrite(scratch_.data(), sizeof(int16_t) * 2u, frames, fp_);
        total_frames_ += static_cast<uint64_t>(wr);
        frames_since_flush_ += static_cast<uint64_t>(wr);
        if (frames_since_flush_ >= std::max<uint32_t>(1u, sample_rate_ / 2u)) {
            frames_since_flush_ = 0ull;
            FlushHeaderAndJsonLocked();
        }
    }

    void Flush() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        if (fp_) {
            FlushHeaderAndJsonLocked();
        }
    }

private:
    bool EnsureOpenLocked(const std::filesystem::path& path, uint32_t sr) {
        if (fp_ && path == path_) return true;
        if (fp_) {
            FlushHeaderAndJsonLocked();
            std::fclose(fp_);
            fp_ = nullptr;
        }
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        fp_ = ::_wfsopen(path.c_str(), L"wb", _SH_DENYNO);
        if (!fp_) return false;
        path_ = path;
        sample_rate_ = (sr >= 8000u) ? sr : kDefaultSampleRate;
        win_target_ = std::max<uint32_t>(1u, sample_rate_ / 100u);
        total_frames_ = 0ull;
        frames_since_flush_ = 0ull;
        peak_abs_ = 0.0f;
        sum_sq_ = 0.0;
        cur_win_frames_ = 0u;
        cur_win_peak_ = 0.0f;
        seen_active_ = false;
        silent_run_ = 0u;
        underrun_in_silent_run_ = false;
        active_10ms_windows_ = 0ull;
        silent_holes_in_bursts_ = 0ull;
        WriteWavHeaderLocked();
        FlushHeaderAndJsonLocked();
        return true;
    }

    void WriteWavHeaderLocked() {
        if (!fp_) return;
        const uint64_t data_bytes64 = std::min<uint64_t>(total_frames_ * 4ull, 0xFFFFFF00ull);
        const uint32_t data_bytes = static_cast<uint32_t>(data_bytes64);
        const uint32_t riff_bytes = 36u + data_bytes;
        const uint32_t byte_rate = sample_rate_ * 4u;
        const uint16_t block_align = 4u;
        const uint16_t bits = 16u;
        const uint16_t ch = 2u;
        const uint16_t pcm_tag = 1u;
        const uint32_t fmt_sz = 16u;
        uint8_t hdr[44]{};
        std::memcpy(hdr + 0, "RIFF", 4);
        std::memcpy(hdr + 4, &riff_bytes, 4);
        std::memcpy(hdr + 8, "WAVEfmt ", 8);
        std::memcpy(hdr + 16, &fmt_sz, 4);
        std::memcpy(hdr + 20, &pcm_tag, 2);
        std::memcpy(hdr + 22, &ch, 2);
        std::memcpy(hdr + 24, &sample_rate_, 4);
        std::memcpy(hdr + 28, &byte_rate, 4);
        std::memcpy(hdr + 32, &block_align, 2);
        std::memcpy(hdr + 34, &bits, 2);
        std::memcpy(hdr + 36, "data", 4);
        std::memcpy(hdr + 40, &data_bytes, 4);
        ::_fseeki64(fp_, 0, SEEK_SET);
        (void)std::fwrite(hdr, 1u, sizeof(hdr), fp_);
        ::_fseeki64(fp_, 0, SEEK_END);
        std::fflush(fp_);
    }

    void FlushHeaderAndJsonLocked() {
        WriteWavHeaderLocked();
        const std::filesystem::path json_path(path_.wstring() + L".json");
        FILE* jfp = ::_wfsopen(json_path.c_str(), L"wb", _SH_DENYNO);
        if (jfp) {
            const double rms = (total_frames_ > 0ull)
                                   ? std::sqrt(sum_sq_ / static_cast<double>(total_frames_ * 2ull))
                                   : 0.0;
            char jbuf[640]{};
            const int n = std::snprintf(
                jbuf, sizeof(jbuf),
                "{\n"
                "  \"source\": \"spatial_audio_bridge\",\n"
                "  \"sample_rate\": %u,\n"
                "  \"channels\": 2,\n"
                "  \"total_frames\": %llu,\n"
                "  \"duration_sec\": %.3f,\n"
                "  \"rms\": %.6f,\n"
                "  \"peak_abs\": %.6f,\n"
                "  \"active_10ms_windows\": %llu,\n"
                "  \"silent_holes_in_bursts\": %llu\n"
                "}\n",
                sample_rate_,
                static_cast<unsigned long long>(total_frames_),
                static_cast<double>(total_frames_) / static_cast<double>(sample_rate_),
                rms,
                static_cast<double>(peak_abs_),
                static_cast<unsigned long long>(active_10ms_windows_),
                static_cast<unsigned long long>(silent_holes_in_bursts_));
            if (n > 0) {
                (void)std::fwrite(jbuf, 1u, static_cast<size_t>(std::min<int>(n, sizeof(jbuf) - 1)), jfp);
            }
            std::fflush(jfp);
            std::fclose(jfp);
        }
    }

    std::mutex mu_;
    FILE* fp_ = nullptr;
    std::filesystem::path path_;
    uint32_t sample_rate_ = kDefaultSampleRate;
    uint32_t win_target_ = 480u;
    uint64_t total_frames_ = 0ull;
    uint64_t frames_since_flush_ = 0ull;
    float peak_abs_ = 0.0f;
    double sum_sq_ = 0.0;
    uint32_t cur_win_frames_ = 0u;
    float cur_win_peak_ = 0.0f;
    bool seen_active_ = false;
    uint32_t silent_run_ = 0u;
    bool underrun_in_silent_run_ = false;
    uint64_t active_10ms_windows_ = 0ull;
    uint64_t silent_holes_in_bursts_ = 0ull;
    std::vector<int16_t> scratch_;
};

class HostWasapiSpatialRenderStream;

class SpatialMasterEngine {
public:
    static SpatialMasterEngine& Instance() {
        static SpatialMasterEngine* s_eng = new SpatialMasterEngine();
        return *s_eng;
    }

    void RegisterStream(HostWasapiSpatialRenderStream* stream, IMMDevice* endpoint);
    void UnregisterStream(HostWasapiSpatialRenderStream* stream);
    void SubmitStreamQuantum(HostWasapiSpatialRenderStream* stream, const float* lr, uint32_t frames);
    void FlushWav() noexcept { wav_recorder_.Flush(); }

private:
    SpatialMasterEngine() = default;
    void EnsureWorkerStartedLocked(IMMDevice* endpoint);
    void WorkerLoop();

    struct StreamSlot {
        HostWasapiSpatialRenderStream* stream = nullptr;
        std::deque<std::vector<float>> pending_quanta;
        bool primed = false;
        uint32_t single_quantum_ticks = 0u;
        uint32_t empty_ticks = 0u;
    };

    std::mutex mu_;
    std::vector<StreamSlot> streams_;
    IMMDevice* endpoint_ = nullptr;
    HANDLE quantum_ready_event_ = nullptr;
    bool worker_started_ = false;
    SpatialWavRecorder wav_recorder_;
};

class HostAudioFormatEnumerator final : public IAudioFormatEnumerator {
public:
    static const WAVEFORMATEX* CanonicalFormat() noexcept {
        static const WAVEFORMATEX kFmt = {
            WAVE_FORMAT_IEEE_FLOAT,
            1u,
            kDefaultSampleRate,
            kDefaultSampleRate * sizeof(float),
            sizeof(float),
            32u,
            0u};
        return &kFmt;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(IAudioFormatEnumerator))) {
            AddRef();
            *ppvObject = static_cast<IAudioFormatEnumerator*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetCount(UINT32* count) override {
        if (!count) return E_POINTER;
        *count = 1u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(UINT32 index, WAVEFORMATEX** format) override {
        if (!format) return E_POINTER;
        if (index != 0u) {
            *format = nullptr;
            return E_INVALIDARG;
        }
        *format = const_cast<WAVEFORMATEX*>(CanonicalFormat());
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
};

class HostWasapiSpatialObject final : public ISpatialAudioObject {
public:
    static constexpr uint32_t kUntouchedFloatSentinel = 0x7FC0DE77u;

    HostWasapiSpatialObject(HostWasapiSpatialRenderStream* owner,
                            AudioObjectType type,
                            uint32_t frame_count,
                            bool is_pcm16)
        : owner_(owner),
          type_(type),
          frame_count_(frame_count),
          is_pcm16_(is_pcm16),
          buffer_(static_cast<size_t>(frame_count) * (is_pcm16 ? 2u : 4u), 0u) {
        ResetBufferToSentinel();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ISpatialAudioObjectBase)) ||
            IsEqualIID(riid, __uuidof(ISpatialAudioObject))) {
            AddRef();
            *ppvObject = static_cast<ISpatialAudioObject*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** buffer, UINT32* bufferLength) override {
        if (!buffer || !bufferLength) return E_POINTER;
        if (!active_.load(std::memory_order_acquire)) {
            *buffer = nullptr;
            *bufferLength = 0u;
            return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
        }
        buffer_touched_.store(true, std::memory_order_release);
        *buffer = buffer_.data();
        *bufferLength = static_cast<UINT32>(buffer_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEndOfStream(UINT32 /*frameCount*/) override {
        active_.store(false, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsActive(BOOL* isActive) override {
        if (!isActive) return E_POINTER;
        *isActive = active_.load(std::memory_order_acquire) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAudioObjectType(AudioObjectType* audioObjectType) override {
        if (!audioObjectType) return E_POINTER;
        *audioObjectType = type_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPosition(float x, float y, float z) override {
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
            pos_x_.store(x, std::memory_order_relaxed);
            pos_y_.store(y, std::memory_order_relaxed);
            pos_z_.store(z, std::memory_order_relaxed);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVolume(float volume) override {
        if (std::isfinite(volume)) {
            volume_.store(std::clamp(volume, 0.0f, 4.0f), std::memory_order_relaxed);
        }
        return S_OK;
    }

    bool MixAndClearIntoStereo(float* dst_lr, uint32_t frames) noexcept {
        if (!active_.load(std::memory_order_acquire)) return false;
        const bool touched = buffer_touched_.exchange(false, std::memory_order_acq_rel);
        if (!touched) return false;

        const uint32_t n = std::min<uint32_t>(frames, frame_count_);
        if (!is_pcm16_ && n > 0u) {
            const auto* raw_u32 = reinterpret_cast<const uint32_t*>(buffer_.data());
            if (raw_u32[0] == kUntouchedFloatSentinel &&
                raw_u32[n / 2u] == kUntouchedFloatSentinel &&
                raw_u32[n - 1u] == kUntouchedFloatSentinel) {
                return false;
            }
        }

        const float vol = volume_.load(std::memory_order_relaxed);
        float gl = 0.70710678f * vol;
        float gr = 0.70710678f * vol;
        const uint32_t t = static_cast<uint32_t>(type_);
        if (t == static_cast<uint32_t>(AudioObjectType_FrontLeft)) {
            gl = 1.0f * vol;
            gr = 0.0f;
        } else if (t == static_cast<uint32_t>(AudioObjectType_FrontRight)) {
            gl = 0.0f;
            gr = 1.0f * vol;
        } else if (t == static_cast<uint32_t>(AudioObjectType_FrontCenter)) {
            gl = 0.70710678f * vol;
            gr = 0.70710678f * vol;
        } else if (t == static_cast<uint32_t>(AudioObjectType_LowFrequency)) {
            gl = 0.5f * vol;
            gr = 0.5f * vol;
        } else if ((t & static_cast<uint32_t>(AudioObjectType_SideLeft | AudioObjectType_BackLeft |
                                              AudioObjectType_TopFrontLeft | AudioObjectType_TopBackLeft)) != 0u) {
            gl = 0.70710678f * vol;
            gr = 0.20f * vol;
        } else if ((t & static_cast<uint32_t>(AudioObjectType_SideRight | AudioObjectType_BackRight |
                                              AudioObjectType_TopFrontRight | AudioObjectType_TopBackRight)) != 0u) {
            gl = 0.20f * vol;
            gr = 0.70710678f * vol;
        } else if (t == static_cast<uint32_t>(AudioObjectType_Dynamic)) {
            const float px = pos_x_.load(std::memory_order_relaxed);
            const float py = pos_y_.load(std::memory_order_relaxed);
            const float pz = pos_z_.load(std::memory_order_relaxed);
            const float dist = std::sqrt(px * px + py * py + pz * pz);
            const float atten = (dist > 1.0f) ? (1.0f / (1.0f + 0.08f * (dist - 1.0f))) : 1.0f;
            const float pan = (dist > 1e-4f) ? std::clamp(px / dist, -1.0f, 1.0f) : 0.0f;
            const float angle = (pan + 1.0f) * 0.78539816f;  // [0, pi/2]
            gl = std::cos(angle) * vol * atten;
            gr = std::sin(angle) * vol * atten;
        }

        if (!is_pcm16_) {
            const auto* src = reinterpret_cast<const float*>(buffer_.data());
            for (uint32_t i = 0u; i < n; ++i) {
                const float s = std::isfinite(src[i]) ? src[i] : 0.0f;
                dst_lr[i * 2u + 0u] += s * gl;
                dst_lr[i * 2u + 1u] += s * gr;
            }
        } else {
            const auto* src = reinterpret_cast<const int16_t*>(buffer_.data());
            for (uint32_t i = 0u; i < n; ++i) {
                const float s = static_cast<float>(src[i]) / 32768.0f;
                dst_lr[i * 2u + 0u] += s * gl;
                dst_lr[i * 2u + 1u] += s * gr;
            }
        }
        ResetBufferToSentinel();
        return true;
    }

    void ResetBufferToSentinel() noexcept {
        if (!is_pcm16_) {
            auto* dst = reinterpret_cast<uint32_t*>(buffer_.data());
            const size_t cnt = buffer_.size() / sizeof(uint32_t);
            for (size_t i = 0u; i < cnt; ++i) {
                dst[i] = kUntouchedFloatSentinel;
            }
        } else {
            std::memset(buffer_.data(), 0, buffer_.size());
        }
    }

    bool IsDynamic() const noexcept { return type_ == AudioObjectType_Dynamic; }
    bool IsObjectActive() const noexcept { return active_.load(std::memory_order_acquire); }
    void DetachOwner() noexcept {
        active_.store(false, std::memory_order_release);
        owner_ = nullptr;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    HostWasapiSpatialRenderStream* owner_ = nullptr;
    AudioObjectType type_ = AudioObjectType_None;
    uint32_t frame_count_ = kDefaultFramesPerQuantum;
    bool is_pcm16_ = false;
    std::atomic<bool> active_{true};
    std::atomic<bool> buffer_touched_{false};
    std::atomic<float> pos_x_{0.0f};
    std::atomic<float> pos_y_{0.0f};
    std::atomic<float> pos_z_{1.0f};
    std::atomic<float> volume_{1.0f};
    std::vector<uint8_t> buffer_;
};

class HostWasapiSpatialRenderStream final : public ISpatialAudioObjectRenderStream {
public:
    HostWasapiSpatialRenderStream(IMMDevice* endpoint,
                                  uint32_t max_dyn_objects,
                                  uint32_t frame_count,
                                  bool is_pcm16,
                                  HANDLE guest_event)
        : max_dyn_objects_(std::clamp<uint32_t>(max_dyn_objects, 0u, 256u)),
          frame_count_(std::max<uint32_t>(64u, frame_count)),
          is_pcm16_(is_pcm16),
          mix_scratch_(static_cast<size_t>(std::max<uint32_t>(64u, frame_count)) * 2u, 0.0f) {
        if (guest_event && guest_event != INVALID_HANDLE_VALUE) {
            (void)::DuplicateHandle(
                ::GetCurrentProcess(), guest_event,
                ::GetCurrentProcess(), &dup_event_,
                0u, FALSE, DUPLICATE_SAME_ACCESS);
        }
        SpatialMasterEngine::Instance().RegisterStream(this, endpoint);
    }

    ~HostWasapiSpatialRenderStream() {
        started_.store(false, std::memory_order_release);
        SpatialMasterEngine::Instance().UnregisterStream(this);
        {
            std::lock_guard<std::mutex> lk(obj_mu_);
            for (auto* obj : objects_) {
                if (obj) obj->DetachOwner();
            }
            objects_.clear();
        }
        if (dup_event_) {
            ::CloseHandle(dup_event_);
            dup_event_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ISpatialAudioObjectRenderStreamBase)) ||
            IsEqualIID(riid, __uuidof(ISpatialAudioObjectRenderStream))) {
            AddRef();
            *ppvObject = static_cast<ISpatialAudioObjectRenderStream*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetAvailableDynamicObjectCount(UINT32* value) override {
        if (!value) return E_POINTER;
        std::lock_guard<std::mutex> lk(obj_mu_);
        uint32_t used = 0u;
        for (auto* o : objects_) {
            if (o && o->IsDynamic() && o->IsObjectActive()) ++used;
        }
        *value = (used < max_dyn_objects_) ? (max_dyn_objects_ - used) : 0u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID /*riid*/, void** service) override {
        if (!service) return E_POINTER;
        *service = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        started_.store(true, std::memory_order_release);
        if (dup_event_) {
            ::SetEvent(dup_event_);
        }
        if (IsAudioTraceEnabled()) {
            std::fprintf(stderr, "[OpenScarlett][SpatialAudio] Stream::Start(frames=%u, maxDyn=%u)\n",
                         frame_count_, max_dyn_objects_);
            std::fflush(stderr);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        started_.store(false, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        if (started_.load(std::memory_order_acquire)) {
            return SPTLAUDCLNT_E_OUT_OF_ORDER;
        }
        updating_.store(false, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginUpdatingAudioObjects(
        UINT32* availableDynamicObjectCount,
        UINT32* frameCountPerBuffer) override {
        if (!availableDynamicObjectCount || !frameCountPerBuffer) return E_POINTER;
        updating_.store(true, std::memory_order_release);
        (void)GetAvailableDynamicObjectCount(availableDynamicObjectCount);
        *frameCountPerBuffer = frame_count_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUpdatingAudioObjects() override {
        updating_.store(false, std::memory_order_release);
        std::fill(mix_scratch_.begin(), mix_scratch_.end(), 0.0f);
        bool any_written = false;
        {
            std::lock_guard<std::mutex> lk(obj_mu_);
            for (auto* obj : objects_) {
                if (obj && obj->MixAndClearIntoStereo(mix_scratch_.data(), frame_count_)) {
                    any_written = true;
                }
            }
        }
        if (any_written) {
            SpatialMasterEngine::Instance().SubmitStreamQuantum(this, mix_scratch_.data(), frame_count_);
        } else {
            // Guest polled GetBuffer before its internal mixer ringbuffer reached >= 1 quantum;
            // re-arm tick event instead of pushing a false 10ms zero hole!
            SignalTickEvent();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateSpatialAudioObject(
        AudioObjectType type,
        ISpatialAudioObject** audioObject) override {
        if (!audioObject) return E_POINTER;
        *audioObject = nullptr;
        auto* obj = new (std::nothrow) HostWasapiSpatialObject(this, type, frame_count_, is_pcm16_);
        if (!obj) return E_OUTOFMEMORY;
        {
            std::lock_guard<std::mutex> lk(obj_mu_);
            objects_.push_back(obj);
        }
        *audioObject = obj;
        return S_OK;
    }

    void OnObjectDestroyed(HostWasapiSpatialObject* obj) noexcept {
        std::lock_guard<std::mutex> lk(obj_mu_);
        objects_.erase(std::remove(objects_.begin(), objects_.end(), obj), objects_.end());
    }

    bool IsStarted() const noexcept { return started_.load(std::memory_order_acquire); }
    void SignalTickEvent() noexcept {
        if (started_.load(std::memory_order_acquire) && dup_event_) {
            ::SetEvent(dup_event_);
        }
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    uint32_t max_dyn_objects_ = 128u;
    uint32_t frame_count_ = kDefaultFramesPerQuantum;
    bool is_pcm16_ = false;
    HANDLE dup_event_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> updating_{false};
    std::mutex obj_mu_;
    std::vector<HostWasapiSpatialObject*> objects_;
    std::vector<float> mix_scratch_;
};

ULONG STDMETHODCALLTYPE HostWasapiSpatialObject::Release() {
    const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
    if (n == 0u) {
        if (owner_) owner_->OnObjectDestroyed(this);
        delete this;
    }
    return n;
}

void SpatialMasterEngine::RegisterStream(HostWasapiSpatialRenderStream* stream, IMMDevice* endpoint) {
    ::SetEnvironmentVariableA("OPENSCARLETT_SPATIAL_WAV_CLAIMED", "1");
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& s : streams_) {
        if (s.stream == stream) return;
    }
    streams_.push_back(StreamSlot{stream, {}});
    EnsureWorkerStartedLocked(endpoint);
}

void SpatialMasterEngine::UnregisterStream(HostWasapiSpatialRenderStream* stream) {
    std::lock_guard<std::mutex> lk(mu_);
    streams_.erase(
        std::remove_if(streams_.begin(), streams_.end(),
                       [stream](const StreamSlot& s) { return s.stream == stream; }),
        streams_.end());
}

void SpatialMasterEngine::SubmitStreamQuantum(
    HostWasapiSpatialRenderStream* stream, const float* lr, uint32_t frames) {
    if (!lr || frames == 0u) return;
    HANDLE ready_ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : streams_) {
            if (s.stream == stream) {
                if (s.pending_quanta.size() >= 8u) {
                    s.pending_quanta.pop_front();
                }
                s.pending_quanta.emplace_back(lr, lr + static_cast<size_t>(frames) * 2u);
                if (s.pending_quanta.size() < 3u && s.stream) {
                    s.stream->SignalTickEvent();
                }
                ready_ev = quantum_ready_event_;
                break;
            }
        }
    }
    if (ready_ev) {
        ::SetEvent(ready_ev);
    }
}

void SpatialMasterEngine::EnsureWorkerStartedLocked(IMMDevice* endpoint) {
    if (endpoint && !endpoint_) {
        endpoint_ = endpoint;
        endpoint_->AddRef();
    }
    if (worker_started_) return;
    worker_started_ = true;
    if (!quantum_ready_event_) {
        quantum_ready_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    // Request 0.5 ms Windows timer resolution so RAGE9's Sleep(9) wakes in ~9.0 ms instead of 15.625 ms!
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        using PfnNtSetTimerResolution = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
        if (auto fn = reinterpret_cast<PfnNtSetTimerResolution>(
                ::GetProcAddress(ntdll, "NtSetTimerResolution"))) {
            ULONG actual_res = 0u;
            (void)fn(5000u, TRUE, &actual_res);
        }
    }
    std::thread([this]() { WorkerLoop(); }).detach();
}

void SpatialMasterEngine::WorkerLoop() {
    (void)::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    IAudioClient* audio_client = nullptr;
    IAudioRenderClient* render_client = nullptr;
    UINT32 buffer_frames = 0u;

    if (!IsNullAudioEnabled()) {
        IMMDevice* ep = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ep = endpoint_;
            if (ep) ep->AddRef();
        }
        if (ep) {
            if (SUCCEEDED(ep->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(&audio_client))) &&
                audio_client) {
                WAVEFORMATEX wf{};
                wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
                wf.nChannels = 2u;
                wf.nSamplesPerSec = kDefaultSampleRate;
                wf.wBitsPerSample = 32u;
                wf.nBlockAlign = 8u;
                wf.nAvgBytesPerSec = kDefaultSampleRate * 8u;
                wf.cbSize = 0u;
                const HRESULT hr_init = audio_client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    800000LL,  // 80 ms shared-mode cushion for zero underruns
                    0LL,
                    &wf,
                    nullptr);
                if (SUCCEEDED(hr_init) &&
                    SUCCEEDED(audio_client->GetBufferSize(&buffer_frames)) &&
                    SUCCEEDED(audio_client->GetService(__uuidof(IAudioRenderClient),
                                                       reinterpret_cast<void**>(&render_client))) &&
                    render_client) {
                    BYTE* prefill = nullptr;
                    const UINT32 pre_frames = std::min<UINT32>(buffer_frames, kDefaultFramesPerQuantum * 2u);
                    if (pre_frames > 0u && SUCCEEDED(render_client->GetBuffer(pre_frames, &prefill))) {
                        (void)render_client->ReleaseBuffer(pre_frames, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                    (void)audio_client->Start();
                    std::fprintf(stderr,
                                 "[OpenScarlett][SpatialAudio] Live WASAPI stereo output started (48000 Hz, buf=%u frames)\n",
                                 buffer_frames);
                    std::fflush(stderr);
                } else {
                    if (render_client) { render_client->Release(); render_client = nullptr; }
                    audio_client->Release();
                    audio_client = nullptr;
                }
            }
            ep->Release();
        }
    }

    HANDLE timer = ::CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    LARGE_INTEGER qpc_freq{}, next_qpc{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&next_qpc);
    const int64_t step_qpc = std::max<int64_t>(1LL, qpc_freq.QuadPart / 100LL);  // exact 10.0 ms

    std::vector<float> master_lr(kDefaultFramesPerQuantum * 2u, 0.0f);

    for (;;) {
        next_qpc.QuadPart += step_qpc;
        LARGE_INTEGER now_qpc{};
        ::QueryPerformanceCounter(&now_qpc);
        const int64_t rem_qpc = next_qpc.QuadPart - now_qpc.QuadPart;
        if (rem_qpc > 0) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -(rem_qpc * 10000000LL / qpc_freq.QuadPart);
                if (due.QuadPart < 0) {
                    ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    ::WaitForSingleObject(timer, 15u);
                }
            } else {
                ::Sleep(5u);
            }
        } else if (rem_qpc < -step_qpc * 4LL) {
            next_qpc = now_qpc;
        }

        // Phase-lock gently to WASAPI hardware DAC clock so buffer never drifts empty or full!
        if (audio_client && buffer_frames >= kDefaultFramesPerQuantum * 4u) {
            UINT32 padding = 0u;
            if (SUCCEEDED(audio_client->GetCurrentPadding(&padding))) {
                // Keep ~20-30 ms (960-1440 frames) buffered; nudge next_qpc by +/- 0.15 ms if drifting.
                if (padding > kDefaultFramesPerQuantum * 4u) {
                    next_qpc.QuadPart += step_qpc / 50LL;
                } else if (padding < kDefaultFramesPerQuantum) {
                    next_qpc.QuadPart -= step_qpc / 50LL;
                }
            }
        }

        bool any_started = false;
        bool need_jitter_wait = false;
        HANDLE ready_ev = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ready_ev = quantum_ready_event_;
            for (auto& s : streams_) {
                if (!s.stream || !s.stream->IsStarted()) continue;
                any_started = true;
                if (s.primed && s.pending_quanta.empty() && s.empty_ticks < 2u) {
                    need_jitter_wait = true;
                    s.stream->SignalTickEvent();
                }
            }
        }
        if (need_jitter_wait && ready_ev) {
            (void)::WaitForSingleObject(ready_ev, 4u);
        }

        std::fill(master_lr.begin(), master_lr.end(), 0.0f);
        bool had_stream_underrun = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& s : streams_) {
                if (!s.stream || !s.stream->IsStarted()) continue;
                if (!s.primed) {
                    if (s.pending_quanta.size() >= 4u ||
                        (!s.pending_quanta.empty() && ++s.single_quantum_ticks >= 3u)) {
                        s.primed = true;
                        s.single_quantum_ticks = 0u;
                    } else {
                        s.stream->SignalTickEvent();
                        continue;
                    }
                }
                if (!s.pending_quanta.empty()) {
                    const auto& q = s.pending_quanta.front();
                    const size_t n = std::min<size_t>(master_lr.size(), q.size());
                    for (size_t i = 0u; i < n; ++i) {
                        master_lr[i] += q[i];
                    }
                    s.pending_quanta.pop_front();
                    s.empty_ticks = 0u;
                } else {
                    had_stream_underrun = true;
                    if (++s.empty_ticks >= 6u) {
                        s.primed = false;
                        s.single_quantum_ticks = 0u;
                    }
                }
                if (s.pending_quanta.size() < 5u) {
                    s.stream->SignalTickEvent();
                }
            }
        }

        if (!any_started) {
            continue;
        }

        for (float& v : master_lr) {
            // Clean soft-clip limiter above +/-0.95 to prevent harsh digital clipping on multi-channel bed sums.
            if (!std::isfinite(v)) {
                v = 0.0f;
            } else if (v > 1.0f || v < -1.0f) {
                v = std::tanh(v);
            }
        }

        if (audio_client && render_client) {
            UINT32 padding = 0u;
            if (SUCCEEDED(audio_client->GetCurrentPadding(&padding)) &&
                padding + kDefaultFramesPerQuantum <= buffer_frames) {
                BYTE* dst = nullptr;
                if (SUCCEEDED(render_client->GetBuffer(kDefaultFramesPerQuantum, &dst)) && dst) {
                    std::memcpy(dst, master_lr.data(), kDefaultFramesPerQuantum * 2u * sizeof(float));
                    (void)render_client->ReleaseBuffer(kDefaultFramesPerQuantum, 0u);
                }
            }
        }

        wav_recorder_.AppendStereoFloat(
            master_lr.data(), kDefaultFramesPerQuantum, kDefaultSampleRate, had_stream_underrun);
    }
}

class HostWasapiSpatialAudioClient final : public ISpatialAudioClient {
public:
    explicit HostWasapiSpatialAudioClient(IMMDevice* endpoint) : endpoint_(endpoint) {
        if (endpoint_) endpoint_->AddRef();
    }
    ~HostWasapiSpatialAudioClient() {
        if (endpoint_) endpoint_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ISpatialAudioClient))) {
            AddRef();
            *ppvObject = static_cast<ISpatialAudioClient*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetStaticObjectPosition(
        AudioObjectType type, float* x, float* y, float* z) override {
        if (!x || !y || !z) return E_POINTER;
        *x = 0.0f; *y = 0.0f; *z = 1.0f;
        switch (type) {
            case AudioObjectType_FrontLeft:     *x = -1.0f; *y = 0.0f;  *z = 1.0f;  break;
            case AudioObjectType_FrontRight:    *x =  1.0f; *y = 0.0f;  *z = 1.0f;  break;
            case AudioObjectType_FrontCenter:   *x =  0.0f; *y = 0.0f;  *z = 1.0f;  break;
            case AudioObjectType_LowFrequency:  *x =  0.0f; *y = -1.0f; *z = 1.0f;  break;
            case AudioObjectType_SideLeft:      *x = -1.0f; *y = 0.0f;  *z = 0.0f;  break;
            case AudioObjectType_SideRight:     *x =  1.0f; *y = 0.0f;  *z = 0.0f;  break;
            case AudioObjectType_BackLeft:      *x = -1.0f; *y = 0.0f;  *z = -1.0f; break;
            case AudioObjectType_BackRight:     *x =  1.0f; *y = 0.0f;  *z = -1.0f; break;
            case AudioObjectType_TopFrontLeft:  *x = -1.0f; *y = 1.0f;  *z = 1.0f;  break;
            case AudioObjectType_TopFrontRight: *x =  1.0f; *y = 1.0f;  *z = 1.0f;  break;
            case AudioObjectType_TopBackLeft:   *x = -1.0f; *y = 1.0f;  *z = -1.0f; break;
            case AudioObjectType_TopBackRight:  *x =  1.0f; *y = 1.0f;  *z = -1.0f; break;
            default: break;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNativeStaticObjectTypeMask(AudioObjectType* mask) override {
        if (!mask) return E_POINTER;
        *mask = static_cast<AudioObjectType>(
            AudioObjectType_FrontLeft | AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter | AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft | AudioObjectType_SideRight |
            AudioObjectType_BackLeft | AudioObjectType_BackRight |
            AudioObjectType_TopFrontLeft | AudioObjectType_TopFrontRight |
            AudioObjectType_TopBackLeft | AudioObjectType_TopBackRight);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMaxDynamicObjectCount(UINT32* value) override {
        if (!value) return E_POINTER;
        *value = 128u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSupportedAudioObjectFormatEnumerator(
        IAudioFormatEnumerator** enumerator) override {
        if (!enumerator) return E_POINTER;
        auto* en = new (std::nothrow) HostAudioFormatEnumerator();
        if (!en) {
            *enumerator = nullptr;
            return E_OUTOFMEMORY;
        }
        *enumerator = en;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMaxFrameCount(
        const WAVEFORMATEX* objectFormat, UINT32* frameCountPerBuffer) override {
        if (!objectFormat || !frameCountPerBuffer) return E_POINTER;
        const HRESULT hr = IsAudioObjectFormatSupported(objectFormat);
        if (FAILED(hr)) return hr;
        *frameCountPerBuffer = std::max<UINT32>(64u, objectFormat->nSamplesPerSec / 100u);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsAudioObjectFormatSupported(const WAVEFORMATEX* objectFormat) override {
        if (!objectFormat) return E_POINTER;
        if (objectFormat->nChannels != 1u || objectFormat->nSamplesPerSec == 0u) {
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if ((objectFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && objectFormat->wBitsPerSample == 32u) ||
            (objectFormat->wFormatTag == WAVE_FORMAT_PCM && objectFormat->wBitsPerSample == 16u) ||
            (objectFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             (objectFormat->wBitsPerSample == 32u || objectFormat->wBitsPerSample == 16u))) {
            return S_OK;
        }
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    HRESULT STDMETHODCALLTYPE IsSpatialAudioStreamAvailable(
        REFIID streamUuid, const PROPVARIANT* /*auxiliaryInfo*/) override {
        if (IsEqualIID(streamUuid, __uuidof(ISpatialAudioObjectRenderStream)) ||
            IsEqualIID(streamUuid, __uuidof(ISpatialAudioObjectRenderStreamBase))) {
            return S_OK;
        }
        return SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE;
    }

    HRESULT STDMETHODCALLTYPE ActivateSpatialAudioStream(
        const PROPVARIANT* activationParams, REFIID riid, void** stream) override {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        if (!activationParams) return E_INVALIDARG;
        if (!IsEqualIID(riid, __uuidof(ISpatialAudioObjectRenderStream)) &&
            !IsEqualIID(riid, __uuidof(ISpatialAudioObjectRenderStreamBase)) &&
            !IsEqualIID(riid, __uuidof(IUnknown))) {
            return E_NOINTERFACE;
        }
        if (activationParams->vt != VT_BLOB ||
            activationParams->blob.cbSize < sizeof(SpatialAudioObjectRenderStreamActivationParams) ||
            !activationParams->blob.pBlobData) {
            return E_INVALIDARG;
        }
        SpatialAudioObjectRenderStreamActivationParams params{};
        std::memcpy(&params, activationParams->blob.pBlobData, sizeof(params));
        if (!params.ObjectFormat) return E_INVALIDARG;

        UINT32 frames = kDefaultFramesPerQuantum;
        const HRESULT fhr = GetMaxFrameCount(params.ObjectFormat, &frames);
        if (FAILED(fhr)) return fhr;

        const bool is_pcm16 = (params.ObjectFormat->wBitsPerSample == 16u);
        auto* rs = new (std::nothrow) HostWasapiSpatialRenderStream(
            endpoint_, params.MaxDynamicObjectCount, frames, is_pcm16, params.EventHandle);
        if (!rs) return E_OUTOFMEMORY;

        std::fprintf(stderr,
                     "[OpenScarlett][SpatialAudio] Activated RenderStream (sr=%u frames=%u staticMask=0x%X maxDyn=%u ev=%p)\n",
                     params.ObjectFormat->nSamplesPerSec,
                     frames,
                     static_cast<unsigned>(params.StaticObjectTypeMask),
                     params.MaxDynamicObjectCount,
                     params.EventHandle);
        std::fflush(stderr);

        *stream = static_cast<ISpatialAudioObjectRenderStream*>(rs);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IMMDevice* endpoint_ = nullptr;
};

class SpatialBridgeDevice final : public IMMDevice {
public:
    explicit SpatialBridgeDevice(IMMDevice* inner) : inner_(inner) {
        if (inner_) inner_->AddRef();
    }
    ~SpatialBridgeDevice() {
        if (inner_) inner_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMDevice))) {
            AddRef();
            *ppvObject = static_cast<IMMDevice*>(this);
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE Activate(
        REFIID iid, DWORD dwClsCtx, PROPVARIANT* pActivationParams, void** ppInterface) override {
        if (!ppInterface) return E_POINTER;
        if (IsEqualIID(iid, __uuidof(ISpatialAudioClient))) {
            auto* sac = new (std::nothrow) HostWasapiSpatialAudioClient(inner_);
            if (!sac) {
                *ppInterface = nullptr;
                return E_OUTOFMEMORY;
            }
            *ppInterface = static_cast<ISpatialAudioClient*>(sac);
            return S_OK;
        }
        return inner_ ? inner_->Activate(iid, dwClsCtx, pActivationParams, ppInterface) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD stgmAccess, IPropertyStore** ppProperties) override {
        return inner_ ? inner_->OpenPropertyStore(stgmAccess, ppProperties) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* ppstrId) override {
        return inner_ ? inner_->GetId(ppstrId) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* pdwState) override {
        return inner_ ? inner_->GetState(pdwState) : E_FAIL;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IMMDevice* inner_ = nullptr;
};

class SpatialBridgeDeviceCollection final : public IMMDeviceCollection {
public:
    explicit SpatialBridgeDeviceCollection(IMMDeviceCollection* inner) : inner_(inner) {
        if (inner_) inner_->AddRef();
    }
    ~SpatialBridgeDeviceCollection() {
        if (inner_) inner_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMDeviceCollection))) {
            AddRef();
            *ppvObject = static_cast<IMMDeviceCollection*>(this);
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetCount(UINT* pcDevices) override {
        return inner_ ? inner_->GetCount(pcDevices) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Item(UINT nDevice, IMMDevice** ppDevice) override {
        if (!ppDevice) return E_POINTER;
        *ppDevice = nullptr;
        if (!inner_) return E_FAIL;
        IMMDevice* raw = nullptr;
        const HRESULT hr = inner_->Item(nDevice, &raw);
        if (FAILED(hr) || !raw) return hr;
        auto* wrapped = new (std::nothrow) SpatialBridgeDevice(raw);
        raw->Release();
        if (!wrapped) return E_OUTOFMEMORY;
        *ppDevice = wrapped;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IMMDeviceCollection* inner_ = nullptr;
};

class SpatialBridgeDeviceEnumerator final : public IMMDeviceEnumerator {
public:
    explicit SpatialBridgeDeviceEnumerator(IMMDeviceEnumerator* inner) : inner_(inner) {
        if (inner_) inner_->AddRef();
    }
    ~SpatialBridgeDeviceEnumerator() {
        if (inner_) inner_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMDeviceEnumerator))) {
            AddRef();
            *ppvObject = static_cast<IMMDeviceEnumerator*>(this);
            return S_OK;
        }
        return inner_ ? inner_->QueryInterface(riid, ppvObject) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (n == 0u) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(
        EDataFlow dataFlow, DWORD dwStateMask, IMMDeviceCollection** ppDevices) override {
        if (!ppDevices) return E_POINTER;
        *ppDevices = nullptr;
        if (!inner_) return E_FAIL;
        IMMDeviceCollection* raw = nullptr;
        const HRESULT hr = inner_->EnumAudioEndpoints(dataFlow, dwStateMask, &raw);
        if (FAILED(hr) || !raw) return hr;
        auto* wrapped = new (std::nothrow) SpatialBridgeDeviceCollection(raw);
        raw->Release();
        if (!wrapped) return E_OUTOFMEMORY;
        *ppDevices = wrapped;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(
        EDataFlow dataFlow, ERole role, IMMDevice** ppEndpoint) override {
        if (!ppEndpoint) return E_POINTER;
        *ppEndpoint = nullptr;
        if (!inner_) return E_FAIL;
        IMMDevice* raw = nullptr;
        const HRESULT hr = inner_->GetDefaultAudioEndpoint(dataFlow, role, &raw);
        if (FAILED(hr) || !raw) return hr;
        auto* wrapped = new (std::nothrow) SpatialBridgeDevice(raw);
        raw->Release();
        if (!wrapped) return E_OUTOFMEMORY;
        *ppEndpoint = wrapped;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR pwstrId, IMMDevice** ppDevice) override {
        if (!ppDevice) return E_POINTER;
        *ppDevice = nullptr;
        if (!inner_) return E_FAIL;
        IMMDevice* raw = nullptr;
        const HRESULT hr = inner_->GetDevice(pwstrId, &raw);
        if (FAILED(hr) || !raw) return hr;
        auto* wrapped = new (std::nothrow) SpatialBridgeDevice(raw);
        raw->Release();
        if (!wrapped) return E_OUTOFMEMORY;
        *ppDevice = wrapped;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(
        IMMNotificationClient* pClient) override {
        return inner_ ? inner_->RegisterEndpointNotificationCallback(pClient) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(
        IMMNotificationClient* pClient) override {
        return inner_ ? inner_->UnregisterEndpointNotificationCallback(pClient) : E_FAIL;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    IMMDeviceEnumerator* inner_ = nullptr;
};

}  // namespace

void WrapMmDeviceEnumeratorIfNeeded(REFCLSID rclsid, REFIID riid, void** ppv) noexcept {
    if (!ppv || !*ppv) return;
    if (!IsEqualCLSID(rclsid, __uuidof(MMDeviceEnumerator))) return;
    if (!IsEqualIID(riid, __uuidof(IMMDeviceEnumerator)) &&
        !IsEqualIID(riid, __uuidof(IUnknown))) {
        return;
    }
    auto* unk = static_cast<IUnknown*>(*ppv);
    IMMDeviceEnumerator* raw_enum = nullptr;
    if (FAILED(unk->QueryInterface(__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&raw_enum))) ||
        !raw_enum) {
        return;
    }
    auto* wrapped = new (std::nothrow) SpatialBridgeDeviceEnumerator(raw_enum);
    raw_enum->Release();
    if (!wrapped) return;
    unk->Release();
    *ppv = IsEqualIID(riid, __uuidof(IMMDeviceEnumerator))
               ? static_cast<void*>(static_cast<IMMDeviceEnumerator*>(wrapped))
               : static_cast<void*>(static_cast<IUnknown*>(wrapped));
}

void FlushSpatialAudioWavRecorder() noexcept {
    SpatialMasterEngine::Instance().FlushWav();
}

}  // namespace openscarlett::loader

