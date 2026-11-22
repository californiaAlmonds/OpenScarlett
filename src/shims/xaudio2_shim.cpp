#define OPENSCARLETT_BUILDING_XAUDIO2 1
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <audioclient.h>
#  include <mmdeviceapi.h>
#endif

namespace {

constexpr const char* kModName = "XAudio2_9.dll";

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const XAudio2Guid* a, const XAudio2Guid& b) {
    return a && std::memcmp(a, &b, sizeof(XAudio2Guid)) == 0;
}

#if defined(_WIN32)
// Inline Core Audio / WASAPI GUIDs (portable across MSVC, Clang-CL, and MinGW-w64; zero __uuidof).
constexpr GUID kClsidMmDeviceEnumerator = {
    0xBCDE0395u, 0xE52Fu, 0x467Cu, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID kIidImmDeviceEnumerator = {
    0xA95664D2u, 0x9614u, 0x4F35u, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID kIidIAudioClient = {
    0x1CB9AD4Cu, 0xDBFAu, 0x4C32u, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID kIidIAudioRenderClient = {
    0xF294ACFCu, 0x3146u, 0x4483u, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
#endif

std::string GetEnvString(const char* name) {
#if defined(_WIN32)
    char buf[256]{};
    const DWORD len = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0u || len >= sizeof(buf)) {
        return {};
    }
    return std::string(buf, len);
#elif defined(_MSC_VER)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) {
        return {};
    }
    std::string out(buf);
    std::free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

bool IsNullAudioForced() {
    const std::string v = GetEnvString("OPENSCARLETT_AUDIO_NULL");
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

// ===========================================================================
// Windows MMDevice Endpoint ID Validation & Normalization
// Accepts:
//   - NULL / empty / "default" (case-insensitive)
//   - Canonical IMMDevice ID: "{0.0.0.00000000}.{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
//   - WinRT/PnP device interface path:
//       "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}"
//       "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface-guid}"
//   - Bare GUID: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
// ===========================================================================
struct ParsedEndpointId {
    bool valid = false;
    bool is_default = false;
    std::wstring canonical_id;
};

bool IsAsciiHexW(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') ||
           (c >= L'a' && c <= L'f') ||
           (c >= L'A' && c <= L'F');
}

bool IsAsciiDigitW(wchar_t c) noexcept {
    return c >= L'0' && c <= L'9';
}

wchar_t ToLowerAsciiW(wchar_t c) noexcept {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

bool EqualsIgnoreCaseAsciiW(const std::wstring& a, const wchar_t* b) {
    size_t i = 0u;
    for (; i < a.size() && b[i] != L'\0'; ++i) {
        if (ToLowerAsciiW(a[i]) != ToLowerAsciiW(b[i])) {
            return false;
        }
    }
    return i == a.size() && b[i] == L'\0';
}

bool StartsWithIgnoreCaseAsciiW(const std::wstring& s, const wchar_t* prefix) {
    size_t i = 0u;
    for (; prefix[i] != L'\0'; ++i) {
        if (i >= s.size() || ToLowerAsciiW(s[i]) != ToLowerAsciiW(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::wstring ToLowerGuidAsciiW(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) {
        c = ToLowerAsciiW(c);
    }
    return out;
}

// Validates "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" (38 chars).
bool IsValidBracedGuidW(const std::wstring& s) noexcept {
    if (s.size() != 38u || s.front() != L'{' || s.back() != L'}') {
        return false;
    }
    for (size_t i = 1u; i < 37u; ++i) {
        if (i == 9u || i == 14u || i == 19u || i == 24u) {
            if (s[i] != L'-') return false;
        } else {
            if (!IsAsciiHexW(s[i])) return false;
        }
    }
    return true;
}

// Validates "{d.d.d.hhhhhhhh}.{guid}" (standard Windows Core Audio IMMDevice ID).
bool IsValidMmDeviceCanonicalIdW(const std::wstring& s) noexcept {
    const size_t sep = s.find(L"}.{");
    if (sep == std::wstring::npos || s.front() != L'{') {
        return false;
    }
    const std::wstring prefix = s.substr(1u, sep - 1u);
    const std::wstring guid = s.substr(sep + 2u);
    if (!IsValidBracedGuidW(guid)) {
        return false;
    }
    // Prefix format: "<flow>.<reserved>.<role>.<8 hex digits>", e.g. "0.0.0.00000000"
    size_t dot1 = prefix.find(L'.');
    if (dot1 == std::wstring::npos || dot1 == 0u) return false;
    size_t dot2 = prefix.find(L'.', dot1 + 1u);
    if (dot2 == std::wstring::npos || dot2 == dot1 + 1u) return false;
    size_t dot3 = prefix.find(L'.', dot2 + 1u);
    if (dot3 == std::wstring::npos || dot3 == dot2 + 1u) return false;
    if (prefix.find(L'.', dot3 + 1u) != std::wstring::npos) return false;

    for (size_t i = 0u; i < dot1; ++i) {
        if (!IsAsciiDigitW(prefix[i])) return false;
    }
    for (size_t i = dot1 + 1u; i < dot2; ++i) {
        if (!IsAsciiDigitW(prefix[i])) return false;
    }
    for (size_t i = dot2 + 1u; i < dot3; ++i) {
        if (!IsAsciiDigitW(prefix[i])) return false;
    }
    const size_t tail_len = prefix.size() - (dot3 + 1u);
    if (tail_len != 8u) return false;
    for (size_t i = dot3 + 1u; i < prefix.size(); ++i) {
        if (!IsAsciiHexW(prefix[i])) return false;
    }
    return true;
}

ParsedEndpointId ParseMmDeviceEndpointIdW(const wchar_t* deviceId) {
    ParsedEndpointId out{};
    if (!deviceId || deviceId[0] == L'\0') {
        out.valid = true;
        out.is_default = true;
        return out;
    }

    const std::wstring raw(deviceId);
    if (raw.size() >= 256u) {
        return out;
    }
    if (EqualsIgnoreCaseAsciiW(raw, L"default")) {
        out.valid = true;
        out.is_default = true;
        return out;
    }

    // 1. Canonical IMMDevice ID: "{0.0.0.00000000}.{guid}"
    if (IsValidMmDeviceCanonicalIdW(raw)) {
        out.valid = true;
        out.is_default = false;
        out.canonical_id = ToLowerGuidAsciiW(raw);
        return out;
    }

    // 2. WinRT / PnP Device Interface Path: "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}[#{iface-guid}]"
    constexpr const wchar_t* kSwdPrefix1 = L"\\\\?\\SWD#MMDEVAPI#";
    constexpr const wchar_t* kSwdPrefix2 = L"\\\\.\\SWD#MMDEVAPI#";
    size_t prefix_len = 0u;
    if (StartsWithIgnoreCaseAsciiW(raw, kSwdPrefix1)) {
        prefix_len = 17u;
    } else if (StartsWithIgnoreCaseAsciiW(raw, kSwdPrefix2)) {
        prefix_len = 17u;
    }
    if (prefix_len > 0u) {
        const std::wstring rest = raw.substr(prefix_len);
        if (rest.empty()) {
            return out;
        }
        const size_t hash_pos = rest.find(L'#');
        const std::wstring id_part = (hash_pos == std::wstring::npos) ? rest : rest.substr(0u, hash_pos);
        if (hash_pos != std::wstring::npos) {
            const std::wstring iface_part = rest.substr(hash_pos + 1u);
            if (!IsValidBracedGuidW(iface_part)) {
                return out;
            }
        }
        if (IsValidMmDeviceCanonicalIdW(id_part)) {
            out.valid = true;
            out.is_default = false;
            out.canonical_id = ToLowerGuidAsciiW(id_part);
            return out;
        }
        if (IsValidBracedGuidW(id_part)) {
            out.valid = true;
            out.is_default = false;
            out.canonical_id = L"{0.0.0.00000000}." + ToLowerGuidAsciiW(id_part);
            return out;
        }
        return out;
    }

    // 3. Bare GUID: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"
    if (IsValidBracedGuidW(raw)) {
        out.valid = true;
        out.is_default = false;
        out.canonical_id = L"{0.0.0.00000000}." + ToLowerGuidAsciiW(raw);
        return out;
    }

    return out;
}

class XAudio2Impl;
class SubmixVoiceImpl;
class MasteringVoiceImpl;
class SourceVoiceImpl;
class ReverbApoImpl;
class VolumeMeterApoImpl;

// Standard XAudio2 Effect Chain ABI structs (matching xaudio2.h)
struct OsXAudio2EffectDescriptor {
    OS_IXAudio2Unknown* pEffect;
    int32_t InitialState;
    uint32_t OutputChannels;
};

struct OsXAudio2EffectChain {
    uint32_t EffectCount;
    OsXAudio2EffectDescriptor* pEffectDescriptors;
};

struct VoiceEffectSlot {
    OS_IXAudio2Unknown* effect = nullptr;
    bool enabled = true;
    uint32_t output_channels = 2u;
};

void ProcessEffectApo(
    OS_IXAudio2Unknown* effect,
    float* interleaved,
    uint32_t frames,
    uint32_t channels,
    uint32_t sample_rate);

void ClearEffectSlots(std::vector<VoiceEffectSlot>& slots) {
    for (auto& s : slots) {
        if (s.effect) {
            s.effect->Release();
            s.effect = nullptr;
        }
    }
    slots.clear();
}

int32_t ReplaceEffectSlots(std::vector<VoiceEffectSlot>& dst, const void* pEffectChain) {
    ClearEffectSlots(dst);
    if (!pEffectChain) {
        return OS_XAUDIO2_S_OK;
    }
    const auto* chain = static_cast<const OsXAudio2EffectChain*>(pEffectChain);
    if (chain->EffectCount > 16u || (chain->EffectCount > 0u && !chain->pEffectDescriptors)) {
        return OS_XAUDIO2_E_INVALIDARG;
    }
    dst.reserve(chain->EffectCount);
    for (uint32_t i = 0u; i < chain->EffectCount; ++i) {
        const auto& desc = chain->pEffectDescriptors[i];
        if (!desc.pEffect) {
            ClearEffectSlots(dst);
            return OS_XAUDIO2_E_INVALIDARG;
        }
        desc.pEffect->AddRef();
        VoiceEffectSlot slot{};
        slot.effect = desc.pEffect;
        slot.enabled = (desc.InitialState != 0);
        slot.output_channels = desc.OutputChannels;
        dst.push_back(slot);
    }
    return OS_XAUDIO2_S_OK;
}

// ===========================================================================
// Decoded Queued PCM Buffer (Interleaved float32)
// ===========================================================================
struct QueuedAudioBuffer {
    std::vector<float> samples;  // interleaved float32 (frames * channels)
    uint32_t channels = 2;
    uint32_t total_frames = 0;
    double play_cursor = 0.0;    // current frame position (fractional for resampling)
    uint32_t loop_begin = 0;
    uint32_t loop_length = 0;
    uint32_t loops_remaining = 0;
    bool end_of_stream = false;
    bool started_notified = false;
    void* context = nullptr;
};

struct DeferredVoiceCallbacks {
    IXAudio2VoiceCallback* cb = nullptr;
    bool pass_started = false;
    uint32_t bytes_required = 0u;
    std::vector<void*> started;
    std::vector<void*> loop_ended;
    std::vector<void*> ended;
    bool stream_ended = false;
};

// ===========================================================================
// Submix Voice
// ===========================================================================
class SubmixVoiceImpl final : public IXAudio2SubmixVoice {
public:
    SubmixVoiceImpl(
        XAudio2Impl* engine,
        uint32_t channels,
        uint32_t sample_rate,
        uint32_t flags,
        uint32_t processing_stage)
        : engine_(engine),
          channels_(std::max(1u, channels)),
          sample_rate_(std::max(1u, sample_rate)),
          flags_(flags),
          processing_stage_(processing_stage),
          channel_volumes_(std::max(1u, channels), 1.0f) {}

    ~SubmixVoiceImpl() {
        ClearEffectSlots(effect_chain_);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2SubmixVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = channels_;
        pVoiceDetails->InputSampleRate = sample_rate_;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* pSendList) override {
        std::lock_guard<std::mutex> lk(mutex_);
        target_submix_ = nullptr;
        if (pSendList && pSendList->SendCount > 0u && pSendList->pSends) {
            for (uint32_t i = 0u; i < pSendList->SendCount; ++i) {
                auto* sub = dynamic_cast<SubmixVoiceImpl*>(pSendList->pSends[i].pOutputVoice);
                if (sub && sub != this) {
                    target_submix_ = sub;
                    break;
                }
            }
        }
        return RecordHr("IXAudio2SubmixVoice::SetOutputVoices", OS_XAUDIO2_S_OK, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override {
        volume_.store(std::clamp(Volume, -16777216.0f, 16777216.0f), std::memory_order_relaxed);
        return RecordHr("IXAudio2SubmixVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2SubmixVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override {
        if (!pVolumes || Channels != channels_) {
            return RecordHr("IXAudio2SubmixVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        channel_volumes_.assign(pVolumes, pVolumes + Channels);
        return RecordHr("IXAudio2SubmixVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2SubmixVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t n = std::min<uint32_t>(Channels, static_cast<uint32_t>(channel_volumes_.size()));
        if (n > 0) std::memcpy(pVolumes, channel_volumes_.data(), n * sizeof(float));
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    int32_t SetEffectChain(const void* pEffectChain) {
        std::lock_guard<std::mutex> lk(mutex_);
        return ReplaceEffectSlots(effect_chain_, pEffectChain);
    }

    int32_t EnableEffect(uint32_t index, bool enable) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        effect_chain_[index].enabled = enable;
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectState(uint32_t index, int32_t* out_enabled) {
        if (!out_enabled) return OS_XAUDIO2_E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        *out_enabled = effect_chain_[index].enabled ? 1 : 0;
        return OS_XAUDIO2_S_OK;
    }

    int32_t SetEffectParameters(uint32_t index, const void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->SetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectParameters(uint32_t index, void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->GetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    void PrepareBus(uint32_t frames, uint32_t channels) {
        const size_t n = static_cast<size_t>(frames) * channels;
        if (mix_bus_.size() != n) {
            mix_bus_.assign(n, 0.0f);
        } else {
            std::fill_n(mix_bus_.data(), n, 0.0f);
        }
    }

    float* BusData() { return mix_bus_.data(); }

    void ProcessAndAccumulateTo(
        float* dst_interleaved,
        uint32_t frames,
        uint32_t channels,
        uint32_t sample_rate) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& slot : effect_chain_) {
            if (slot.enabled && slot.effect) {
                ProcessEffectApo(slot.effect, mix_bus_.data(), frames, channels, sample_rate);
            }
        }
        const float vol = volume_.load(std::memory_order_relaxed);
        float* target = target_submix_ ? target_submix_->BusData() : dst_interleaved;
        if (!target) target = dst_interleaved;
        for (uint32_t f = 0u; f < frames; ++f) {
            for (uint32_t c = 0u; c < channels; ++c) {
                const float cg = (c < channel_volumes_.size()) ? channel_volumes_[c] : 1.0f;
                target[static_cast<size_t>(f) * channels + c] +=
                    mix_bus_[static_cast<size_t>(f) * channels + c] * vol * cg;
            }
        }
    }

    void ClearSubmixReference(SubmixVoiceImpl* dead) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (target_submix_ == dead) {
            target_submix_ = nullptr;
        }
    }

    float Volume() const { return volume_.load(std::memory_order_relaxed); }
    uint32_t ProcessingStage() const { return processing_stage_; }

private:
    XAudio2Impl* engine_;
    uint32_t channels_;
    uint32_t sample_rate_;
    uint32_t flags_;
    uint32_t processing_stage_ = 0u;
    std::atomic<float> volume_{1.0f};
    std::mutex mutex_;
    SubmixVoiceImpl* target_submix_ = nullptr;
    std::vector<float> channel_volumes_;
    std::vector<VoiceEffectSlot> effect_chain_;
    std::vector<float> mix_bus_;
};

// ===========================================================================
// Mastering Voice
// ===========================================================================
class MasteringVoiceImpl final : public IXAudio2MasteringVoice {
public:
    MasteringVoiceImpl(XAudio2Impl* engine, uint32_t channels, uint32_t sample_rate, uint32_t flags)
        : engine_(engine),
          channels_(channels != 0 ? channels : 2u),
          sample_rate_(sample_rate != 0 ? sample_rate : 48000u),
          flags_(flags),
          channel_volumes_(channels != 0 ? channels : 2u, 1.0f) {}

    ~MasteringVoiceImpl() {
        ClearEffectSlots(effect_chain_);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2MasteringVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = channels_;
        pVoiceDetails->InputSampleRate = sample_rate_;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* /*pSendList*/) override {
        return RecordHr("IXAudio2MasteringVoice::SetOutputVoices", OS_XAUDIO2_E_INVALID_CALL, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override;
    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override;

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2MasteringVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2MasteringVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t n = std::min<uint32_t>(Channels, static_cast<uint32_t>(channel_volumes_.size()));
        if (n > 0) std::memcpy(pVolumes, channel_volumes_.data(), n * sizeof(float));
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    int32_t OPENSCARLETT_XAUDIO2_CALL GetChannelMask(uint32_t* pChannelMask) override {
        if (!pChannelMask) {
            return RecordHr("IXAudio2MasteringVoice::GetChannelMask", OS_XAUDIO2_E_INVALIDARG, 7);
        }
        *pChannelMask = (channels_ == 1) ? 0x4u : (channels_ == 6 ? 0x3Fu : 0x3u);
        return RecordHr("IXAudio2MasteringVoice::GetChannelMask", OS_XAUDIO2_S_OK, 7);
    }

    int32_t SetEffectChain(const void* pEffectChain);

    int32_t EnableEffect(uint32_t index, bool enable) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        effect_chain_[index].enabled = enable;
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectState(uint32_t index, int32_t* out_enabled) {
        if (!out_enabled) return OS_XAUDIO2_E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        *out_enabled = effect_chain_[index].enabled ? 1 : 0;
        return OS_XAUDIO2_S_OK;
    }

    int32_t SetEffectParameters(uint32_t index, const void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->SetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectParameters(uint32_t index, void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->GetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    void ProcessMasterEffects(float* interleaved, uint32_t frames, uint32_t channels, uint32_t sample_rate) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& slot : effect_chain_) {
            if (slot.enabled && slot.effect) {
                ProcessEffectApo(slot.effect, interleaved, frames, channels, sample_rate);
            }
        }
    }

    uint32_t Channels() const { return channels_; }
    uint32_t SampleRate() const { return sample_rate_; }
    float Volume() const { return volume_.load(std::memory_order_relaxed); }

    std::vector<float> SnapshotChannelVolumes() {
        std::lock_guard<std::mutex> lk(mutex_);
        return channel_volumes_;
    }

private:
    XAudio2Impl* engine_;
    uint32_t channels_;
    uint32_t sample_rate_;
    uint32_t flags_;
    std::atomic<float> volume_{1.0f};
    std::mutex mutex_;
    std::vector<float> channel_volumes_;
    std::vector<VoiceEffectSlot> effect_chain_;
};

// ===========================================================================
// Lock-Free Bounded MPMC Queue (Vyukov Ring Buffer) for Zero-Contention Audio Mixing
// ===========================================================================
class LockFreeBufferQueue {
public:
    static constexpr uint32_t kCapacity = 128u;  // Power of two
    static constexpr uint64_t kMask = kCapacity - 1u;

    LockFreeBufferQueue() {
        for (uint64_t i = 0u; i < kCapacity; ++i) {
            cells_[i].seq.store(i, std::memory_order_relaxed);
            cells_[i].ptr = nullptr;
        }
    }

    bool TryEnqueue(QueuedAudioBuffer* item) noexcept {
        Cell* cell = nullptr;
        uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->ptr = item;
        cell->seq.store(pos + 1u, std::memory_order_release);
        return true;
    }

    QueuedAudioBuffer* TryDequeue() noexcept {
        Cell* cell = nullptr;
        uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & kMask];
            const uint64_t seq = cell->seq.load(std::memory_order_acquire);
            const int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1u);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return nullptr;  // Queue empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        QueuedAudioBuffer* item = cell->ptr;
        cell->ptr = nullptr;
        cell->seq.store(pos + kCapacity, std::memory_order_release);
        return item;
    }

private:
    struct Cell {
        std::atomic<uint64_t> seq{0u};
        QueuedAudioBuffer* ptr = nullptr;
    };
    std::array<Cell, kCapacity> cells_{};
    std::atomic<uint64_t> enqueue_pos_{0u};
    std::atomic<uint64_t> dequeue_pos_{0u};
};

// ===========================================================================
// Source Voice (PCM8/16/24/32, Float32, ADPCM, Opus, and native Xbox XMA2 0x0166)
//   Lock-free buffer submission, state queries, and real-time audio mixing!
// ===========================================================================
class SourceVoiceImpl final : public IXAudio2SourceVoice {
public:
    static constexpr uint32_t kMaxInlineChannels = 8u;

    SourceVoiceImpl(
        XAudio2Impl* engine,
        const OS_WAVEFORMATEX& fmt,
        uint32_t flags,
        float max_freq_ratio,
        IXAudio2VoiceCallback* cb)
        : engine_(engine),
          format_(fmt),
          flags_(flags),
          max_freq_ratio_(std::max(1.0f, max_freq_ratio)),
          callback_(cb) {
        for (auto& cv : channel_volumes_) {
            cv.store(1.0f, std::memory_order_relaxed);
        }
        if (format_.wFormatTag == OS_WAVE_FORMAT_EXTENSIBLE) {
            effective_format_tag_ = OS_WAVE_FORMAT_PCM;
        } else {
            effective_format_tag_ = format_.wFormatTag;
        }
    }

    ~SourceVoiceImpl() {
        ClearEffectSlots(effect_chain_);
        QueuedAudioBuffer* cur = current_buf_.exchange(nullptr, std::memory_order_acq_rel);
        delete cur;
        while (QueuedAudioBuffer* q = buffer_queue_.TryDequeue()) {
            delete q;
        }
    }

    void SetEffectiveFormatTag(uint16_t tag) { effective_format_tag_ = tag; }
    void SetAdpcmSamplesPerBlock(uint16_t spb) { adpcm_samples_per_block_ = spb; }
    void SetOpusPreSkip(uint16_t pre_skip) { opus_pre_skip_ = pre_skip; }
    void SetSlotIndex(uint32_t idx) { slot_index_.store(idx, std::memory_order_release); }
    uint32_t SlotIndex() const { return slot_index_.load(std::memory_order_acquire); }
    void MarkDestroying() { destroying_.store(true, std::memory_order_release); }
    bool IsDestroying() const { return destroying_.load(std::memory_order_acquire); }
    bool IsOpusMftActive() const {
        return opus_mft_active_.load(std::memory_order_acquire);
    }

    int32_t SetEffectChain(const void* pEffectChain) {
        std::lock_guard<std::mutex> lk(config_mutex_);
        const int32_t hr = ReplaceEffectSlots(effect_chain_, pEffectChain);
        has_effects_.store(!effect_chain_.empty(), std::memory_order_release);
        return hr;
    }

    int32_t EnableEffect(uint32_t index, bool enable) {
        std::lock_guard<std::mutex> lk(config_mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        effect_chain_[index].enabled = enable;
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectState(uint32_t index, int32_t* out_enabled) {
        if (!out_enabled) return OS_XAUDIO2_E_POINTER;
        std::lock_guard<std::mutex> lk(config_mutex_);
        if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
        *out_enabled = effect_chain_[index].enabled ? 1 : 0;
        return OS_XAUDIO2_S_OK;
    }

    int32_t SetEffectParameters(uint32_t index, const void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(config_mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->SetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    int32_t GetEffectParameters(uint32_t index, void* params, uint32_t bytes) {
        if (!params || bytes == 0u) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAudio2Unknown* eff = nullptr;
        {
            std::lock_guard<std::mutex> lk(config_mutex_);
            if (index >= effect_chain_.size()) return OS_XAUDIO2_E_INVALIDARG;
            eff = effect_chain_[index].effect;
            if (eff) eff->AddRef();
        }
        if (!eff) return OS_XAUDIO2_E_INVALIDARG;
        OS_IXAPOParameters* xapo = nullptr;
        const int32_t hr = eff->QueryInterface(&IID_OS_IXAPOParameters, reinterpret_cast<void**>(&xapo));
        eff->Release();
        if (hr < 0 || !xapo) return OS_XAUDIO2_E_INVALIDARG;
        xapo->GetParameters(params, bytes);
        xapo->Release();
        return OS_XAUDIO2_S_OK;
    }

    void ClearSubmixReference(SubmixVoiceImpl* dead) {
        std::lock_guard<std::mutex> lk(config_mutex_);
        send_submixes_.erase(
            std::remove(send_submixes_.begin(), send_submixes_.end(), dead),
            send_submixes_.end());
        has_submix_sends_.store(!send_submixes_.empty(), std::memory_order_release);
        if (send_submixes_.empty()) {
            send_to_master_.store(true, std::memory_order_release);
        }
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) override {
        RecordOk("IXAudio2SourceVoice::GetVoiceDetails", 0);
        if (!pVoiceDetails) return;
        pVoiceDetails->CreationFlags = flags_;
        pVoiceDetails->ActiveFlags = flags_;
        pVoiceDetails->InputChannels = format_.nChannels;
        pVoiceDetails->InputSampleRate = sample_rate_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* pSendList) override {
        std::lock_guard<std::mutex> lk(config_mutex_);
        send_submixes_.clear();
        bool to_master = false;
        if (!pSendList || pSendList->SendCount == 0u || !pSendList->pSends) {
            to_master = true;
        } else {
            for (uint32_t i = 0u; i < pSendList->SendCount; ++i) {
                IXAudio2Voice* dst = pSendList->pSends[i].pOutputVoice;
                if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(dst)) {
                    if (std::find(send_submixes_.begin(), send_submixes_.end(), sub) ==
                        send_submixes_.end()) {
                        send_submixes_.push_back(sub);
                    }
                } else if (dynamic_cast<MasteringVoiceImpl*>(dst) != nullptr) {
                    to_master = true;
                }
            }
            if (send_submixes_.empty() && !to_master) {
                to_master = true;
            }
        }
        send_to_master_.store(to_master, std::memory_order_release);
        has_submix_sends_.store(!send_submixes_.empty(), std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::SetOutputVoices", OS_XAUDIO2_S_OK, 1);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float Volume, uint32_t /*OperationSet*/) override {
        volume_.store(Volume, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) override {
        RecordOk("IXAudio2SourceVoice::GetVolume", 3);
        if (pVolume) *pVolume = volume_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) override {
        if (!pVolumes || Channels != format_.nChannels) {
            return RecordHr("IXAudio2SourceVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
        }
        const uint32_t n = std::min<uint32_t>(Channels, kMaxInlineChannels);
        for (uint32_t i = 0u; i < n; ++i) {
            channel_volumes_[i].store(pVolumes[i], std::memory_order_relaxed);
        }
        return RecordHr("IXAudio2SourceVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(uint32_t Channels, float* pVolumes) override {
        RecordOk("IXAudio2SourceVoice::GetChannelVolumes", 5);
        if (!pVolumes) return;
        const uint32_t n = std::min<uint32_t>(Channels, std::min<uint32_t>(format_.nChannels, kMaxInlineChannels));
        for (uint32_t i = 0u; i < n; ++i) {
            pVolumes[i] = channel_volumes_[i].load(std::memory_order_relaxed);
        }
    }

    void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() override;

    int32_t OPENSCARLETT_XAUDIO2_CALL Start(uint32_t /*Flags*/, uint32_t /*OperationSet*/) override {
        playing_.store(true, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::Start", OS_XAUDIO2_S_OK, 7);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Stop(uint32_t /*Flags*/, uint32_t /*OperationSet*/) override {
        playing_.store(false, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::Stop", OS_XAUDIO2_S_OK, 8);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SubmitSourceBuffer(
        const OS_XAUDIO2_BUFFER* pBuffer,
        const OS_XAUDIO2_BUFFER_WMA* /*pBufferWMA*/) override {
        if (destroying_.load(std::memory_order_acquire)) {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALID_CALL, 9);
        }
        if (!pBuffer || (pBuffer->AudioBytes > 0 && !pBuffer->pAudioData)) {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALIDARG, 9);
        }

        const uint32_t ch = std::max<uint32_t>(1u, format_.nChannels);
        std::vector<float> decoded;

        if (effective_format_tag_ == OS_WAVE_FORMAT_XMA2) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!xma2_decoder_) {
                xma2_decoder_ = std::make_unique<openscarlett::Xma2Decoder>();
                openscarlett::Xma2Config cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                if (!xma2_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_XMA_DECODER_ERROR, 9);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = xma2_decoder_->DecodePackets(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_XMA_DECODER_ERROR, 9);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_IMA_ADPCM ||
                   effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!adpcm_decoder_) {
                adpcm_decoder_ = std::make_unique<openscarlett::ImaAdpcmDecoder>();
                openscarlett::ImaAdpcmConfig cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                cfg.block_align = format_.nBlockAlign;
                cfg.samples_per_block = adpcm_samples_per_block_;
                cfg.xbox_adpcm = (effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM);
                if (!adpcm_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = adpcm_decoder_->DecodeBlocks(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_OPUS) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (!opus_decoder_) {
                opus_decoder_ = std::make_unique<openscarlett::OpusMftDecoder>();
                openscarlett::OpusDecoderConfig cfg{};
                cfg.sample_rate = format_.nSamplesPerSec != 0 ? format_.nSamplesPerSec : 48000u;
                cfg.channels = static_cast<uint16_t>(std::min<uint32_t>(2u, ch));
                cfg.pre_skip = opus_pre_skip_;
                cfg.allow_fallback = true;
                if (!opus_decoder_->Initialize(cfg).ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
                opus_mft_active_.store(opus_decoder_->IsNativeMftActive(), std::memory_order_release);
                const uint32_t out_sr = opus_decoder_->Stats().output_sample_rate;
                if (out_sr != 0u) {
                    sample_rate_.store(out_sr, std::memory_order_relaxed);
                }
            }
            if (pBuffer->AudioBytes > 0) {
                const auto st = opus_decoder_->DecodeStream(
                    pBuffer->pAudioData, pBuffer->AudioBytes, decoded,
                    openscarlett::OpusPacketFraming::Auto);
                if (!st.ok()) {
                    return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                    OS_XAUDIO2_E_INVALIDARG, 9);
                }
                if ((pBuffer->Flags & OS_XAUDIO2_END_OF_STREAM) != 0u) {
                    (void)opus_decoder_->Drain(decoded);
                }
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_IEEE_FLOAT) {
            if (format_.wBitsPerSample != 32) {
                return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                OS_XAUDIO2_E_INVALIDARG, 9);
            }
            const size_t sample_count = pBuffer->AudioBytes / sizeof(float);
            decoded.resize(sample_count);
            if (sample_count > 0) {
                std::memcpy(decoded.data(), pBuffer->pAudioData, sample_count * sizeof(float));
            }
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_PCM) {
            if (format_.wBitsPerSample == 16) {
                const size_t sample_count = pBuffer->AudioBytes / sizeof(int16_t);
                decoded.resize(sample_count);
                const auto* src = reinterpret_cast<const int16_t*>(pBuffer->pAudioData);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = static_cast<float>(src[i]) / 32768.0f;
                }
            } else if (format_.wBitsPerSample == 8) {
                const size_t sample_count = pBuffer->AudioBytes;
                decoded.resize(sample_count);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = (static_cast<float>(pBuffer->pAudioData[i]) - 128.0f) / 128.0f;
                }
            } else if (format_.wBitsPerSample == 32) {
                const size_t sample_count = pBuffer->AudioBytes / sizeof(int32_t);
                decoded.resize(sample_count);
                const auto* src = reinterpret_cast<const int32_t*>(pBuffer->pAudioData);
                for (size_t i = 0; i < sample_count; ++i) {
                    decoded[i] = static_cast<float>(static_cast<double>(src[i]) / 2147483648.0);
                }
            } else {
                return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer",
                                OS_XAUDIO2_E_INVALIDARG, 9);
            }
        } else {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALIDARG, 9);
        }

        uint32_t eff_ch = ch;
        if (effective_format_tag_ == OS_WAVE_FORMAT_XMA2 ||
            effective_format_tag_ == OS_WAVE_FORMAT_IMA_ADPCM ||
            effective_format_tag_ == OS_WAVE_FORMAT_XBOX_ADPCM) {
            eff_ch = std::min<uint32_t>(2u, ch);
        } else if (effective_format_tag_ == OS_WAVE_FORMAT_OPUS) {
            std::lock_guard<std::mutex> lk(decoder_mutex_);
            if (opus_decoder_ && opus_decoder_->Stats().output_channels > 0u) {
                eff_ch = opus_decoder_->Stats().output_channels;
            } else {
                eff_ch = std::min<uint32_t>(2u, ch);
            }
        }
        uint32_t total_frames = static_cast<uint32_t>(decoded.size() / std::max<uint32_t>(1u, eff_ch));

        uint32_t play_slice_start = 0u;
        if (pBuffer->PlayBegin > 0 || pBuffer->PlayLength > 0) {
            play_slice_start = std::min<uint32_t>(pBuffer->PlayBegin, total_frames);
            const uint32_t max_avail = total_frames - play_slice_start;
            const uint32_t len = (pBuffer->PlayLength > 0)
                                     ? std::min<uint32_t>(pBuffer->PlayLength, max_avail)
                                     : max_avail;
            std::vector<float> sliced(len * eff_ch);
            if (len > 0) {
                std::memcpy(sliced.data(), decoded.data() + play_slice_start * eff_ch,
                            len * eff_ch * sizeof(float));
            }
            decoded = std::move(sliced);
            total_frames = len;
        }

        const uint32_t adj_loop_begin =
            (pBuffer->LoopBegin >= play_slice_start)
                ? std::min<uint32_t>(pBuffer->LoopBegin - play_slice_start, total_frames)
                : 0u;

        auto* qb = new (std::nothrow) QueuedAudioBuffer();
        if (!qb) {
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_OUTOFMEMORY, 9);
        }
        qb->samples = std::move(decoded);
        qb->channels = eff_ch;
        qb->total_frames = total_frames;
        qb->play_cursor = 0.0;
        qb->loop_begin = adj_loop_begin;
        qb->loop_length = (pBuffer->LoopLength > 0 && qb->loop_begin + pBuffer->LoopLength <= total_frames)
                              ? pBuffer->LoopLength
                              : (total_frames - qb->loop_begin);
        qb->loops_remaining = pBuffer->LoopCount;
        qb->end_of_stream = (pBuffer->Flags & OS_XAUDIO2_END_OF_STREAM) != 0;
        qb->context = pBuffer->pContext;

        if (!buffer_queue_.TryEnqueue(qb)) {
            delete qb;
            return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_E_INVALID_CALL, 9);
        }
        const uint32_t prev_cnt = queued_count_.fetch_add(1u, std::memory_order_acq_rel);
        if (prev_cnt == 0u) {
            current_context_.store(pBuffer->pContext, std::memory_order_release);
        }

        return RecordHr("IXAudio2SourceVoice::SubmitSourceBuffer", OS_XAUDIO2_S_OK, 9);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL FlushSourceBuffers() override {
        flush_epoch_.fetch_add(1u, std::memory_order_acq_rel);
        std::vector<void*> flushed_ctxs;
        if (QueuedAudioBuffer* cur = current_buf_.exchange(nullptr, std::memory_order_acq_rel)) {
            flushed_ctxs.push_back(cur->context);
            delete cur;
            DecrementQueuedCount();
        }
        while (QueuedAudioBuffer* q = buffer_queue_.TryDequeue()) {
            flushed_ctxs.push_back(q->context);
            delete q;
            DecrementQueuedCount();
        }
        if (queued_count_.load(std::memory_order_acquire) == 0u) {
            current_context_.store(nullptr, std::memory_order_release);
        }
        if (callback_) {
            for (void* ctx : flushed_ctxs) {
                callback_->OnBufferEnd(ctx);
            }
        }
        return RecordHr("IXAudio2SourceVoice::FlushSourceBuffers", OS_XAUDIO2_S_OK, 10);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Discontinuity() override {
        discontinuity_requested_.store(true, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::Discontinuity", OS_XAUDIO2_S_OK, 11);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL ExitLoop(uint32_t /*OperationSet*/) override {
        exit_loop_requested_.store(true, std::memory_order_release);
        return RecordHr("IXAudio2SourceVoice::ExitLoop", OS_XAUDIO2_S_OK, 12);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetState(
        OS_XAUDIO2_VOICE_STATE* pVoiceState, uint32_t /*Flags*/) override {
        RecordOk("IXAudio2SourceVoice::GetState", 13);
        if (!pVoiceState) return;
        const uint32_t qcnt = queued_count_.load(std::memory_order_acquire);
        pVoiceState->BuffersQueued = qcnt;
        pVoiceState->pCurrentBufferContext =
            (qcnt == 0u) ? nullptr : current_context_.load(std::memory_order_acquire);
        pVoiceState->SamplesPlayed = samples_played_.load(std::memory_order_acquire);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetFrequencyRatio(
        float Ratio, uint32_t /*OperationSet*/) override {
        if (Ratio <= 0.0f || Ratio > max_freq_ratio_) {
            return RecordHr("IXAudio2SourceVoice::SetFrequencyRatio", OS_XAUDIO2_E_INVALIDARG, 14);
        }
        freq_ratio_.store(Ratio, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetFrequencyRatio", OS_XAUDIO2_S_OK, 14);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetFrequencyRatio(float* pRatio) override {
        RecordOk("IXAudio2SourceVoice::GetFrequencyRatio", 15);
        if (pRatio) *pRatio = freq_ratio_.load(std::memory_order_relaxed);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetSourceSampleRate(uint32_t NewSourceSampleRate) override {
        if (NewSourceSampleRate == 0) {
            return RecordHr("IXAudio2SourceVoice::SetSourceSampleRate", OS_XAUDIO2_E_INVALIDARG, 16);
        }
        sample_rate_.store(NewSourceSampleRate, std::memory_order_relaxed);
        return RecordHr("IXAudio2SourceVoice::SetSourceSampleRate", OS_XAUDIO2_S_OK, 16);
    }

    bool IsPlaying() const { return playing_.load(std::memory_order_acquire); }
    bool IsXma2() const { return effective_format_tag_ == OS_WAVE_FORMAT_XMA2; }

    void MixInto(
        float* master_interleaved,
        uint32_t out_frames,
        uint32_t out_channels,
        uint32_t out_sample_rate,
        DeferredVoiceCallbacks& out_cbs) {
        if (!playing_.load(std::memory_order_acquire) ||
            destroying_.load(std::memory_order_acquire)) {
            return;
        }

        // Prevent concurrent mix passes from overlapping on the same voice without blocking!
        if (mix_in_progress_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        struct MixGuard {
            std::atomic<bool>& flag;
            ~MixGuard() { flag.store(false, std::memory_order_release); }
        } guard{mix_in_progress_};

        const uint32_t start_flush_epoch = flush_epoch_.load(std::memory_order_acquire);
        QueuedAudioBuffer* cur = current_buf_.exchange(nullptr, std::memory_order_acq_rel);
        if (!cur) {
            cur = buffer_queue_.TryDequeue();
        }
        if (!cur) {
            return;
        }
        current_context_.store(cur->context, std::memory_order_release);

        out_cbs.cb = callback_;
        out_cbs.pass_started = true;
        out_cbs.bytes_required = 0u;

        const float voice_vol = volume_.load(std::memory_order_relaxed);
        const double src_rate = static_cast<double>(sample_rate_.load(std::memory_order_relaxed));
        const double ratio = static_cast<double>(freq_ratio_.load(std::memory_order_relaxed));
        const double step = (src_rate / static_cast<double>(std::max(1u, out_sample_rate))) * ratio;

        const size_t total_samples = static_cast<size_t>(out_frames) * out_channels;
        if (voice_scratch_.size() != total_samples) {
            voice_scratch_.assign(total_samples, 0.0f);
        } else {
            std::fill_n(voice_scratch_.data(), total_samples, 0.0f);
        }

        float local_ch_vol[kMaxInlineChannels];
        for (uint32_t i = 0u; i < kMaxInlineChannels; ++i) {
            local_ch_vol[i] = channel_volumes_[i].load(std::memory_order_relaxed);
        }

        uint32_t mixed_frames = 0u;
        for (uint32_t f = 0; f < out_frames; ++f) {
            if (flush_epoch_.load(std::memory_order_acquire) != start_flush_epoch) {
                if (cur) {
                    out_cbs.ended.push_back(cur->context);
                    delete cur;
                    cur = nullptr;
                    DecrementQueuedCount();
                }
                break;
            }
            if (exit_loop_requested_.exchange(false, std::memory_order_acq_rel) && cur) {
                cur->loops_remaining = 0u;
            }

            while (cur != nullptr) {
                if (!cur->started_notified) {
                    cur->started_notified = true;
                    out_cbs.started.push_back(cur->context);
                }
                if (cur->total_frames == 0 ||
                    cur->play_cursor >= static_cast<double>(cur->total_frames)) {
                    if (cur->loops_remaining > 0 && cur->loop_length > 0) {
                        if (cur->loops_remaining != OS_XAUDIO2_LOOP_INFINITE) {
                            --cur->loops_remaining;
                        }
                        cur->play_cursor = static_cast<double>(cur->loop_begin);
                        out_cbs.loop_ended.push_back(cur->context);
                        break;
                    }
                    const bool eos = cur->end_of_stream;
                    out_cbs.ended.push_back(cur->context);
                    delete cur;
                    DecrementQueuedCount();
                    cur = buffer_queue_.TryDequeue();
                    if (!cur && discontinuity_requested_.exchange(false, std::memory_order_acq_rel)) {
                        out_cbs.stream_ended = true;
                    }
                    if (eos) {
                        out_cbs.stream_ended = true;
                    }
                    current_context_.store(cur ? cur->context : nullptr, std::memory_order_release);
                    continue;
                }
                break;
            }
            if (!cur) {
                break;
            }

            const uint32_t i0 = static_cast<uint32_t>(cur->play_cursor);
            const uint32_t i1 = std::min<uint32_t>(i0 + 1, cur->total_frames - 1);
            const float frac = static_cast<float>(cur->play_cursor - static_cast<double>(i0));

            const uint32_t src_ch = cur->channels;
            for (uint32_t oc = 0; oc < out_channels; ++oc) {
                const uint32_t sc = (src_ch == 1) ? 0u : (oc % src_ch);
                const float s0 = cur->samples[static_cast<size_t>(i0) * src_ch + sc];
                const float s1 = cur->samples[static_cast<size_t>(i1) * src_ch + sc];
                const float sample = s0 + (s1 - s0) * frac;
                const float ch_gain = (sc < kMaxInlineChannels) ? local_ch_vol[sc] : 1.0f;
                voice_scratch_[static_cast<size_t>(f) * out_channels + oc] =
                    sample * voice_vol * ch_gain;
            }

            cur->play_cursor += step;
            ++mixed_frames;
            samples_played_.fetch_add(1ull, std::memory_order_release);

            if (cur->loops_remaining > 0u && cur->loop_length > 0u &&
                cur->play_cursor >= static_cast<double>(cur->loop_begin + cur->loop_length)) {
                const double overshoot =
                    cur->play_cursor - static_cast<double>(cur->loop_begin + cur->loop_length);
                if (cur->loops_remaining != OS_XAUDIO2_LOOP_INFINITE) {
                    --cur->loops_remaining;
                }
                cur->play_cursor = static_cast<double>(cur->loop_begin) +
                                   std::fmod(overshoot, static_cast<double>(cur->loop_length));
                out_cbs.loop_ended.push_back(cur->context);
            } else if (cur->play_cursor >= static_cast<double>(cur->total_frames)) {
                const bool eos = cur->end_of_stream;
                out_cbs.ended.push_back(cur->context);
                delete cur;
                DecrementQueuedCount();
                cur = buffer_queue_.TryDequeue();
                if (!cur && discontinuity_requested_.exchange(false, std::memory_order_acq_rel)) {
                    out_cbs.stream_ended = true;
                }
                if (eos) {
                    out_cbs.stream_ended = true;
                }
                current_context_.store(cur ? cur->context : nullptr, std::memory_order_release);
            }
        }

        if (cur) {
            if (flush_epoch_.load(std::memory_order_acquire) != start_flush_epoch) {
                out_cbs.ended.push_back(cur->context);
                delete cur;
                DecrementQueuedCount();
                current_context_.store(nullptr, std::memory_order_release);
            } else {
                QueuedAudioBuffer* prev = current_buf_.exchange(cur, std::memory_order_acq_rel);
                delete prev;
            }
        }

        if (mixed_frames > 0u) {
            if (has_effects_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(config_mutex_);
                for (const auto& slot : effect_chain_) {
                    if (slot.enabled && slot.effect) {
                        ProcessEffectApo(
                            slot.effect, voice_scratch_.data(), out_frames, out_channels, out_sample_rate);
                    }
                }
            }
            if (send_to_master_.load(std::memory_order_acquire)) {
                for (size_t i = 0u; i < total_samples; ++i) {
                    master_interleaved[i] += voice_scratch_[i];
                }
            }
            if (has_submix_sends_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(config_mutex_);
                for (auto* sub : send_submixes_) {
                    if (sub && sub->BusData()) {
                        float* dst = sub->BusData();
                        for (size_t i = 0u; i < total_samples; ++i) {
                            dst[i] += voice_scratch_[i];
                        }
                    }
                }
            }
        }
    }

private:
    void DecrementQueuedCount() noexcept {
        uint32_t cur = queued_count_.load(std::memory_order_acquire);
        while (cur > 0u &&
               !queued_count_.compare_exchange_weak(cur, cur - 1u, std::memory_order_acq_rel)) {
        }
    }

    XAudio2Impl* engine_;
    OS_WAVEFORMATEX format_{};
    uint16_t effective_format_tag_ = OS_WAVE_FORMAT_PCM;
    uint32_t flags_ = 0;
    float max_freq_ratio_ = 2.0f;
    IXAudio2VoiceCallback* callback_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<bool> destroying_{false};
    std::atomic<bool> mix_in_progress_{false};
    std::atomic<uint32_t> slot_index_{0xFFFFFFFFu};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> freq_ratio_{1.0f};
    std::atomic<uint32_t> sample_rate_{48000u};
    std::array<std::atomic<float>, kMaxInlineChannels> channel_volumes_{};

    std::mutex decoder_mutex_;
    std::unique_ptr<openscarlett::Xma2Decoder> xma2_decoder_;
    std::unique_ptr<openscarlett::ImaAdpcmDecoder> adpcm_decoder_;
    std::unique_ptr<openscarlett::OpusMftDecoder> opus_decoder_;
    uint16_t adpcm_samples_per_block_ = 0u;
    uint16_t opus_pre_skip_ = 0u;
    std::atomic<bool> opus_mft_active_{false};

    std::atomic<bool> send_to_master_{true};
    std::atomic<bool> has_submix_sends_{false};
    std::atomic<bool> has_effects_{false};
    std::mutex config_mutex_;
    std::vector<SubmixVoiceImpl*> send_submixes_;
    std::vector<VoiceEffectSlot> effect_chain_;
    std::vector<float> voice_scratch_;

    LockFreeBufferQueue buffer_queue_;
    std::atomic<QueuedAudioBuffer*> current_buf_{nullptr};
    std::atomic<uint32_t> queued_count_{0u};
    std::atomic<void*> current_context_{nullptr};
    std::atomic<uint32_t> flush_epoch_{0u};
    std::atomic<bool> exit_loop_requested_{false};
    std::atomic<bool> discontinuity_requested_{false};
    std::atomic<uint64_t> samples_played_{0ull};
};

// ===========================================================================
// IXAudio2 Engine Implementation (Lock-Free 256-Voice Software Mixer + Live WASAPI)
// ===========================================================================
class XAudio2Impl final : public IXAudio2 {
public:
    static constexpr uint32_t kMaxLockFreeSourceVoices = 256u;  // >= 128 simultaneous source voices
    static constexpr uint32_t kMaxMasterChannels = 8u;

    XAudio2Impl() {
        for (auto& mv : master_ch_vol_atomic_) {
            mv.store(1.0f, std::memory_order_relaxed);
        }
    }

    ~XAudio2Impl() {
        StopWasapiThread();
        // Drain lock-free source voice slots safely after WASAPI stops.
        for (auto& slot : voice_slots_) {
            SourceVoiceImpl* v = slot.voice.exchange(nullptr, std::memory_order_seq_cst);
            if (v) {
                v->MarkDestroying();
                while (slot.active_mixers.load(std::memory_order_seq_cst) != 0u) {
                    std::this_thread::yield();
                }
                delete v;
            }
        }
        std::lock_guard<std::mutex> lk(mutex_);
        submix_voices_.clear();
        mastering_voice_.reset();
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) || GuidEquals(riid, IID_OS_IXAudio2)) {
            AddRef();
            *ppvObject = static_cast<IXAudio2*>(this);
            return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_S_OK, 0);
        }
        return RecordHr("IXAudio2::QueryInterface", OS_XAUDIO2_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        RecordOk("IXAudio2::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        RecordOk("IXAudio2::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL RegisterForCallbacks(
        IXAudio2EngineCallback* pCallback) override {
        if (!pCallback) {
            return RecordHr("IXAudio2::RegisterForCallbacks", OS_XAUDIO2_E_INVALIDARG, 3);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (std::find(engine_callbacks_.begin(), engine_callbacks_.end(), pCallback) ==
            engine_callbacks_.end()) {
            engine_callbacks_.push_back(pCallback);
            has_engine_callbacks_.store(true, std::memory_order_release);
        }
        return RecordHr("IXAudio2::RegisterForCallbacks", OS_XAUDIO2_S_OK, 3);
    }

    void OPENSCARLETT_XAUDIO2_CALL UnregisterForCallbacks(
        IXAudio2EngineCallback* pCallback) override {
        RecordOk("IXAudio2::UnregisterForCallbacks", 4);
        std::lock_guard<std::mutex> lk(mutex_);
        engine_callbacks_.erase(
            std::remove(engine_callbacks_.begin(), engine_callbacks_.end(), pCallback),
            engine_callbacks_.end());
        has_engine_callbacks_.store(!engine_callbacks_.empty(), std::memory_order_release);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const OS_WAVEFORMATEX* pSourceFormat,
        uint32_t Flags,
        float MaxFrequencyRatio,
        IXAudio2VoiceCallback* pCallback,
        const OS_XAUDIO2_VOICE_SENDS* pSendList,
        const void* pEffectChain) override {
        if (!ppSourceVoice || !pSourceFormat) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }
        *ppSourceVoice = nullptr;
        if (pSourceFormat->nChannels == 0 || pSourceFormat->nSamplesPerSec == 0) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }

        uint16_t effective_tag = pSourceFormat->wFormatTag;
        if (effective_tag == OS_WAVE_FORMAT_EXTENSIBLE) {
            if (pSourceFormat->cbSize >= 22) {
                const auto* ext = reinterpret_cast<const OS_WAVEFORMATEXTENSIBLE*>(pSourceFormat);
                effective_tag = static_cast<uint16_t>(ext->SubFormat.Data1 & 0xFFFFu);
            } else {
                effective_tag = OS_WAVE_FORMAT_PCM;
            }
        }
        if (effective_tag != OS_WAVE_FORMAT_PCM &&
            effective_tag != OS_WAVE_FORMAT_IEEE_FLOAT &&
            effective_tag != OS_WAVE_FORMAT_IMA_ADPCM &&
            effective_tag != OS_WAVE_FORMAT_XBOX_ADPCM &&
            effective_tag != OS_WAVE_FORMAT_XMA2 &&
            effective_tag != OS_WAVE_FORMAT_OPUS) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_INVALIDARG, 5);
        }

        auto voice = std::make_unique<SourceVoiceImpl>(
            this, *pSourceFormat, Flags, MaxFrequencyRatio, pCallback);
        voice->SetEffectiveFormatTag(effective_tag);
        voice->SetSourceSampleRate(pSourceFormat->nSamplesPerSec);
        if ((effective_tag == OS_WAVE_FORMAT_IMA_ADPCM ||
             effective_tag == OS_WAVE_FORMAT_XBOX_ADPCM) &&
            pSourceFormat->cbSize >= 2u) {
            const auto* ima = reinterpret_cast<const OS_IMAADPCMWAVEFORMAT*>(pSourceFormat);
            voice->SetAdpcmSamplesPerBlock(ima->wSamplesPerBlock);
        } else if (effective_tag == OS_WAVE_FORMAT_OPUS && pSourceFormat->cbSize >= 2u) {
            const auto* opus = reinterpret_cast<const OS_OPUSWAVEFORMAT*>(pSourceFormat);
            voice->SetOpusPreSkip(opus->PreSkip);
        }
        if (pSendList) {
            voice->SetOutputVoices(pSendList);
        }
        if (pEffectChain) {
            const int32_t ehr = voice->SetEffectChain(pEffectChain);
            if (ehr < 0) {
                return RecordHr("IXAudio2::CreateSourceVoice", ehr, 5);
            }
        }

        SourceVoiceImpl* raw = voice.get();
        uint32_t chosen_slot = kMaxLockFreeSourceVoices;
        for (uint32_t i = 0u; i < kMaxLockFreeSourceVoices; ++i) {
            SourceVoiceImpl* expected = nullptr;
            if (voice_slots_[i].voice.compare_exchange_strong(
                    expected, raw, std::memory_order_seq_cst, std::memory_order_acquire)) {
                chosen_slot = i;
                break;
            }
        }
        if (chosen_slot >= kMaxLockFreeSourceVoices) {
            return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_E_OUTOFMEMORY, 5);
        }
        raw->SetSlotIndex(chosen_slot);
        voice.release();  // Ownership transferred to lock-free slot table.

        uint32_t hw = voice_slot_high_water_.load(std::memory_order_acquire);
        while (chosen_slot + 1u > hw &&
               !voice_slot_high_water_.compare_exchange_weak(
                   hw, chosen_slot + 1u, std::memory_order_acq_rel)) {
        }

        *ppSourceVoice = raw;
        return RecordHr("IXAudio2::CreateSourceVoice", OS_XAUDIO2_S_OK, 5);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        uint32_t InputChannels,
        uint32_t InputSampleRate,
        uint32_t Flags,
        uint32_t ProcessingStage,
        const OS_XAUDIO2_VOICE_SENDS* pSendList,
        const void* pEffectChain) override {
        if (!ppSubmixVoice || InputChannels == 0 || InputSampleRate == 0) {
            return RecordHr("IXAudio2::CreateSubmixVoice", OS_XAUDIO2_E_INVALIDARG, 6);
        }
        auto sub = std::make_unique<SubmixVoiceImpl>(
            this, InputChannels, InputSampleRate, Flags, ProcessingStage);
        if (pSendList) {
            sub->SetOutputVoices(pSendList);
        }
        if (pEffectChain) {
            const int32_t ehr = sub->SetEffectChain(pEffectChain);
            if (ehr < 0) {
                return RecordHr("IXAudio2::CreateSubmixVoice", ehr, 6);
            }
        }
        SubmixVoiceImpl* raw = sub.get();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            submix_voices_.push_back(std::move(sub));
            has_submixes_.store(true, std::memory_order_release);
        }
        *ppSubmixVoice = raw;
        return RecordHr("IXAudio2::CreateSubmixVoice", OS_XAUDIO2_S_OK, 6);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        uint32_t InputChannels,
        uint32_t InputSampleRate,
        uint32_t Flags,
        const wchar_t* szDeviceId,
        const void* pEffectChain,
        uint32_t /*StreamCategory*/) override {
        if (!ppMasteringVoice) {
            return RecordHr("IXAudio2::CreateMasteringVoice", OS_XAUDIO2_E_INVALIDARG, 7);
        }
        *ppMasteringVoice = nullptr;
        const ParsedEndpointId parsed_ep = ParseMmDeviceEndpointIdW(szDeviceId);
        if (!parsed_ep.valid) {
            return RecordHr("IXAudio2::CreateMasteringVoice", OS_XAUDIO2_E_INVALIDARG, 7);
        }
#if defined(_WIN32)
        {
            std::lock_guard<std::mutex> wlk(wasapi_mutex_);
            requested_endpoint_id_ = parsed_ep.is_default ? std::wstring() : parsed_ep.canonical_id;
        }
#endif
        {
            std::lock_guard<std::mutex> lk(mutex_);
            const uint32_t eff_ch = InputChannels != 0 ? InputChannels : 2u;
            const uint32_t eff_sr = InputSampleRate != 0 ? InputSampleRate : 48000u;
            auto mv = std::make_unique<MasteringVoiceImpl>(this, eff_ch, eff_sr, Flags);
            if (pEffectChain) {
                const int32_t ehr = mv->SetEffectChain(pEffectChain);
                if (ehr < 0) {
                    return RecordHr("IXAudio2::CreateMasteringVoice", ehr, 7);
                }
                has_master_effects_.store(true, std::memory_order_release);
            } else {
                has_master_effects_.store(false, std::memory_order_release);
            }
            master_channels_atomic_.store(eff_ch, std::memory_order_release);
            master_sr_atomic_.store(eff_sr, std::memory_order_release);
            mastering_voice_ = std::move(mv);
            *ppMasteringVoice = mastering_voice_.get();
        }
        StartWasapiThreadIfAllowed();
        return RecordHr("IXAudio2::CreateMasteringVoice", OS_XAUDIO2_S_OK, 7);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL StartEngine() override {
        engine_running_.store(true, std::memory_order_release);
        StartWasapiThreadIfAllowed();
        return RecordHr("IXAudio2::StartEngine", OS_XAUDIO2_S_OK, 8);
    }

    void OPENSCARLETT_XAUDIO2_CALL StopEngine() override {
        RecordOk("IXAudio2::StopEngine", 9);
        engine_running_.store(false, std::memory_order_release);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL CommitChanges(uint32_t /*OperationSet*/) override {
        return RecordHr("IXAudio2::CommitChanges", OS_XAUDIO2_S_OK, 10);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetPerformanceData(
        OS_XAUDIO2_PERFORMANCE_DATA* pPerfData) override {
        RecordOk("IXAudio2::GetPerformanceData", 11);
        if (!pPerfData) return;
        std::memset(pPerfData, 0, sizeof(*pPerfData));
        uint32_t total_src = 0u;
        uint32_t active_src = 0u;
        uint32_t active_xma = 0u;
        const uint32_t hw = std::min<uint32_t>(
            voice_slot_high_water_.load(std::memory_order_acquire), kMaxLockFreeSourceVoices);
        for (uint32_t i = 0u; i < hw; ++i) {
            auto& slot = voice_slots_[i];
            slot.active_mixers.fetch_add(1u, std::memory_order_seq_cst);
            SourceVoiceImpl* v = slot.voice.load(std::memory_order_seq_cst);
            if (v && !v->IsDestroying()) {
                ++total_src;
                if (v->IsPlaying()) {
                    ++active_src;
                    if (v->IsXma2()) ++active_xma;
                }
            }
            slot.active_mixers.fetch_sub(1u, std::memory_order_acq_rel);
        }
        uint32_t sub_cnt = 0u;
        if (has_submixes_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(mutex_);
            sub_cnt = static_cast<uint32_t>(submix_voices_.size());
        }
        pPerfData->ActiveSourceVoiceCount = active_src;
        pPerfData->TotalSourceVoiceCount = total_src;
        pPerfData->ActiveSubmixVoiceCount = sub_cnt;
        pPerfData->ActiveXmaSourceVoices = active_xma;
        pPerfData->ActiveXmaStreams = active_xma;
        pPerfData->CurrentLatencyInSamples = 480u;
    }

    void OPENSCARLETT_XAUDIO2_CALL SetDebugConfiguration(
        const OS_XAUDIO2_DEBUG_CONFIGURATION* /*pDebugConfiguration*/,
        void* /*pReserved*/) override {
        RecordOk("IXAudio2::SetDebugConfiguration", 12);
    }

    void NotifyMasterEffectChainChanged() {
        has_master_effects_.store(true, std::memory_order_release);
    }

    void RemoveSourceVoice(SourceVoiceImpl* v) {
        if (!v) return;
        v->MarkDestroying();
        v->Stop(0u, 0u);
        const uint32_t idx = v->SlotIndex();
        if (idx < kMaxLockFreeSourceVoices) {
            auto& slot = voice_slots_[idx];
            SourceVoiceImpl* expected = v;
            if (slot.voice.compare_exchange_strong(
                    expected, nullptr, std::memory_order_seq_cst, std::memory_order_acquire)) {
                while (slot.active_mixers.load(std::memory_order_seq_cst) != 0u) {
                    std::this_thread::yield();
                }
                delete v;
            }
        }
    }

    void RemoveSubmixVoice(SubmixVoiceImpl* v) {
        const uint32_t hw = std::min<uint32_t>(
            voice_slot_high_water_.load(std::memory_order_acquire), kMaxLockFreeSourceVoices);
        for (uint32_t i = 0u; i < hw; ++i) {
            auto& slot = voice_slots_[i];
            slot.active_mixers.fetch_add(1u, std::memory_order_seq_cst);
            SourceVoiceImpl* sv = slot.voice.load(std::memory_order_seq_cst);
            if (sv && !sv->IsDestroying()) {
                sv->ClearSubmixReference(v);
            }
            slot.active_mixers.fetch_sub(1u, std::memory_order_acq_rel);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& sub : submix_voices_) {
            if (sub.get() != v) {
                sub->ClearSubmixReference(v);
            }
        }
        submix_voices_.erase(
            std::remove_if(submix_voices_.begin(), submix_voices_.end(),
                           [&](const std::unique_ptr<SubmixVoiceImpl>& p) { return p.get() == v; }),
            submix_voices_.end());
        has_submixes_.store(!submix_voices_.empty(), std::memory_order_release);
    }

    void RemoveMasteringVoice(MasteringVoiceImpl* v) {
        StopWasapiThread();
        std::lock_guard<std::mutex> lk(mutex_);
        if (mastering_voice_.get() == v) {
            mastering_voice_.reset();
            has_master_effects_.store(false, std::memory_order_release);
        }
    }

    uint64_t TotalFramesMixed() const {
        return total_frames_mixed_.load(std::memory_order_acquire);
    }

    void GetLockFreeMixerStats(uint32_t* out_cap, uint32_t* out_peak, uint64_t* out_passes) const {
        if (out_cap) *out_cap = kMaxLockFreeSourceVoices;
        if (out_peak) *out_peak = peak_concurrent_voices_.load(std::memory_order_acquire);
        if (out_passes) *out_passes = lock_free_mix_passes_.load(std::memory_order_acquire);
    }

    int32_t RenderMixFrames(
        float* out_frames,
        uint32_t frame_count,
        uint32_t override_channels = 0,
        uint32_t override_sample_rate = 0) {
        if (!out_frames || frame_count == 0) {
            return OS_XAUDIO2_E_INVALIDARG;
        }

        std::vector<IXAudio2EngineCallback*> eng_cbs;
        std::vector<DeferredVoiceCallbacks> voice_cbs;
        uint32_t out_ch = master_channels_atomic_.load(std::memory_order_acquire);
        uint32_t out_sr = master_sr_atomic_.load(std::memory_order_acquire);
        float master_vol = 1.0f;
        std::vector<float> master_ch_vol;

        const bool need_engine_lock =
            has_engine_callbacks_.load(std::memory_order_acquire) ||
            has_submixes_.load(std::memory_order_acquire) ||
            has_master_effects_.load(std::memory_order_acquire);

        if (need_engine_lock) {
            std::lock_guard<std::mutex> lk(mutex_);
            eng_cbs = engine_callbacks_;
            if (mastering_voice_) {
                out_ch = mastering_voice_->Channels();
                out_sr = mastering_voice_->SampleRate();
                master_vol = mastering_voice_->Volume();
                master_ch_vol = mastering_voice_->SnapshotChannelVolumes();
            }
        } else {
            // Fast lock-free master snapshot (zero mutex contention in 128-voice mix path!)
            master_vol = master_vol_atomic_.load(std::memory_order_relaxed);
            master_ch_vol.resize(std::min<uint32_t>(out_ch, kMaxMasterChannels));
            for (size_t i = 0u; i < master_ch_vol.size(); ++i) {
                master_ch_vol[i] = master_ch_vol_atomic_[i].load(std::memory_order_relaxed);
            }
        }

        if (override_channels != 0) out_ch = override_channels;
        if (override_sample_rate != 0) out_sr = override_sample_rate;

        std::fill_n(out_frames, static_cast<size_t>(frame_count) * out_ch, 0.0f);

        if (engine_running_.load(std::memory_order_acquire)) {
            if (has_submixes_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(mutex_);
                for (auto& sub : submix_voices_) {
                    sub->PrepareBus(frame_count, out_ch);
                }
            }

            // Completely lock-free iteration across up to 256 concurrent source voices!
            const uint32_t hw = std::min<uint32_t>(
                voice_slot_high_water_.load(std::memory_order_acquire), kMaxLockFreeSourceVoices);
            uint32_t active_in_pass = 0u;
            for (uint32_t i = 0u; i < hw; ++i) {
                auto& slot = voice_slots_[i];
                slot.active_mixers.fetch_add(1u, std::memory_order_seq_cst);
                SourceVoiceImpl* v = slot.voice.load(std::memory_order_seq_cst);
                if (v && !v->IsDestroying()) {
                    if (v->IsPlaying()) {
                        ++active_in_pass;
                    }
                    DeferredVoiceCallbacks dcb{};
                    v->MixInto(out_frames, frame_count, out_ch, out_sr, dcb);
                    if (dcb.cb && (dcb.pass_started || !dcb.started.empty() ||
                                   !dcb.loop_ended.empty() || !dcb.ended.empty() ||
                                   dcb.stream_ended)) {
                        voice_cbs.push_back(std::move(dcb));
                    }
                }
                slot.active_mixers.fetch_sub(1u, std::memory_order_acq_rel);
            }

            uint32_t prev_peak = peak_concurrent_voices_.load(std::memory_order_relaxed);
            while (active_in_pass > prev_peak &&
                   !peak_concurrent_voices_.compare_exchange_weak(
                       prev_peak, active_in_pass, std::memory_order_acq_rel)) {
            }
            lock_free_mix_passes_.fetch_add(1ull, std::memory_order_release);

            if (has_submixes_.load(std::memory_order_acquire) ||
                has_master_effects_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!submix_voices_.empty()) {
                    std::vector<SubmixVoiceImpl*> ordered;
                    ordered.reserve(submix_voices_.size());
                    for (auto& sub : submix_voices_) {
                        ordered.push_back(sub.get());
                    }
                    std::stable_sort(
                        ordered.begin(), ordered.end(),
                        [](const SubmixVoiceImpl* a, const SubmixVoiceImpl* b) {
                            return a->ProcessingStage() < b->ProcessingStage();
                        });
                    for (auto* sub : ordered) {
                        sub->ProcessAndAccumulateTo(out_frames, frame_count, out_ch, out_sr);
                    }
                }
                if (mastering_voice_) {
                    mastering_voice_->ProcessMasterEffects(out_frames, frame_count, out_ch, out_sr);
                }
            }
        }

        // Apply mastering volume + clamp to [-1, 1].
        for (uint32_t f = 0; f < frame_count; ++f) {
            for (uint32_t c = 0; c < out_ch; ++c) {
                const float cg = (c < master_ch_vol.size()) ? master_ch_vol[c] : 1.0f;
                float& s = out_frames[static_cast<size_t>(f) * out_ch + c];
                s = std::clamp(s * master_vol * cg, -1.0f, 1.0f);
            }
        }

        total_frames_mixed_.fetch_add(frame_count, std::memory_order_release);

        // Deliver callbacks strictly outside internal locks.
        for (auto* ecb : eng_cbs) {
            if (ecb) ecb->OnProcessingPassStart();
        }
        for (const auto& dcb : voice_cbs) {
            if (dcb.pass_started) {
                dcb.cb->OnVoiceProcessingPassStart(dcb.bytes_required);
            }
            for (void* ctx : dcb.started) dcb.cb->OnBufferStart(ctx);
            for (void* ctx : dcb.loop_ended) dcb.cb->OnLoopEnd(ctx);
            for (void* ctx : dcb.ended) dcb.cb->OnBufferEnd(ctx);
            if (dcb.stream_ended) dcb.cb->OnStreamEnd();
            if (dcb.pass_started) {
                dcb.cb->OnVoiceProcessingPassEnd();
            }
        }
        for (auto* ecb : eng_cbs) {
            if (ecb) ecb->OnProcessingPassEnd();
        }
        return OS_XAUDIO2_S_OK;
    }

    void SyncMasterVolumeAtomic(float v) noexcept {
        master_vol_atomic_.store(v, std::memory_order_relaxed);
    }

    void SyncMasterChannelVolumesAtomic(uint32_t channels, const float* vols) noexcept {
        const uint32_t n = std::min<uint32_t>(channels, kMaxMasterChannels);
        for (uint32_t i = 0u; i < n; ++i) {
            master_ch_vol_atomic_[i].store(vols[i], std::memory_order_relaxed);
        }
    }

    void GetBackendInfo(OS_XAUDIO2_BACKEND_INFO& out) {
        std::memset(&out, 0, sizeof(out));
        out.null_audio_forced = IsNullAudioForced() ? 1u : 0u;
#if defined(_WIN32)
        out.wasapi_active = wasapi_active_.load(std::memory_order_acquire) ? 1u : 0u;
        out.wasapi_event_driven = wasapi_event_driven_.load(std::memory_order_acquire) ? 1u : 0u;
        out.wasapi_sample_rate = wasapi_sample_rate_.load(std::memory_order_acquire);
        out.wasapi_channels = wasapi_channels_.load(std::memory_order_acquire);
        out.wasapi_bits_per_sample = wasapi_bits_per_sample_.load(std::memory_order_acquire);
        out.wasapi_buffer_frames = wasapi_buffer_frames_.load(std::memory_order_acquire);
        out.wasapi_frames_rendered = wasapi_frames_rendered_.load(std::memory_order_acquire);
        out.wasapi_last_hresult = wasapi_last_hr_.load(std::memory_order_acquire);
#endif
        uint32_t opus_mft_count = 0u;
        const uint32_t hw = std::min<uint32_t>(
            voice_slot_high_water_.load(std::memory_order_acquire), kMaxLockFreeSourceVoices);
        for (uint32_t i = 0u; i < hw; ++i) {
            auto& slot = voice_slots_[i];
            slot.active_mixers.fetch_add(1u, std::memory_order_seq_cst);
            SourceVoiceImpl* v = slot.voice.load(std::memory_order_seq_cst);
            if (v && !v->IsDestroying() && v->IsOpusMftActive()) {
                ++opus_mft_count;
            }
            slot.active_mixers.fetch_sub(1u, std::memory_order_acq_rel);
        }
        out.opus_mft_active_voices = opus_mft_count;
    }

private:
    void StartWasapiThreadIfAllowed() {
#if defined(_WIN32)
        if (IsNullAudioForced()) {
            return;
        }
        std::unique_lock<std::mutex> lk(wasapi_mutex_);
        if (wasapi_thread_.joinable()) {
            return;
        }
        wasapi_stop_.store(false, std::memory_order_release);
        wasapi_init_done_ = false;
        if (!wasapi_stop_event_) {
            wasapi_stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        } else {
            ResetEvent(wasapi_stop_event_);
        }
        wasapi_thread_ = std::thread(&XAudio2Impl::WasapiRenderLoop, this);
        wasapi_init_cv_.wait_for(lk, std::chrono::milliseconds(2000), [&]() {
            return wasapi_init_done_;
        });
#endif
    }

    void StopWasapiThread() {
#if defined(_WIN32)
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            wasapi_stop_.store(true, std::memory_order_release);
            if (wasapi_stop_event_) {
                SetEvent(wasapi_stop_event_);
            }
            t.swap(wasapi_thread_);
        }
        if (t.joinable()) {
            t.join();
        }
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            if (wasapi_stop_event_) {
                CloseHandle(wasapi_stop_event_);
                wasapi_stop_event_ = nullptr;
            }
        }
#endif
    }

#if defined(_WIN32)
    void SignalWasapiInitDone() {
        {
            std::lock_guard<std::mutex> lk(wasapi_mutex_);
            wasapi_init_done_ = true;
        }
        wasapi_init_cv_.notify_all();
    }

    void WasapiRenderLoop() {
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool co_init = SUCCEEDED(co_hr);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render_client = nullptr;
        WAVEFORMATEX* mix_fmt = nullptr;
        HANDLE ready_event = nullptr;
        bool init_signaled = false;

        auto signal_once = [&]() {
            if (!init_signaled) {
                init_signaled = true;
                SignalWasapiInitDone();
            }
        };

        auto cleanup = [&]() {
            wasapi_active_.store(false, std::memory_order_release);
            signal_once();
            if (client) client->Stop();
            if (ready_event) CloseHandle(ready_event);
            if (mix_fmt) CoTaskMemFree(mix_fmt);
            if (render_client) render_client->Release();
            if (client) client->Release();
            if (device) device->Release();
            if (enumerator) enumerator->Release();
            if (co_init) CoUninitialize();
        };

        HRESULT hr = CoCreateInstance(
            kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
            kIidImmDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !enumerator) {
            cleanup();
            return;
        }

        std::wstring req_endpoint;
        {
            std::lock_guard<std::mutex> wlk(wasapi_mutex_);
            req_endpoint = requested_endpoint_id_;
        }
        if (!req_endpoint.empty()) {
            hr = enumerator->GetDevice(req_endpoint.c_str(), &device);
        } else {
            hr = E_FAIL;
        }
        if (FAILED(hr) || !device) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (FAILED(hr) || !device) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        }
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !device || wasapi_stop_.load(std::memory_order_acquire)) {
            cleanup();
            return;
        }

        hr = device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !client || wasapi_stop_.load(std::memory_order_acquire)) {
            cleanup();
            return;
        }

        hr = client->GetMixFormat(&mix_fmt);
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !mix_fmt) {
            cleanup();
            return;
        }

        REFERENCE_TIME default_period = 200000;  // 20 ms default
        REFERENCE_TIME min_period = 0;
        (void)client->GetDevicePeriod(&default_period, &min_period);
        if (default_period <= 0) default_period = 200000;

        bool event_driven = false;
        ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ready_event) {
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                default_period,
                0,
                mix_fmt,
                nullptr);
            if (SUCCEEDED(hr)) {
                hr = client->SetEventHandle(ready_event);
                if (SUCCEEDED(hr)) {
                    event_driven = true;
                }
            }
            if (!event_driven) {
                // Re-activate fresh IAudioClient for polling fallback (Initialize is single-use).
                client->Release();
                client = nullptr;
                CloseHandle(ready_event);
                ready_event = nullptr;
                hr = device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&client));
                if (FAILED(hr) || !client) {
                    wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
                    cleanup();
                    return;
                }
            }
        }

        if (!event_driven) {
            constexpr REFERENCE_TIME kPollBufferDuration = 500000;  // 50 ms
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, 0, kPollBufferDuration, 0, mix_fmt, nullptr);
            wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
            if (FAILED(hr)) {
                cleanup();
                return;
            }
        }

        UINT32 buffer_frames = 0;
        hr = client->GetBufferSize(&buffer_frames);
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || buffer_frames == 0) {
            cleanup();
            return;
        }

        hr = client->GetService(kIidIAudioRenderClient,
                                reinterpret_cast<void**>(&render_client));
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr) || !render_client) {
            cleanup();
            return;
        }

        const uint32_t out_ch = std::max<uint32_t>(1u, mix_fmt->nChannels);
        const uint32_t out_sr = std::max<uint32_t>(1u, mix_fmt->nSamplesPerSec);
        bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
            is_float = (ext->SubFormat.Data1 == 0x00000003u);
        }

        // Pre-fill initial buffer with silence/mix before Start() to prevent startup glitches.
        std::vector<float> mix_buf;
        auto pump_available = [&]() -> bool {
            UINT32 padding = 0;
            const HRESULT pad_hr = client->GetCurrentPadding(&padding);
            if (FAILED(pad_hr)) {
                wasapi_last_hr_.store(static_cast<int32_t>(pad_hr), std::memory_order_release);
                return false;
            }
            const UINT32 avail = (padding < buffer_frames) ? (buffer_frames - padding) : 0u;
            if (avail == 0u) {
                return true;
            }
            BYTE* dst = nullptr;
            const HRESULT buf_hr = render_client->GetBuffer(avail, &dst);
            if (FAILED(buf_hr) || !dst) {
                wasapi_last_hr_.store(static_cast<int32_t>(buf_hr), std::memory_order_release);
                return buf_hr != AUDCLNT_E_DEVICE_INVALIDATED;
            }
            mix_buf.resize(static_cast<size_t>(avail) * out_ch);
            RenderMixFrames(mix_buf.data(), avail, out_ch, out_sr);
            if (is_float && mix_fmt->wBitsPerSample == 32u) {
                std::memcpy(dst, mix_buf.data(), mix_buf.size() * sizeof(float));
            } else if (mix_fmt->wBitsPerSample == 16u) {
                auto* out16 = reinterpret_cast<int16_t*>(dst);
                for (size_t i = 0u; i < mix_buf.size(); ++i) {
                    const float clamped = std::clamp(mix_buf[i], -1.0f, 1.0f);
                    out16[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
                }
            } else if (mix_fmt->wBitsPerSample == 32u) {
                auto* out32 = reinterpret_cast<int32_t*>(dst);
                for (size_t i = 0u; i < mix_buf.size(); ++i) {
                    const float clamped = std::clamp(mix_buf[i], -1.0f, 1.0f);
                    out32[i] = static_cast<int32_t>(static_cast<double>(clamped) * 2147483647.0);
                }
            } else {
                std::memset(dst, 0, static_cast<size_t>(avail) * mix_fmt->nBlockAlign);
            }
            const HRESULT rel_hr = render_client->ReleaseBuffer(avail, 0);
            if (SUCCEEDED(rel_hr)) {
                wasapi_frames_rendered_.fetch_add(avail, std::memory_order_relaxed);
            } else {
                wasapi_last_hr_.store(static_cast<int32_t>(rel_hr), std::memory_order_release);
            }
            return rel_hr != AUDCLNT_E_DEVICE_INVALIDATED;
        };

        (void)pump_available();

        hr = client->Start();
        wasapi_last_hr_.store(static_cast<int32_t>(hr), std::memory_order_release);
        if (FAILED(hr)) {
            cleanup();
            return;
        }

        wasapi_event_driven_.store(event_driven, std::memory_order_release);
        wasapi_sample_rate_.store(out_sr, std::memory_order_release);
        wasapi_channels_.store(out_ch, std::memory_order_release);
        wasapi_bits_per_sample_.store(mix_fmt->wBitsPerSample, std::memory_order_release);
        wasapi_buffer_frames_.store(buffer_frames, std::memory_order_release);
        wasapi_active_.store(true, std::memory_order_release);
        signal_once();

        while (!wasapi_stop_.load(std::memory_order_acquire)) {
            if (event_driven && ready_event && wasapi_stop_event_) {
                HANDLE wait_handles[2] = {wasapi_stop_event_, ready_event};
                const DWORD wr = WaitForMultipleObjects(2u, wait_handles, FALSE, 50u);
                if (wr == WAIT_OBJECT_0 || wasapi_stop_.load(std::memory_order_acquire)) {
                    break;
                }
            } else if (wasapi_stop_event_) {
                if (WaitForSingleObject(wasapi_stop_event_, 10u) == WAIT_OBJECT_0) {
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!pump_available()) {
                break;
            }
        }

        cleanup();
    }
#endif

    struct LockFreeVoiceSlot {
        std::atomic<SourceVoiceImpl*> voice{nullptr};
        std::atomic<uint32_t> active_mixers{0u};
    };

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<bool> engine_running_{true};
    std::atomic<uint64_t> total_frames_mixed_{0ull};
    std::atomic<uint32_t> master_channels_atomic_{2u};
    std::atomic<uint32_t> master_sr_atomic_{48000u};
    std::atomic<float> master_vol_atomic_{1.0f};
    std::array<std::atomic<float>, kMaxMasterChannels> master_ch_vol_atomic_{};
    std::atomic<bool> has_engine_callbacks_{false};
    std::atomic<bool> has_submixes_{false};
    std::atomic<bool> has_master_effects_{false};
    std::array<LockFreeVoiceSlot, kMaxLockFreeSourceVoices> voice_slots_{};
    std::atomic<uint32_t> voice_slot_high_water_{0u};
    std::atomic<uint32_t> peak_concurrent_voices_{0u};
    std::atomic<uint64_t> lock_free_mix_passes_{0ull};

    std::mutex mutex_;
    std::unique_ptr<MasteringVoiceImpl> mastering_voice_;
    std::vector<std::unique_ptr<SubmixVoiceImpl>> submix_voices_;
    std::vector<IXAudio2EngineCallback*> engine_callbacks_;

#if defined(_WIN32)
    std::mutex wasapi_mutex_;
    std::condition_variable wasapi_init_cv_;
    bool wasapi_init_done_ = false;
    std::wstring requested_endpoint_id_;
    HANDLE wasapi_stop_event_ = nullptr;
    std::atomic<bool> wasapi_stop_{false};
    std::atomic<bool> wasapi_active_{false};
    std::atomic<bool> wasapi_event_driven_{false};
    std::atomic<uint32_t> wasapi_sample_rate_{0u};
    std::atomic<uint32_t> wasapi_channels_{0u};
    std::atomic<uint32_t> wasapi_bits_per_sample_{0u};
    std::atomic<uint32_t> wasapi_buffer_frames_{0u};
    std::atomic<uint64_t> wasapi_frames_rendered_{0u};
    std::atomic<int32_t> wasapi_last_hr_{0};
    std::thread wasapi_thread_;
#endif
};

void OPENSCARLETT_XAUDIO2_CALL SubmixVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2SubmixVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveSubmixVoice(this);
    }
}

int32_t MasteringVoiceImpl::SetEffectChain(const void* pEffectChain) {
    int32_t hr = OS_XAUDIO2_S_OK;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        hr = ReplaceEffectSlots(effect_chain_, pEffectChain);
    }
    if (hr >= 0 && engine_) {
        engine_->NotifyMasterEffectChainChanged();
    }
    return hr;
}

int32_t OPENSCARLETT_XAUDIO2_CALL MasteringVoiceImpl::SetVolume(
    float Volume, uint32_t /*OperationSet*/) {
    volume_.store(Volume, std::memory_order_relaxed);
    if (engine_) {
        engine_->SyncMasterVolumeAtomic(Volume);
    }
    return RecordHr("IXAudio2MasteringVoice::SetVolume", OS_XAUDIO2_S_OK, 2);
}

int32_t OPENSCARLETT_XAUDIO2_CALL MasteringVoiceImpl::SetChannelVolumes(
    uint32_t Channels, const float* pVolumes, uint32_t /*OperationSet*/) {
    if (!pVolumes || Channels != channels_) {
        return RecordHr("IXAudio2MasteringVoice::SetChannelVolumes", OS_XAUDIO2_E_INVALIDARG, 4);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        channel_volumes_.assign(pVolumes, pVolumes + Channels);
    }
    if (engine_) {
        engine_->SyncMasterChannelVolumesAtomic(Channels, pVolumes);
    }
    return RecordHr("IXAudio2MasteringVoice::SetChannelVolumes", OS_XAUDIO2_S_OK, 4);
}

void OPENSCARLETT_XAUDIO2_CALL MasteringVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2MasteringVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveMasteringVoice(this);
    }
}

void OPENSCARLETT_XAUDIO2_CALL SourceVoiceImpl::DestroyVoice() {
    RecordOk("IXAudio2SourceVoice::DestroyVoice", 6);
    if (engine_) {
        engine_->RemoveSourceVoice(this);
    }
}

// ===========================================================================
// ISpatialAudioClient / ISpatialAudioObjectRenderStream / ISpatialAudioObject
//   Thread-safe & re-entrancy-hardened under heavy concurrent streaming load!
// ===========================================================================
constexpr int32_t OS_SPTLAUDCLNT_E_DESTROYED = static_cast<int32_t>(0x88890100u);
constexpr int32_t OS_SPTLAUDCLNT_E_OUT_OF_ORDER = static_cast<int32_t>(0x88890101u);
constexpr int32_t OS_SPTLAUDCLNT_E_RESOURCES_INVALIDATED = static_cast<int32_t>(0x88890102u);
constexpr int32_t OS_SPTLAUDCLNT_E_NO_MORE_OBJECTS = static_cast<int32_t>(0x88890103u);

constexpr XAudio2Guid IID_OS_ISpatialAudioClient = {
    0xBBF8E066u, 0xAAAAu, 0x49BEu, {0x9Au, 0x4Du, 0xFDu, 0x2Au, 0x85u, 0x8Eu, 0xAEu, 0x29u}};
constexpr XAudio2Guid IID_OS_ISpatialAudioObjectRenderStream = {
    0xBAB5F473u, 0xB423u, 0x477Bu, {0x85u, 0xF5u, 0xB5u, 0xA3u, 0x32u, 0xA0u, 0x41u, 0x53u}};
constexpr XAudio2Guid IID_OS_ISpatialAudioObject = {
    0xDDE28967u, 0x521Bu, 0x46E5u, {0x8Fu, 0x00u, 0xBDu, 0x6Fu, 0x2Bu, 0xC8u, 0xABu, 0x1Du}};

struct OS_SpatialAudioObjectRenderStreamActivationParams {
    const OS_WAVEFORMATEX* ObjectFormat;
    uint32_t StaticObjectTypeMask;
    uint32_t MinDynamicObjectCount;
    uint32_t MaxDynamicObjectCount;
    uint32_t Category;
    void* EventHandle;
    void* NotifyObject;
};

struct OS_ISpatialAudioObject : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetBuffer(uint8_t** buffer, uint32_t* bufferLength) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetEndOfStream(uint32_t frameCount) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL IsActive(int32_t* isActive) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetAudioObjectType(uint32_t* audioObjectType) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetPosition(float x, float y, float z) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float volume) = 0;
};

struct OS_ISpatialAudioObjectRenderStream : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetAvailableDynamicObjectCount(uint32_t* value) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Start() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Stop() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Reset() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL BeginUpdatingAudioObjects(
        uint32_t* availableDynamicObjectCount,
        uint32_t* frameCountPerBuffer) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioObject(
        uint32_t type,
        OS_ISpatialAudioObject** audioObject) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL EndUpdatingAudioObjects() = 0;
};

struct OS_ISpatialAudioClient : public OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetStaticObjectPosition(
        uint32_t type, float* x, float* y, float* z) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetNativeStaticObjectTypeMask(uint32_t* mask) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxDynamicObjectCount(uint32_t* value) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL IsAudioObjectFormatSupported(
        const OS_WAVEFORMATEX* objectFormat) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxFrameCount(
        const OS_WAVEFORMATEX* objectFormat, uint32_t* frameCountPerBuffer) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioStream(
        const OS_SpatialAudioObjectRenderStreamActivationParams* activationParams,
        const XAudio2Guid* riid,
        void** stream) = 0;
};

class SpatialAudioObjectRenderStreamImpl;

class SpatialAudioObjectImpl final : public OS_ISpatialAudioObject {
public:
    SpatialAudioObjectImpl(SpatialAudioObjectRenderStreamImpl* owner,
                           uint32_t object_type,
                           uint32_t buffer_bytes)
        : owner_(owner),
          object_type_(object_type),
          buffer_(std::max<uint32_t>(4u, buffer_bytes), 0u) {}

    ~SpatialAudioObjectImpl() = default;

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) return OS_XAUDIO2_E_POINTER;
        *ppvObject = nullptr;
        if (!riid) return OS_XAUDIO2_E_INVALIDARG;
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) || GuidEquals(riid, IID_OS_ISpatialAudioObject)) {
            AddRef();
            *ppvObject = static_cast<OS_ISpatialAudioObject*>(this);
            return OS_XAUDIO2_S_OK;
        }
        return OS_XAUDIO2_E_NOINTERFACE;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            NotifyOwnerReleased();
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetBuffer(uint8_t** buffer, uint32_t* bufferLength) override {
        if (!buffer || !bufferLength) return OS_XAUDIO2_E_POINTER;
        if (!active_.load(std::memory_order_acquire) || invalidated_.load(std::memory_order_acquire)) {
            *buffer = nullptr;
            *bufferLength = 0u;
            return OS_SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
        }
        std::lock_guard<std::mutex> lk(obj_mutex_);
        *buffer = buffer_.data();
        *bufferLength = static_cast<uint32_t>(buffer_.size());
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetEndOfStream(uint32_t /*frameCount*/) override {
        if (!active_.exchange(false, std::memory_order_acq_rel)) {
            return OS_SPTLAUDCLNT_E_OUT_OF_ORDER;
        }
        NotifyOwnerDeactivated();
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL IsActive(int32_t* isActive) override {
        if (!isActive) return OS_XAUDIO2_E_POINTER;
        *isActive = (active_.load(std::memory_order_acquire) &&
                     !invalidated_.load(std::memory_order_acquire))
                        ? 1
                        : 0;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetAudioObjectType(uint32_t* audioObjectType) override {
        if (!audioObjectType) return OS_XAUDIO2_E_POINTER;
        *audioObjectType = object_type_;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetPosition(float x, float y, float z) override {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            return OS_XAUDIO2_E_INVALIDARG;
        }
        if (invalidated_.load(std::memory_order_acquire)) {
            return OS_SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
        }
        pos_x_.store(x, std::memory_order_relaxed);
        pos_y_.store(y, std::memory_order_relaxed);
        pos_z_.store(z, std::memory_order_relaxed);
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(float volume) override {
        if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
            return OS_XAUDIO2_E_INVALIDARG;
        }
        if (invalidated_.load(std::memory_order_acquire)) {
            return OS_SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
        }
        volume_.store(volume, std::memory_order_relaxed);
        return OS_XAUDIO2_S_OK;
    }

    void InvalidateFromStream() noexcept {
        invalidated_.store(true, std::memory_order_release);
        active_.store(false, std::memory_order_release);
    }

private:
    void NotifyOwnerDeactivated();
    void NotifyOwnerReleased();

    std::atomic<uint32_t> ref_count_{1u};
    SpatialAudioObjectRenderStreamImpl* owner_ = nullptr;
    uint32_t object_type_ = 0u;
    std::atomic<bool> active_{true};
    std::atomic<bool> invalidated_{false};
    std::atomic<bool> owner_notified_deactivated_{false};
    std::atomic<float> pos_x_{0.0f};
    std::atomic<float> pos_y_{0.0f};
    std::atomic<float> pos_z_{0.0f};
    std::atomic<float> volume_{1.0f};
    std::mutex obj_mutex_;
    std::vector<uint8_t> buffer_;
};

class SpatialAudioObjectRenderStreamImpl final : public OS_ISpatialAudioObjectRenderStream {
public:
    SpatialAudioObjectRenderStreamImpl(uint32_t max_dynamic_objects, uint32_t frame_count)
        : max_dynamic_objects_(std::clamp<uint32_t>(max_dynamic_objects, 1u, 128u)),
          frame_count_(std::max<uint32_t>(64u, frame_count)) {}

    ~SpatialAudioObjectRenderStreamImpl() {
        destroyed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(stream_mutex_);
        for (auto* obj : tracked_objects_) {
            if (obj) obj->InvalidateFromStream();
        }
        tracked_objects_.clear();
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) return OS_XAUDIO2_E_POINTER;
        *ppvObject = nullptr;
        if (!riid) return OS_XAUDIO2_E_INVALIDARG;
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) ||
            GuidEquals(riid, IID_OS_ISpatialAudioObjectRenderStream)) {
            AddRef();
            *ppvObject = static_cast<OS_ISpatialAudioObjectRenderStream*>(this);
            return OS_XAUDIO2_S_OK;
        }
        return OS_XAUDIO2_E_NOINTERFACE;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetAvailableDynamicObjectCount(uint32_t* value) override {
        if (!value) return OS_XAUDIO2_E_POINTER;
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        const uint32_t active = active_dynamic_count_.load(std::memory_order_acquire);
        *value = (active < max_dynamic_objects_) ? (max_dynamic_objects_ - active) : 0u;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Start() override {
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        running_.store(true, std::memory_order_release);
        return RecordHr("ISpatialAudioObjectRenderStream::Start", OS_XAUDIO2_S_OK);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Stop() override {
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        running_.store(false, std::memory_order_release);
        return RecordHr("ISpatialAudioObjectRenderStream::Stop", OS_XAUDIO2_S_OK);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL Reset() override {
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        if (running_.load(std::memory_order_acquire)) {
            return OS_SPTLAUDCLNT_E_OUT_OF_ORDER;
        }
        std::lock_guard<std::mutex> lk(stream_mutex_);
        for (auto* obj : tracked_objects_) {
            if (obj) obj->InvalidateFromStream();
        }
        tracked_objects_.clear();
        active_dynamic_count_.store(0u, std::memory_order_release);
        updating_.store(false, std::memory_order_release);
        return RecordHr("ISpatialAudioObjectRenderStream::Reset", OS_XAUDIO2_S_OK);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL BeginUpdatingAudioObjects(
        uint32_t* availableDynamicObjectCount,
        uint32_t* frameCountPerBuffer) override {
        if (!availableDynamicObjectCount || !frameCountPerBuffer) {
            return OS_XAUDIO2_E_POINTER;
        }
        if (destroyed_.load(std::memory_order_acquire)) {
            return OS_SPTLAUDCLNT_E_DESTROYED;
        }
        // Strict re-entrancy & concurrent-pass guard: reject nested/overlapping Begin calls!
        bool expected = false;
        if (!updating_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return RecordHr("ISpatialAudioObjectRenderStream::BeginUpdatingAudioObjects",
                            OS_SPTLAUDCLNT_E_OUT_OF_ORDER);
        }
        const uint32_t active = active_dynamic_count_.load(std::memory_order_acquire);
        *availableDynamicObjectCount =
            (active < max_dynamic_objects_) ? (max_dynamic_objects_ - active) : 0u;
        *frameCountPerBuffer = frame_count_;
        return RecordHr("ISpatialAudioObjectRenderStream::BeginUpdatingAudioObjects", OS_XAUDIO2_S_OK);
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioObject(
        uint32_t type,
        OS_ISpatialAudioObject** audioObject) override {
        if (!audioObject) return OS_XAUDIO2_E_POINTER;
        *audioObject = nullptr;
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        if (!updating_.load(std::memory_order_acquire)) {
            return OS_SPTLAUDCLNT_E_OUT_OF_ORDER;
        }
        // Dynamic object type == 0x00000001u (AudioObjectType_Dynamic)
        const bool is_dynamic = (type == 0x00000001u);
        if (is_dynamic) {
            uint32_t cur = active_dynamic_count_.load(std::memory_order_acquire);
            for (;;) {
                if (cur >= max_dynamic_objects_) {
                    return OS_SPTLAUDCLNT_E_NO_MORE_OBJECTS;
                }
                if (active_dynamic_count_.compare_exchange_weak(
                        cur, cur + 1u, std::memory_order_acq_rel)) {
                    break;
                }
            }
        }
        auto* obj = new (std::nothrow) SpatialAudioObjectImpl(this, type, frame_count_ * sizeof(float));
        if (!obj) {
            if (is_dynamic) {
                active_dynamic_count_.fetch_sub(1u, std::memory_order_acq_rel);
            }
            return OS_XAUDIO2_E_OUTOFMEMORY;
        }
        AddRef();  // Keep stream alive until all activated objects release!
        {
            std::lock_guard<std::mutex> lk(stream_mutex_);
            tracked_objects_.push_back(obj);
        }
        *audioObject = obj;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL EndUpdatingAudioObjects() override {
        if (destroyed_.load(std::memory_order_acquire)) return OS_SPTLAUDCLNT_E_DESTROYED;
        bool expected = true;
        if (!updating_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return RecordHr("ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects",
                            OS_SPTLAUDCLNT_E_OUT_OF_ORDER);
        }
        return RecordHr("ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects", OS_XAUDIO2_S_OK);
    }

    void OnObjectDeactivated(uint32_t object_type) {
        if (object_type == 0x00000001u) {
            uint32_t cur = active_dynamic_count_.load(std::memory_order_acquire);
            while (cur > 0u &&
                   !active_dynamic_count_.compare_exchange_weak(
                       cur, cur - 1u, std::memory_order_acq_rel)) {
            }
        }
    }

    void OnObjectReleased(SpatialAudioObjectImpl* obj, uint32_t object_type, bool already_deactivated) {
        if (!already_deactivated) {
            OnObjectDeactivated(object_type);
        }
        {
            std::lock_guard<std::mutex> lk(stream_mutex_);
            tracked_objects_.erase(
                std::remove(tracked_objects_.begin(), tracked_objects_.end(), obj),
                tracked_objects_.end());
        }
        Release();
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    uint32_t max_dynamic_objects_ = 128u;
    uint32_t frame_count_ = 480u;
    std::atomic<bool> running_{false};
    std::atomic<bool> updating_{false};
    std::atomic<bool> destroyed_{false};
    std::atomic<uint32_t> active_dynamic_count_{0u};
    std::mutex stream_mutex_;
    std::vector<SpatialAudioObjectImpl*> tracked_objects_;
};

void SpatialAudioObjectImpl::NotifyOwnerDeactivated() {
    if (!owner_notified_deactivated_.exchange(true, std::memory_order_acq_rel) && owner_) {
        owner_->OnObjectDeactivated(object_type_);
    }
}

void SpatialAudioObjectImpl::NotifyOwnerReleased() {
    if (owner_) {
        const bool already_deact = owner_notified_deactivated_.exchange(true, std::memory_order_acq_rel);
        owner_->OnObjectReleased(this, object_type_, already_deact);
        owner_ = nullptr;
    }
}

class SpatialAudioClientImpl final : public OS_ISpatialAudioClient {
public:
    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("ISpatialAudioClient::QueryInterface", OS_XAUDIO2_E_POINTER);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("ISpatialAudioClient::QueryInterface", OS_XAUDIO2_E_INVALIDARG);
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) ||
            GuidEquals(riid, IID_OS_ISpatialAudioClient)) {
            AddRef();
            *ppvObject = static_cast<OS_ISpatialAudioClient*>(this);
            return RecordHr("ISpatialAudioClient::QueryInterface", OS_XAUDIO2_S_OK);
        }
        return RecordHr("ISpatialAudioClient::QueryInterface", OS_XAUDIO2_E_NOINTERFACE);
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetStaticObjectPosition(
        uint32_t type, float* x, float* y, float* z) override {
        if (!x || !y || !z) return OS_XAUDIO2_E_POINTER;
        *x = 0.0f;
        *y = 0.0f;
        *z = (type == 0x00000002u) ? 1.0f : -1.0f;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetNativeStaticObjectTypeMask(uint32_t* mask) override {
        if (!mask) return OS_XAUDIO2_E_POINTER;
        *mask = 0x0000003Fu;  // 5.1 bed
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxDynamicObjectCount(uint32_t* value) override {
        if (!value) return OS_XAUDIO2_E_POINTER;
        *value = 128u;
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL IsAudioObjectFormatSupported(
        const OS_WAVEFORMATEX* objectFormat) override {
        if (!objectFormat) return OS_XAUDIO2_E_POINTER;
        if (objectFormat->nChannels != 1u || objectFormat->nSamplesPerSec == 0u) {
            return OS_XAUDIO2_E_INVALIDARG;
        }
        if (objectFormat->wFormatTag == OS_WAVE_FORMAT_IEEE_FLOAT &&
            objectFormat->wBitsPerSample == 32u) {
            return OS_XAUDIO2_S_OK;
        }
        if (objectFormat->wFormatTag == OS_WAVE_FORMAT_PCM &&
            objectFormat->wBitsPerSample == 16u) {
            return OS_XAUDIO2_S_OK;
        }
        return OS_XAUDIO2_E_INVALIDARG;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL GetMaxFrameCount(
        const OS_WAVEFORMATEX* objectFormat, uint32_t* frameCountPerBuffer) override {
        if (!objectFormat || !frameCountPerBuffer) return OS_XAUDIO2_E_POINTER;
        if (IsAudioObjectFormatSupported(objectFormat) < 0) return OS_XAUDIO2_E_INVALIDARG;
        *frameCountPerBuffer = std::max<uint32_t>(64u, objectFormat->nSamplesPerSec / 100u);
        return OS_XAUDIO2_S_OK;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL ActivateSpatialAudioStream(
        const OS_SpatialAudioObjectRenderStreamActivationParams* activationParams,
        const XAudio2Guid* riid,
        void** stream) override {
        if (!stream) return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", OS_XAUDIO2_E_POINTER);
        *stream = nullptr;
        if (!activationParams || !activationParams->ObjectFormat || !riid) {
            return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", OS_XAUDIO2_E_INVALIDARG);
        }
        if (!GuidEquals(riid, IID_OS_ISpatialAudioObjectRenderStream) &&
            !GuidEquals(riid, IID_OS_XAudio2_IUnknown)) {
            return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", OS_XAUDIO2_E_NOINTERFACE);
        }
        uint32_t max_frames = 480u;
        const int32_t fmt_hr = GetMaxFrameCount(activationParams->ObjectFormat, &max_frames);
        if (fmt_hr < 0) {
            return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", fmt_hr);
        }
        const uint32_t dyn_cnt = activationParams->MaxDynamicObjectCount > 0u
                                     ? activationParams->MaxDynamicObjectCount
                                     : 128u;
        auto* s = new (std::nothrow) SpatialAudioObjectRenderStreamImpl(dyn_cnt, max_frames);
        if (!s) {
            return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", OS_XAUDIO2_E_OUTOFMEMORY);
        }
        *stream = static_cast<OS_ISpatialAudioObjectRenderStream*>(s);
        return RecordHr("ISpatialAudioClient::ActivateSpatialAudioStream", OS_XAUDIO2_S_OK);
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
};

// ===========================================================================
// X3DAudio 3D Spatialization Engine
// ===========================================================================
constexpr uint32_t kX3dAudioMagic = 0x58334441u;  // "X3DA"

struct X3dAudioInstanceState {
    uint32_t magic;
    uint32_t speaker_channel_mask;
    uint32_t channel_count;
    float speed_of_sound;
    uint32_t reserved;
};
static_assert(sizeof(X3dAudioInstanceState) == OS_X3DAUDIO_HANDLE_BYTESIZE,
              "X3dAudioInstanceState must fit exactly in 20-byte X3DAUDIO_HANDLE");

uint32_t Popcount32(uint32_t v) noexcept {
    uint32_t c = 0;
    while (v != 0u) {
        v &= (v - 1u);
        ++c;
    }
    return c;
}

float VecDot(const X3DAUDIO_VECTOR& a, const X3DAUDIO_VECTOR& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

X3DAUDIO_VECTOR VecSub(const X3DAUDIO_VECTOR& a, const X3DAUDIO_VECTOR& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

X3DAUDIO_VECTOR VecCross(const X3DAUDIO_VECTOR& a, const X3DAUDIO_VECTOR& b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float VecLength(const X3DAUDIO_VECTOR& v) noexcept {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

X3DAUDIO_VECTOR VecNormalizeOrDefault(const X3DAUDIO_VECTOR& v,
                                      const X3DAUDIO_VECTOR& fallback) noexcept {
    const float len = VecLength(v);
    if (len <= 1e-6f || !std::isfinite(len)) {
        return fallback;
    }
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

float EvalDistanceCurve(const X3DAUDIO_DISTANCE_CURVE* curve, float norm_dist, float fallback) noexcept {
    if (!curve || !curve->pPoints || curve->PointCount == 0u) {
        return fallback;
    }
    if (norm_dist <= curve->pPoints[0].Distance) {
        return curve->pPoints[0].DSPSetting;
    }
    const uint32_t last = curve->PointCount - 1u;
    if (norm_dist >= curve->pPoints[last].Distance) {
        return curve->pPoints[last].DSPSetting;
    }
    for (uint32_t i = 0; i < last; ++i) {
        const auto& p0 = curve->pPoints[i];
        const auto& p1 = curve->pPoints[i + 1u];
        if (norm_dist >= p0.Distance && norm_dist <= p1.Distance) {
            const float span = p1.Distance - p0.Distance;
            if (span <= 1e-6f) {
                return p1.DSPSetting;
            }
            const float t = (norm_dist - p0.Distance) / span;
            return p0.DSPSetting + t * (p1.DSPSetting - p0.DSPSetting);
        }
    }
    return curve->pPoints[last].DSPSetting;
}

float WrapAnglePi(float a) noexcept {
    constexpr float kTwoPi = 2.0f * OS_X3DAUDIO_PI;
    while (a > OS_X3DAUDIO_PI) a -= kTwoPi;
    while (a <= -OS_X3DAUDIO_PI) a += kTwoPi;
    return a;
}

struct SpeakerSlot {
    uint32_t bit = 0;
    uint32_t dst_index = 0;
    float azimuth = 0.0f;
    bool is_lfe = false;
    bool is_center = false;
};

std::vector<SpeakerSlot> BuildSpeakerLayout(uint32_t mask, uint32_t dst_channels) {
    struct KnownSpeaker {
        uint32_t bit;
        float azimuth;
        bool is_lfe;
        bool is_center;
    };
    static constexpr KnownSpeaker kKnown[] = {
        {OS_SPEAKER_FRONT_LEFT, -OS_X3DAUDIO_PI * 0.25f, false, false},
        {OS_SPEAKER_FRONT_RIGHT, OS_X3DAUDIO_PI * 0.25f, false, false},
        {OS_SPEAKER_FRONT_CENTER, 0.0f, false, true},
        {OS_SPEAKER_LOW_FREQUENCY, 0.0f, true, false},
        {OS_SPEAKER_BACK_LEFT, -OS_X3DAUDIO_PI * 0.75f, false, false},
        {OS_SPEAKER_BACK_RIGHT, OS_X3DAUDIO_PI * 0.75f, false, false},
        {0x00000040u, -OS_X3DAUDIO_PI * 0.125f, false, false},  // FRONT_LEFT_OF_CENTER
        {0x00000080u, OS_X3DAUDIO_PI * 0.125f, false, false},   // FRONT_RIGHT_OF_CENTER
        {0x00000100u, OS_X3DAUDIO_PI, false, false},            // BACK_CENTER
        {OS_SPEAKER_SIDE_LEFT, -OS_X3DAUDIO_PI * 0.5f, false, false},
        {OS_SPEAKER_SIDE_RIGHT, OS_X3DAUDIO_PI * 0.5f, false, false},
    };

    std::vector<SpeakerSlot> out;
    uint32_t idx = 0;
    for (const auto& sp : kKnown) {
        if ((mask & sp.bit) != 0u && idx < dst_channels) {
            out.push_back(SpeakerSlot{sp.bit, idx, sp.azimuth, sp.is_lfe, sp.is_center});
            ++idx;
        }
    }
    // Fill any remaining channels generically if caller passed a custom mask/count.
    while (idx < dst_channels) {
        const float az = -OS_X3DAUDIO_PI + (2.0f * OS_X3DAUDIO_PI * static_cast<float>(idx)) /
                                               static_cast<float>(std::max(1u, dst_channels));
        out.push_back(SpeakerSlot{0u, idx, az, false, false});
        ++idx;
    }
    return out;
}

// ===========================================================================
// XAudio2FX COM APOs (Reverb & VolumeMeter)
// ===========================================================================
class ReverbApoImpl final : public OS_IXAPOParameters {
public:
    explicit ReverbApoImpl(uint32_t flags) : flags_(flags) {
        // Initialize standard XAudio2FX reverb defaults.
        params_.WetDryMix = 100.0f;
        params_.ReflectionsDelay = 5u;
        params_.ReverbDelay = 5u;
        params_.RearDelay = 5u;
        params_.SideDelay = 5u;
        params_.PositionLeft = 6u;
        params_.PositionRight = 6u;
        params_.PositionMatrixLeft = 27u;
        params_.PositionMatrixRight = 27u;
        params_.EarlyDiffusion = 8u;
        params_.LateDiffusion = 8u;
        params_.LowEQGain = 8u;
        params_.LowEQCutoff = 4u;
        params_.HighEQGain = 8u;
        params_.HighEQCutoff = 4u;
        params_.RoomFilterFreq = 5000.0f;
        params_.RoomFilterMain = 0.0f;
        params_.RoomFilterHF = 0.0f;
        params_.ReflectionsGain = 0.0f;
        params_.ReverbGain = 0.0f;
        params_.DecayTime = 1.0f;
        params_.Density = 100.0f;
        params_.RoomSize = 100.0f;
        params_.DisableLateField = 0;
    }

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("XAudio2FXReverb::QueryInterface", OS_XAUDIO2_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("XAudio2FXReverb::QueryInterface", OS_XAUDIO2_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) ||
            GuidEquals(riid, IID_OS_IXAPO) ||
            GuidEquals(riid, IID_OS_IXAPOParameters)) {
            AddRef();
            *ppvObject = static_cast<OS_IXAPOParameters*>(this);
            return RecordHr("XAudio2FXReverb::QueryInterface", OS_XAUDIO2_S_OK, 0);
        }
        return RecordHr("XAudio2FXReverb::QueryInterface", OS_XAUDIO2_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        RecordOk("XAudio2FXReverb::AddRef", 1);
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        RecordOk("XAudio2FXReverb::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_XAUDIO2_CALL SetParameters(
        const void* pParameters, uint32_t ParameterByteSize) override {
        RecordOk("XAudio2FXReverb::SetParameters", 3);
        if (!pParameters || ParameterByteSize == 0u) {
            return;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t copy_bytes = std::min<size_t>(sizeof(params_), ParameterByteSize);
        std::memcpy(&params_, pParameters, copy_bytes);
    }

    void OPENSCARLETT_XAUDIO2_CALL GetParameters(
        void* pParameters, uint32_t ParameterByteSize) override {
        RecordOk("XAudio2FXReverb::GetParameters", 4);
        if (!pParameters || ParameterByteSize == 0u) {
            return;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t copy_bytes = std::min<size_t>(sizeof(params_), ParameterByteSize);
        std::memcpy(pParameters, &params_, copy_bytes);
    }

    void ProcessAudio(
        float* interleaved,
        uint32_t frames,
        uint32_t channels,
        uint32_t sample_rate) {
        if (!interleaved || frames == 0u || channels == 0u) return;
        OS_XAUDIO2FX_REVERB_PARAMETERS p{};
        {
            std::lock_guard<std::mutex> lk(mutex_);
            p = params_;
            const uint32_t sr = std::max<uint32_t>(8000u, sample_rate);
            const size_t base_delay = std::clamp<size_t>(
                static_cast<size_t>((sr * std::max<uint32_t>(5u, p.ReflectionsDelay + p.ReverbDelay)) / 1000u),
                64u, 4800u);
            if (comb_len_[0] != base_delay) {
                comb_len_[0] = base_delay;
                comb_len_[1] = (base_delay * 117u) / 100u + 13u;
                comb_len_[2] = (base_delay * 139u) / 100u + 29u;
                comb_len_[3] = (base_delay * 161u) / 100u + 41u;
                for (int k = 0; k < 4; ++k) {
                    comb_buf_[k].assign(comb_len_[k], 0.0f);
                    comb_pos_[k] = 0u;
                }
            }
            const float wet = std::clamp(p.WetDryMix / 100.0f, 0.0f, 1.0f);
            const float dry = 1.0f - wet * 0.5f;
            const float decay = std::clamp(0.35f + p.DecayTime * 0.25f, 0.2f, 0.85f);
            const float rev_gain = std::pow(10.0f, std::clamp(p.ReverbGain, -24.0f, 6.0f) / 20.0f);
            for (uint32_t f = 0u; f < frames; ++f) {
                float mono_in = 0.0f;
                for (uint32_t c = 0u; c < channels; ++c) {
                    mono_in += interleaved[static_cast<size_t>(f) * channels + c];
                }
                mono_in /= static_cast<float>(channels);

                float wet_l = 0.0f;
                float wet_r = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const size_t idx = comb_pos_[k];
                    const float delayed = comb_buf_[k][idx];
                    comb_buf_[k][idx] = mono_in + delayed * decay;
                    comb_pos_[k] = (idx + 1u) % comb_len_[k];
                    if ((k & 1) == 0) {
                        wet_l += delayed * 0.5f;
                    } else {
                        wet_r += delayed * 0.5f;
                    }
                }
                for (uint32_t c = 0u; c < channels; ++c) {
                    const float wet_s = ((c & 1u) == 0u ? wet_l : wet_r) * rev_gain;
                    float& dst = interleaved[static_cast<size_t>(f) * channels + c];
                    dst = dst * dry + wet_s * wet;
                }
            }
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    uint32_t flags_ = 0u;
    std::mutex mutex_;
    OS_XAUDIO2FX_REVERB_PARAMETERS params_{};
    std::vector<float> comb_buf_[4];
    size_t comb_len_[4] = {0u, 0u, 0u, 0u};
    size_t comb_pos_[4] = {0u, 0u, 0u, 0u};
};

class VolumeMeterApoImpl final : public OS_IXAPOParameters {
public:
    explicit VolumeMeterApoImpl(uint32_t flags)
        : flags_(flags), peak_levels_(8u, 0.0f), rms_levels_(8u, 0.0f) {}

    int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) override {
        if (!ppvObject) {
            return RecordHr("XAudio2FXVolumeMeter::QueryInterface", OS_XAUDIO2_E_POINTER, 0);
        }
        *ppvObject = nullptr;
        if (!riid) {
            return RecordHr("XAudio2FXVolumeMeter::QueryInterface", OS_XAUDIO2_E_INVALIDARG, 0);
        }
        if (GuidEquals(riid, IID_OS_XAudio2_IUnknown) ||
            GuidEquals(riid, IID_OS_IXAPO) ||
            GuidEquals(riid, IID_OS_IXAPOParameters)) {
            AddRef();
            *ppvObject = static_cast<OS_IXAPOParameters*>(this);
            return RecordHr("XAudio2FXVolumeMeter::QueryInterface", OS_XAUDIO2_S_OK, 0);
        }
        return RecordHr("XAudio2FXVolumeMeter::QueryInterface", OS_XAUDIO2_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() override {
        RecordOk("XAudio2FXVolumeMeter::AddRef", 1);
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    uint32_t OPENSCARLETT_XAUDIO2_CALL Release() override {
        RecordOk("XAudio2FXVolumeMeter::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    void OPENSCARLETT_XAUDIO2_CALL SetParameters(
        const void* pParameters, uint32_t ParameterByteSize) override {
        RecordOk("XAudio2FXVolumeMeter::SetParameters", 3);
        if (!pParameters || ParameterByteSize < sizeof(OS_XAUDIO2FX_VOLUMEMETER_LEVELS)) {
            return;
        }
        const auto* levels = static_cast<const OS_XAUDIO2FX_VOLUMEMETER_LEVELS*>(pParameters);
        std::lock_guard<std::mutex> lk(mutex_);
        const uint32_t ch = std::min<uint32_t>(levels->ChannelCount, 64u);
        if (ch > 0u) {
            peak_levels_.resize(ch, 0.0f);
            rms_levels_.resize(ch, 0.0f);
            if (levels->pPeakLevels) {
                std::memcpy(peak_levels_.data(), levels->pPeakLevels, ch * sizeof(float));
            }
            if (levels->pRMSLevels) {
                std::memcpy(rms_levels_.data(), levels->pRMSLevels, ch * sizeof(float));
            }
        }
    }

    void OPENSCARLETT_XAUDIO2_CALL GetParameters(
        void* pParameters, uint32_t ParameterByteSize) override {
        RecordOk("XAudio2FXVolumeMeter::GetParameters", 4);
        if (!pParameters || ParameterByteSize < sizeof(OS_XAUDIO2FX_VOLUMEMETER_LEVELS)) {
            return;
        }
        auto* levels = static_cast<OS_XAUDIO2FX_VOLUMEMETER_LEVELS*>(pParameters);
        std::lock_guard<std::mutex> lk(mutex_);
        for (uint32_t i = 0; i < levels->ChannelCount; ++i) {
            const float peak = (i < peak_levels_.size()) ? peak_levels_[i] : 0.0f;
            const float rms = (i < rms_levels_.size()) ? rms_levels_[i] : 0.0f;
            if (levels->pPeakLevels) levels->pPeakLevels[i] = peak;
            if (levels->pRMSLevels) levels->pRMSLevels[i] = rms;
        }
    }

    void ProcessAudio(
        const float* interleaved,
        uint32_t frames,
        uint32_t channels) {
        if (!interleaved || frames == 0u || channels == 0u) return;
        const uint32_t ch = std::min<uint32_t>(channels, 64u);
        std::vector<float> peaks(ch, 0.0f);
        std::vector<double> sum_sq(ch, 0.0);
        for (uint32_t f = 0u; f < frames; ++f) {
            for (uint32_t c = 0u; c < ch; ++c) {
                const float s = interleaved[static_cast<size_t>(f) * channels + c];
                const float a = std::fabs(s);
                if (a > peaks[c]) peaks[c] = a;
                sum_sq[c] += static_cast<double>(s) * static_cast<double>(s);
            }
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (peak_levels_.size() < ch) peak_levels_.resize(ch, 0.0f);
        if (rms_levels_.size() < ch) rms_levels_.resize(ch, 0.0f);
        for (uint32_t c = 0u; c < ch; ++c) {
            peak_levels_[c] = peaks[c];
            rms_levels_[c] = static_cast<float>(std::sqrt(sum_sq[c] / static_cast<double>(frames)));
        }
    }

private:
    std::atomic<uint32_t> ref_count_{1u};
    uint32_t flags_ = 0u;
    std::mutex mutex_;
    std::vector<float> peak_levels_;
    std::vector<float> rms_levels_;
};

void ProcessEffectApo(
    OS_IXAudio2Unknown* effect,
    float* interleaved,
    uint32_t frames,
    uint32_t channels,
    uint32_t sample_rate) {
    if (!effect || !interleaved || frames == 0u || channels == 0u) return;
    if (auto* rev = dynamic_cast<ReverbApoImpl*>(effect)) {
        rev->ProcessAudio(interleaved, frames, channels, sample_rate);
    } else if (auto* vm = dynamic_cast<VolumeMeterApoImpl*>(effect)) {
        vm->ProcessAudio(interleaved, frames, channels);
    }
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2Create(
    IXAudio2** ppXAudio2,
    uint32_t /*Flags*/,
    uint32_t /*XAudio2Processor*/) {
    if (!ppXAudio2) {
        return RecordHr("XAudio2Create", OS_XAUDIO2_E_POINTER);
    }
    *ppXAudio2 = nullptr;
    auto* engine = new (std::nothrow) XAudio2Impl();
    if (!engine) {
        return RecordHr("XAudio2Create", OS_XAUDIO2_E_OUTOFMEMORY);
    }
    *ppXAudio2 = engine;
    return RecordHr("XAudio2Create", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    uint32_t Flags,
    uint32_t XAudio2Processor,
    uint32_t /*ntddiVersion*/) {
    const int32_t hr = XAudio2Create(ppXAudio2, Flags, XAudio2Processor);
    return RecordHr("XAudio2CreateWithVersionInfo", hr);
}

int32_t OPENSCARLETT_XAUDIO2_CALL X3DAudioInitialize(
    uint32_t SpeakerChannelMask,
    float SpeedOfSound,
    X3DAUDIO_HANDLE Instance) {
    if (!Instance || SpeakerChannelMask == 0u ||
        !(SpeedOfSound > 0.0f) || !std::isfinite(SpeedOfSound)) {
        return RecordHr("X3DAudioInitialize", OS_XAUDIO2_E_INVALIDARG);
    }
    const uint32_t ch_count = Popcount32(SpeakerChannelMask);
    if (ch_count == 0u) {
        return RecordHr("X3DAudioInitialize", OS_XAUDIO2_E_INVALIDARG);
    }
    X3dAudioInstanceState state{};
    state.magic = kX3dAudioMagic;
    state.speaker_channel_mask = SpeakerChannelMask;
    state.channel_count = ch_count;
    state.speed_of_sound = SpeedOfSound;
    state.reserved = 0u;
    std::memcpy(Instance, &state, sizeof(state));
    return RecordHr("X3DAudioInitialize", OS_XAUDIO2_S_OK);
}

void OPENSCARLETT_XAUDIO2_CALL X3DAudioCalculate(
    const X3DAUDIO_HANDLE Instance,
    const X3DAUDIO_LISTENER* pListener,
    const X3DAUDIO_EMITTER* pEmitter,
    uint32_t Flags,
    X3DAUDIO_DSP_SETTINGS* pDSPSettings) {
    RecordOk("X3DAudioCalculate");
    if (!Instance || !pListener || !pEmitter || !pDSPSettings) {
        return;
    }

    X3dAudioInstanceState state{};
    std::memcpy(&state, Instance, sizeof(state));
    if (state.magic != kX3dAudioMagic || state.speed_of_sound <= 0.0f) {
        state.speaker_channel_mask = OS_SPEAKER_STEREO;
        state.channel_count = 2u;
        state.speed_of_sound = OS_X3DAUDIO_SPEED_OF_SOUND;
    }

    // Build orthonormal left-handed listener basis (DirectX convention: +X right, +Y up, +Z front).
    const X3DAUDIO_VECTOR front = VecNormalizeOrDefault(pListener->OrientFront, {0.0f, 0.0f, 1.0f});
    const X3DAUDIO_VECTOR up_hint = VecNormalizeOrDefault(pListener->OrientTop, {0.0f, 1.0f, 0.0f});
    const X3DAUDIO_VECTOR right = VecNormalizeOrDefault(VecCross(up_hint, front), {1.0f, 0.0f, 0.0f});
    const X3DAUDIO_VECTOR up = VecNormalizeOrDefault(VecCross(front, right), {0.0f, 1.0f, 0.0f});

    // Vector from listener to emitter.
    const X3DAUDIO_VECTOR l2e = VecSub(pEmitter->Position, pListener->Position);
    const float distance = VecLength(l2e);
    pDSPSettings->EmitterToListenerDistance = distance;

    const X3DAUDIO_VECTOR dir_l2e =
        (distance > 1e-6f) ? X3DAUDIO_VECTOR{l2e.x / distance, l2e.y / distance, l2e.z / distance}
                           : front;
    const X3DAUDIO_VECTOR dir_e2l = {-dir_l2e.x, -dir_l2e.y, -dir_l2e.z};

    const float local_x = VecDot(dir_l2e, right);
    const float local_y = VecDot(dir_l2e, up);
    const float local_z = VecDot(dir_l2e, front);
    (void)local_y;

    const float curve_scaler =
        (pEmitter->CurveDistanceScaler > 0.0f && std::isfinite(pEmitter->CurveDistanceScaler))
            ? pEmitter->CurveDistanceScaler
            : 1.0f;
    const float norm_dist = distance / curve_scaler;

    // Default inverse-distance attenuation falloff: 1.0 within 1 unit, 1 / norm_dist beyond.
    const float default_vol = (norm_dist <= 1.0f) ? 1.0f : (1.0f / norm_dist);
    float volume = std::clamp(EvalDistanceCurve(pEmitter->pVolumeCurve, norm_dist, default_vol), 0.0f, 1.0f);

    // Optional emitter cone attenuation & angle.
    const X3DAUDIO_VECTOR emitter_front = VecNormalizeOrDefault(pEmitter->OrientFront, {0.0f, 0.0f, 1.0f});
    const float emitter_cos = std::clamp(VecDot(emitter_front, dir_e2l), -1.0f, 1.0f);
    const float emitter_angle = std::acos(emitter_cos);
    if ((Flags & OS_X3DAUDIO_CALCULATE_EMITTER_ANGLE) != 0u) {
        pDSPSettings->EmitterToListenerAngle = emitter_angle;
    }

    float cone_lpf = 0.0f;
    float cone_reverb = 1.0f;
    if (pEmitter->pCone) {
        const float half_inner = 0.5f * pEmitter->pCone->InnerAngle;
        const float half_outer = 0.5f * pEmitter->pCone->OuterAngle;
        float t = 0.0f;
        if (emitter_angle <= half_inner) {
            t = 0.0f;
        } else if (emitter_angle >= half_outer || half_outer <= half_inner) {
            t = 1.0f;
        } else {
            t = (emitter_angle - half_inner) / (half_outer - half_inner);
        }
        const float cone_vol =
            pEmitter->pCone->InnerVolume + t * (pEmitter->pCone->OuterVolume - pEmitter->pCone->InnerVolume);
        volume *= std::clamp(cone_vol, 0.0f, 2.0f);
        cone_lpf = pEmitter->pCone->InnerLPF + t * (pEmitter->pCone->OuterLPF - pEmitter->pCone->InnerLPF);
        cone_reverb =
            pEmitter->pCone->InnerReverb + t * (pEmitter->pCone->OuterReverb - pEmitter->pCone->InnerReverb);
    }

    // 1. Speaker Panning Matrix (Mono / Stereo / Quad / 5.1 / 7.1 equal-power panning)
    if ((Flags & OS_X3DAUDIO_CALCULATE_MATRIX) != 0u && pDSPSettings->pMatrixCoefficients != nullptr) {
        const uint32_t src_ch = std::max(1u, pDSPSettings->SrcChannelCount);
        const uint32_t dst_ch = std::max(1u, pDSPSettings->DstChannelCount);
        std::fill_n(pDSPSettings->pMatrixCoefficients, static_cast<size_t>(src_ch) * dst_ch, 0.0f);

        const float base_azimuth = (distance > 1e-6f) ? std::atan2(local_x, local_z) : 0.0f;
        const float inner_blend =
            (pEmitter->InnerRadius > 1e-6f && distance < pEmitter->InnerRadius)
                ? (distance / pEmitter->InnerRadius)
                : 1.0f;
        const auto speakers = BuildSpeakerLayout(state.speaker_channel_mask, dst_ch);

        // Compute LFE gain if LFE channel exists.
        const float lfe_default = ((Flags & OS_X3DAUDIO_CALCULATE_REDIRECT_TO_LFE) != 0u) ? volume : 0.0f;
        const float lfe_gain =
            std::clamp(EvalDistanceCurve(pEmitter->pLFECurve, norm_dist, lfe_default), 0.0f, 1.0f);

        const float src_norm = 1.0f / std::sqrt(static_cast<float>(src_ch));
        for (uint32_t sc = 0; sc < src_ch; ++sc) {
            float ch_az = base_azimuth;
            float ch_pan_x = local_x * inner_blend;
            if (src_ch > 1u && pEmitter->pChannelAzimuths != nullptr) {
                ch_az = WrapAnglePi(base_azimuth + pEmitter->pChannelAzimuths[sc]);
                ch_pan_x = std::sin(ch_az) * inner_blend;
            }

            if (dst_ch == 1u) {
                pDSPSettings->pMatrixCoefficients[sc] = volume * src_norm;
            } else if (dst_ch == 2u && state.speaker_channel_mask == OS_SPEAKER_STEREO) {
                // Equal-power stereo panning: pan in [-1, +1] -> t in [0, 1].
                const float pan = std::clamp(ch_pan_x, -1.0f, 1.0f);
                const float t = 0.5f * (pan + 1.0f);
                const float angle = t * (OS_X3DAUDIO_PI * 0.5f);
                const float gain_l = std::cos(angle) * volume * src_norm;
                const float gain_r = std::sin(angle) * volume * src_norm;
                pDSPSettings->pMatrixCoefficients[0u * src_ch + sc] = gain_l;
                pDSPSettings->pMatrixCoefficients[1u * src_ch + sc] = gain_r;
            } else {
                // Multi-channel equal-power circular speaker pair panning (Quad / 5.1 / 7.1).
                std::vector<SpeakerSlot> ring;
                ring.reserve(speakers.size());
                for (const auto& sp : speakers) {
                    if (sp.is_lfe) {
                        pDSPSettings->pMatrixCoefficients[static_cast<size_t>(sp.dst_index) * src_ch + sc] =
                            lfe_gain * src_norm;
                        continue;
                    }
                    if (sp.is_center && (Flags & OS_X3DAUDIO_CALCULATE_ZEROCENTER) != 0u) {
                        continue;
                    }
                    ring.push_back(sp);
                }
                if (!ring.empty()) {
                    std::sort(ring.begin(), ring.end(),
                              [](const SpeakerSlot& a, const SpeakerSlot& b) {
                                  return a.azimuth < b.azimuth;
                              });
                    if (ring.size() == 1u) {
                        pDSPSettings->pMatrixCoefficients[static_cast<size_t>(ring[0].dst_index) * src_ch + sc] =
                            volume * src_norm;
                    } else {
                        // Find adjacent pair bracketing ch_az on the circle (-pi, +pi].
                        const size_t n = ring.size();
                        size_t idx_a = n - 1u;
                        size_t idx_b = 0u;
                        for (size_t i = 0; i + 1u < n; ++i) {
                            if (ch_az >= ring[i].azimuth && ch_az <= ring[i + 1u].azimuth) {
                                idx_a = i;
                                idx_b = i + 1u;
                                break;
                            }
                        }
                        float az_a = ring[idx_a].azimuth;
                        float az_b = ring[idx_b].azimuth;
                        float az_q = ch_az;
                        if (idx_b == 0u) {
                            az_b += 2.0f * OS_X3DAUDIO_PI;
                            if (az_q < az_a) {
                                az_q += 2.0f * OS_X3DAUDIO_PI;
                            }
                        }
                        const float span = std::max(1e-5f, az_b - az_a);
                        const float u = std::clamp((az_q - az_a) / span, 0.0f, 1.0f);
                        const float gain_a = std::cos(u * (OS_X3DAUDIO_PI * 0.5f)) * volume * src_norm;
                        const float gain_b = std::sin(u * (OS_X3DAUDIO_PI * 0.5f)) * volume * src_norm;

                        // If inside InnerRadius, blend smoothly toward omnidirectional distribution.
                        if (inner_blend < 1.0f) {
                            const float omni = (1.0f - inner_blend) * volume * src_norm /
                                               std::sqrt(static_cast<float>(n));
                            for (const auto& sp : ring) {
                                pDSPSettings->pMatrixCoefficients[static_cast<size_t>(sp.dst_index) * src_ch + sc] +=
                                    omni;
                            }
                        }
                        pDSPSettings->pMatrixCoefficients[static_cast<size_t>(ring[idx_a].dst_index) * src_ch + sc] +=
                            gain_a * inner_blend;
                        pDSPSettings->pMatrixCoefficients[static_cast<size_t>(ring[idx_b].dst_index) * src_ch + sc] +=
                            gain_b * inner_blend;
                    }
                }
            }
        }
    }

    // 2. Inter-channel Delay Times (ms)
    if ((Flags & OS_X3DAUDIO_CALCULATE_DELAY) != 0u && pDSPSettings->pDelayTimes != nullptr) {
        const uint32_t dst_ch = std::max(1u, pDSPSettings->DstChannelCount);
        const auto speakers = BuildSpeakerLayout(state.speaker_channel_mask, dst_ch);
        const float az = (distance > 1e-6f) ? std::atan2(local_x, local_z) : 0.0f;
        for (const auto& sp : speakers) {
            if (sp.dst_index < dst_ch) {
                const float diff = std::fabs(WrapAnglePi(az - sp.azimuth));
                // Up to ~1.5 ms interaural time difference across the head/speaker ring.
                pDSPSettings->pDelayTimes[sp.dst_index] = (diff / OS_X3DAUDIO_PI) * 1.5f;
            }
        }
    }

    // 3. LPF Direct, LPF Reverb & Reverb Level
    if ((Flags & OS_X3DAUDIO_CALCULATE_LPF_DIRECT) != 0u) {
        const float default_lpf = std::clamp(1.0f - 0.05f * norm_dist, 0.25f, 1.0f);
        const float raw_lpf = EvalDistanceCurve(pEmitter->pLPFDirectCurve, norm_dist, default_lpf);
        pDSPSettings->LPFDirectCoefficient = std::clamp(raw_lpf - cone_lpf * 0.25f, 0.0f, 1.0f);
    }
    if ((Flags & OS_X3DAUDIO_CALCULATE_LPF_REVERB) != 0u) {
        const float default_lpf_rev = std::clamp(0.75f - 0.03f * norm_dist, 0.2f, 0.75f);
        pDSPSettings->LPFReverbCoefficient =
            std::clamp(EvalDistanceCurve(pEmitter->pLPFReverbCurve, norm_dist, default_lpf_rev), 0.0f, 1.0f);
    }
    if ((Flags & OS_X3DAUDIO_CALCULATE_REVERB) != 0u) {
        const float default_rev = std::clamp(0.25f * volume, 0.0f, 1.0f);
        const float raw_rev = EvalDistanceCurve(pEmitter->pReverbCurve, norm_dist, default_rev);
        pDSPSettings->ReverbLevel = std::clamp(raw_rev * cone_reverb, 0.0f, 1.0f);
    }

    // 4. Doppler Pitch Shift Factor: (c - v_listener) / (c - v_emitter)
    //    where v_listener & v_emitter are velocity projections along emitter-to-listener axis dir_e2l.
    if ((Flags & OS_X3DAUDIO_CALCULATE_DOPPLER) != 0u) {
        const float v_emitter = (distance > 1e-6f) ? VecDot(pEmitter->Velocity, dir_e2l) : 0.0f;
        const float v_listener = (distance > 1e-6f) ? VecDot(pListener->Velocity, dir_e2l) : 0.0f;
        pDSPSettings->EmitterVelocityComponent = v_emitter;
        pDSPSettings->ListenerVelocityComponent = v_listener;

        const float dscaler =
            (pEmitter->DopplerScaler >= 0.0f && std::isfinite(pEmitter->DopplerScaler))
                ? pEmitter->DopplerScaler
                : 1.0f;
        if (dscaler == 0.0f || distance <= 1e-6f) {
            pDSPSettings->DopplerFactor = 1.0f;
        } else {
            const float c = state.speed_of_sound;
            const float min_term = c * 0.05f;
            const float numer = std::max(min_term, c - v_listener * dscaler);
            const float denom = std::max(min_term, c - v_emitter * dscaler);
            pDSPSettings->DopplerFactor = std::clamp(numer / denom, 1.0f / 1024.0f, 1024.0f);
        }
    }
}

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateReverb(
    OS_IXAudio2Unknown** ppApo,
    uint32_t Flags) {
    if (!ppApo) {
        return RecordHr("XAudio2CreateReverb", OS_XAUDIO2_E_POINTER);
    }
    *ppApo = nullptr;
    auto* apo = new (std::nothrow) ReverbApoImpl(Flags);
    if (!apo) {
        return RecordHr("XAudio2CreateReverb", OS_XAUDIO2_E_OUTOFMEMORY);
    }
    *ppApo = apo;
    return RecordHr("XAudio2CreateReverb", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateVolumeMeter(
    OS_IXAudio2Unknown** ppApo,
    uint32_t Flags) {
    if (!ppApo) {
        return RecordHr("XAudio2CreateVolumeMeter", OS_XAUDIO2_E_POINTER);
    }
    *ppApo = nullptr;
    auto* apo = new (std::nothrow) VolumeMeterApoImpl(Flags);
    if (!apo) {
        return RecordHr("XAudio2CreateVolumeMeter", OS_XAUDIO2_E_OUTOFMEMORY);
    }
    *ppApo = apo;
    return RecordHr("XAudio2CreateVolumeMeter", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2RenderMixForTest(
    IXAudio2* pXAudio2,
    float* out_frames,
    uint32_t frame_count) {
    if (!pXAudio2 || !out_frames || frame_count == 0) {
        return RecordHr("OpenScarlettXAudio2RenderMixForTest", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    const int32_t hr = impl->RenderMixFrames(out_frames, frame_count);
    return RecordHr("OpenScarlettXAudio2RenderMixForTest", hr);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2GetBackendInfo(
    IXAudio2* pXAudio2,
    OS_XAUDIO2_BACKEND_INFO* out_info) {
    if (!pXAudio2 || !out_info) {
        return RecordHr("OpenScarlettXAudio2GetBackendInfo", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    impl->GetBackendInfo(*out_info);
    return RecordHr("OpenScarlettXAudio2GetBackendInfo", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ValidateEndpointIdW(
    const wchar_t* deviceId,
    wchar_t* outCanonicalId,
    uint32_t outCanonicalChars) {
    const ParsedEndpointId parsed = ParseMmDeviceEndpointIdW(deviceId);
    if (!parsed.valid) {
        if (outCanonicalId && outCanonicalChars > 0u) {
            outCanonicalId[0] = L'\0';
        }
        return RecordHr("OpenScarlettXAudio2ValidateEndpointIdW", OS_XAUDIO2_E_INVALIDARG);
    }
    if (outCanonicalId && outCanonicalChars > 0u) {
        const size_t needed = parsed.canonical_id.size() + 1u;
        if (outCanonicalChars < needed) {
            outCanonicalId[0] = L'\0';
            return RecordHr("OpenScarlettXAudio2ValidateEndpointIdW", OS_XAUDIO2_E_INVALIDARG);
        }
        std::memcpy(outCanonicalId, parsed.canonical_id.c_str(), needed * sizeof(wchar_t));
    }
    return RecordHr("OpenScarlettXAudio2ValidateEndpointIdW", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ValidateEndpointIdA(
    const char* deviceId,
    char* outCanonicalId,
    uint32_t outCanonicalChars) {
    std::wstring wide_in;
    if (deviceId && deviceId[0] != '\0') {
        for (const char* p = deviceId; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20u || c >= 0x7Fu) {
                if (outCanonicalId && outCanonicalChars > 0u) {
                    outCanonicalId[0] = '\0';
                }
                return RecordHr("OpenScarlettXAudio2ValidateEndpointIdA", OS_XAUDIO2_E_INVALIDARG);
            }
            wide_in.push_back(static_cast<wchar_t>(c));
        }
    }
    const ParsedEndpointId parsed =
        ParseMmDeviceEndpointIdW(deviceId ? wide_in.c_str() : nullptr);
    if (!parsed.valid) {
        if (outCanonicalId && outCanonicalChars > 0u) {
            outCanonicalId[0] = '\0';
        }
        return RecordHr("OpenScarlettXAudio2ValidateEndpointIdA", OS_XAUDIO2_E_INVALIDARG);
    }
    if (outCanonicalId && outCanonicalChars > 0u) {
        const size_t needed = parsed.canonical_id.size() + 1u;
        if (outCanonicalChars < needed) {
            outCanonicalId[0] = '\0';
            return RecordHr("OpenScarlettXAudio2ValidateEndpointIdA", OS_XAUDIO2_E_INVALIDARG);
        }
        for (size_t i = 0u; i < parsed.canonical_id.size(); ++i) {
            outCanonicalId[i] = static_cast<char>(parsed.canonical_id[i] & 0x7Fu);
        }
        outCanonicalId[parsed.canonical_id.size()] = '\0';
    }
    return RecordHr("OpenScarlettXAudio2ValidateEndpointIdA", OS_XAUDIO2_S_OK);
}

int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ProbeWasapiEndpointW(
    const wchar_t* deviceId,
    OS_XAUDIO2_ENDPOINT_PROBE* outProbe) {
    if (!outProbe) {
        return RecordHr("OpenScarlettXAudio2ProbeWasapiEndpointW", OS_XAUDIO2_E_POINTER);
    }
    std::memset(outProbe, 0, sizeof(*outProbe));
    const ParsedEndpointId parsed = ParseMmDeviceEndpointIdW(deviceId);
    if (!parsed.valid) {
        return RecordHr("OpenScarlettXAudio2ProbeWasapiEndpointW", OS_XAUDIO2_E_INVALIDARG);
    }
    outProbe->format_valid = 1u;
    outProbe->is_default = parsed.is_default ? 1u : 0u;
    const size_t copy_len = std::min<size_t>(parsed.canonical_id.size(), 255u);
    if (copy_len > 0u) {
        std::memcpy(outProbe->canonical_id, parsed.canonical_id.c_str(), copy_len * sizeof(wchar_t));
    }
    outProbe->canonical_id[copy_len] = L'\0';

#if defined(_WIN32)
    if (!IsNullAudioForced()) {
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool co_init = SUCCEEDED(co_hr);
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        WAVEFORMATEX* mix_fmt = nullptr;

        HRESULT hr = CoCreateInstance(
            kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
            kIidImmDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr) && enumerator) {
            if (!parsed.is_default) {
                hr = enumerator->GetDevice(parsed.canonical_id.c_str(), &device);
                if (SUCCEEDED(hr) && device) {
                    outProbe->exact_device_found = 1u;
                }
            }
            if (!device) {
                hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
                if (FAILED(hr) || !device) {
                    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
                }
                if (SUCCEEDED(hr) && device) {
                    if (parsed.is_default) {
                        outProbe->exact_device_found = 1u;
                        LPWSTR actual_id = nullptr;
                        if (SUCCEEDED(device->GetId(&actual_id)) && actual_id) {
                            const size_t alen = std::min<size_t>(std::wcslen(actual_id), 255u);
                            std::memcpy(outProbe->canonical_id, actual_id, alen * sizeof(wchar_t));
                            outProbe->canonical_id[alen] = L'\0';
                            CoTaskMemFree(actual_id);
                        }
                    } else {
                        outProbe->fallback_used = 1u;
                    }
                }
            }
            if (device) {
                hr = device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(&client));
                if (SUCCEEDED(hr) && client) {
                    hr = client->GetMixFormat(&mix_fmt);
                    if (SUCCEEDED(hr) && mix_fmt) {
                        outProbe->mix_sample_rate = mix_fmt->nSamplesPerSec;
                        outProbe->mix_channels = mix_fmt->nChannels;
                        outProbe->mix_bits_per_sample = mix_fmt->wBitsPerSample;
                    }
                }
            }
        }
        outProbe->wasapi_hresult = static_cast<int32_t>(hr);
        if (mix_fmt) CoTaskMemFree(mix_fmt);
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (co_init) CoUninitialize();
    }
#endif

    return RecordHr("OpenScarlettXAudio2ProbeWasapiEndpointW", OS_XAUDIO2_S_OK);
}

// --- Sprint 9/10 Effect Chain Control & Null-Mode Audio Clock Advancement ---

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2SetEffectChain(IXAudio2Voice* pVoice, const void* pEffectChain) {
    if (!pVoice) return RecordHr("OpenScarlettXAudio2SetEffectChain", OS_XAUDIO2_E_INVALIDARG);
    int32_t hr = OS_XAUDIO2_E_INVALIDARG;
    if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pVoice)) {
        hr = sub->SetEffectChain(pEffectChain);
    } else if (auto* src = dynamic_cast<SourceVoiceImpl*>(pVoice)) {
        hr = src->SetEffectChain(pEffectChain);
    } else if (auto* mv = dynamic_cast<MasteringVoiceImpl*>(pVoice)) {
        hr = mv->SetEffectChain(pEffectChain);
        if (hr >= 0) {
            // Ensure engine sees dynamically added master effects on the mix path.
            (void)mv;
        }
    }
    return RecordHr("OpenScarlettXAudio2SetEffectChain", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2EnableEffect(IXAudio2Voice* pVoice, uint32_t EffectIndex, int32_t Enable) {
    if (!pVoice) return RecordHr("OpenScarlettXAudio2EnableEffect", OS_XAUDIO2_E_INVALIDARG);
    int32_t hr = OS_XAUDIO2_E_INVALIDARG;
    if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pVoice)) {
        hr = sub->EnableEffect(EffectIndex, Enable != 0);
    } else if (auto* src = dynamic_cast<SourceVoiceImpl*>(pVoice)) {
        hr = src->EnableEffect(EffectIndex, Enable != 0);
    } else if (auto* mv = dynamic_cast<MasteringVoiceImpl*>(pVoice)) {
        hr = mv->EnableEffect(EffectIndex, Enable != 0);
    }
    return RecordHr("OpenScarlettXAudio2EnableEffect", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2GetEffectState(IXAudio2Voice* pVoice, uint32_t EffectIndex, int32_t* pEnabled) {
    if (!pVoice || !pEnabled) return RecordHr("OpenScarlettXAudio2GetEffectState", OS_XAUDIO2_E_INVALIDARG);
    int32_t hr = OS_XAUDIO2_E_INVALIDARG;
    if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pVoice)) {
        hr = sub->GetEffectState(EffectIndex, pEnabled);
    } else if (auto* src = dynamic_cast<SourceVoiceImpl*>(pVoice)) {
        hr = src->GetEffectState(EffectIndex, pEnabled);
    } else if (auto* mv = dynamic_cast<MasteringVoiceImpl*>(pVoice)) {
        hr = mv->GetEffectState(EffectIndex, pEnabled);
    }
    return RecordHr("OpenScarlettXAudio2GetEffectState", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2SetEffectParameters(
    IXAudio2Voice* pVoice,
    uint32_t EffectIndex,
    const void* pParameters,
    uint32_t ParametersByteSize) {
    if (!pVoice) return RecordHr("OpenScarlettXAudio2SetEffectParameters", OS_XAUDIO2_E_INVALIDARG);
    int32_t hr = OS_XAUDIO2_E_INVALIDARG;
    if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pVoice)) {
        hr = sub->SetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    } else if (auto* src = dynamic_cast<SourceVoiceImpl*>(pVoice)) {
        hr = src->SetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    } else if (auto* mv = dynamic_cast<MasteringVoiceImpl*>(pVoice)) {
        hr = mv->SetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    }
    return RecordHr("OpenScarlettXAudio2SetEffectParameters", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2GetEffectParameters(
    IXAudio2Voice* pVoice,
    uint32_t EffectIndex,
    void* pParameters,
    uint32_t ParametersByteSize) {
    if (!pVoice) return RecordHr("OpenScarlettXAudio2GetEffectParameters", OS_XAUDIO2_E_INVALIDARG);
    int32_t hr = OS_XAUDIO2_E_INVALIDARG;
    if (auto* sub = dynamic_cast<SubmixVoiceImpl*>(pVoice)) {
        hr = sub->GetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    } else if (auto* src = dynamic_cast<SourceVoiceImpl*>(pVoice)) {
        hr = src->GetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    } else if (auto* mv = dynamic_cast<MasteringVoiceImpl*>(pVoice)) {
        hr = mv->GetEffectParameters(EffectIndex, pParameters, ParametersByteSize);
    }
    return RecordHr("OpenScarlettXAudio2GetEffectParameters", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2AdvanceClock(
    IXAudio2* pXAudio2,
    uint32_t frame_count,
    uint64_t* out_total_frames_mixed) {
    if (!pXAudio2 || frame_count == 0u) {
        return RecordHr("OpenScarlettXAudio2AdvanceClock", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    std::vector<float> discard(static_cast<size_t>(frame_count) * 8u, 0.0f);
    const int32_t hr = impl->RenderMixFrames(discard.data(), frame_count);
    if (out_total_frames_mixed) {
        *out_total_frames_mixed = impl->TotalFramesMixed();
    }
    return RecordHr("OpenScarlettXAudio2AdvanceClock", hr);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettXAudio2GetLockFreeMixerStats(
    IXAudio2* pXAudio2,
    uint32_t* out_capacity,
    uint32_t* out_peak_concurrent_voices,
    uint64_t* out_lock_free_passes) {
    if (!pXAudio2) {
        return RecordHr("OpenScarlettXAudio2GetLockFreeMixerStats", OS_XAUDIO2_E_INVALIDARG);
    }
    auto* impl = static_cast<XAudio2Impl*>(pXAudio2);
    impl->GetLockFreeMixerStats(out_capacity, out_peak_concurrent_voices, out_lock_free_passes);
    return RecordHr("OpenScarlettXAudio2GetLockFreeMixerStats", OS_XAUDIO2_S_OK);
}

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL
OpenScarlettCreateSpatialAudioClient(void** ppSpatialAudioClient) {
    if (!ppSpatialAudioClient) {
        return RecordHr("OpenScarlettCreateSpatialAudioClient", OS_XAUDIO2_E_POINTER);
    }
    *ppSpatialAudioClient = nullptr;
    auto* client = new (std::nothrow) SpatialAudioClientImpl();
    if (!client) {
        return RecordHr("OpenScarlettCreateSpatialAudioClient", OS_XAUDIO2_E_OUTOFMEMORY);
    }
    *ppSpatialAudioClient = static_cast<OS_ISpatialAudioClient*>(client);
    return RecordHr("OpenScarlettCreateSpatialAudioClient", OS_XAUDIO2_S_OK);
}

}  // extern "C"
