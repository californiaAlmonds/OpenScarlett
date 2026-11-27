#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/loader/apc_injector.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

void SetEnvVarLocal(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct InviteRecvCtx {
    std::atomic<int> hits{0};
    std::string last_uri;
};

void OPENSCARLETT_GDK_CALL TestInviteCallback(void* context, const char* inviteUri) {
    auto* ctx = static_cast<InviteRecvCtx*>(context);
    if (ctx) {
        ctx->last_uri = inviteUri ? inviteUri : "";
        ctx->hits.fetch_add(1, std::memory_order_release);
    }
}

bool OPENSCARLETT_GDK_CALL EnumVoicesCb(
    const XSpeechSynthesizerVoiceInformation* info,
    void* context) {
    auto* ids = static_cast<std::vector<std::string>*>(context);
    if (info && info->VoiceId && ids) {
        ids->emplace_back(info->VoiceId);
    }
    return true;
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ===========================================================================
// 1. XDisplay HDR & Timeout Deferral
// ===========================================================================
void TestXDisplayHdrAndDeferral() {
    OS_REQUIRE(XGameRuntimeInitialize() == XGR_S_OK);

    // Force SDR path deterministically (GDK zeroes luminance metadata when HDR is disabled)
    SetEnvVarLocal("OPENSCARLETT_FORCE_HDR", "0");
    XDisplayHdrModeInfo sdr_info{0.05f, 300.0f, 250.0f};
    const XDisplayHdrModeResult sdr_res =
        XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferHdr, &sdr_info);
    OS_REQUIRE(sdr_res == XDisplayHdrModeResult_Disabled);
    OS_REQUIRE(sdr_info.minToneMapLuminance == 0.0f &&
               sdr_info.maxToneMapLuminance == 0.0f &&
               sdr_info.maxFullFrameToneMapLuminance == 0.0f);

    // Force HDR10 path deterministically
    SetEnvVarLocal("OPENSCARLETT_FORCE_HDR", "1");
    XDisplayHdrModeInfo hdr_info{};
    const XDisplayHdrModeResult hdr_res =
        XDisplayTryEnableHdrMode(XDisplayHdrModePreference_PreferHdr, &hdr_info);
    OS_REQUIRE(hdr_res == XDisplayHdrModeResult_Enabled);
    OS_REQUIRE(hdr_info.minToneMapLuminance <= 0.01f);
    OS_REQUIRE(hdr_info.maxToneMapLuminance >= 1000.0f);
    OS_REQUIRE(hdr_info.maxFullFrameToneMapLuminance >= 400.0f);

    // Clear env override
    SetEnvVarLocal("OPENSCARLETT_FORCE_HDR", nullptr);

    // Acquire + close timeout deferral (official XDisplayAcquireTimeoutDeferral)
    XDisplayTimeoutDeferralHandle deferral = nullptr;
    OS_REQUIRE(XDisplayAcquireTimeoutDeferral(&deferral) == XGR_S_OK);
    OS_REQUIRE(deferral != nullptr);
    XDisplayCloseTimeoutDeferralHandle(deferral);
    // Double-close or null-close must be safe
    XDisplayCloseTimeoutDeferralHandle(nullptr);

    std::printf("[PASS] XDisplay (SDR + OPENSCARLETT_FORCE_HDR=1 HDR10 luminance metadata + deferral)\n");
}

// ===========================================================================
// 2. XLauncher, XGameUI, XGameInvite, XAccessibility, XSpeechSynthesizer
// ===========================================================================
void TestExtendedGdkApis() {
    // --- XLauncher ---
    OS_REQUIRE(XLaunchUri(nullptr, "https://github.com/OpenScarlett") == XGR_S_OK);
    OS_REQUIRE(XLaunchUri(nullptr, "ms-xbl-12345678://invite") == XGR_S_OK);
    OS_REQUIRE(XLaunchUri(nullptr, "") == XGR_E_INVALIDARG);
    OS_REQUIRE(XLaunchUri(nullptr, "no_scheme_uri") == XGR_E_INVALIDARG);

    // --- XGameUI Message Dialog ---
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUIShowMessageDialogAsync(
                       &ab,
                       "Confirm",
                       "Proceed with test?",
                       "Yes",
                       "No",
                       "Cancel",
                       XGameUIMessageDialogButton_Second,
                       XGameUIMessageDialogButton_Third) == XGR_S_OK);
        XGameUIMessageDialogButton btn = XGameUIMessageDialogButton_First;
        OS_REQUIRE(XGameUIShowMessageDialogResult(&ab, &btn) == XGR_S_OK);
        OS_REQUIRE(btn == XGameUIMessageDialogButton_Second);
    }

    // --- XGameUI Text Entry ---
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUIShowTextEntryAsync(
                       &ab,
                       "Profile Name",
                       "Enter your callsign",
                       "ScarlettPilot99",
                       XGameUITextEntryInputScope_Alphanumeric,
                       13u) == XGR_S_OK);
        uint32_t needed = 0;
        OS_REQUIRE(XGameUIShowTextEntryResultSize(&ab, &needed) == XGR_S_OK);
        OS_REQUIRE(needed == 14u);  // "ScarlettPilot" (13 chars) + NUL
        char buf[64]{};
        uint32_t used = 0;
        OS_REQUIRE(XGameUIShowTextEntryResult(&ab, sizeof(buf), buf, &used) == XGR_S_OK);
        OS_REQUIRE(used == 14u && std::strcmp(buf, "ScarlettPilot") == 0);
    }

    // --- XGameUI Player Picker ---
    {
        const uint64_t candidates[4] = {1001ULL, 1002ULL, 1003ULL, 1004ULL};
        const uint64_t preselected[1] = {1003ULL};
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUIShowPlayerPickerAsync(
                       &ab,
                       nullptr,
                       "Select Squad",
                       4u,
                       candidates,
                       1u,
                       preselected,
                       2u,
                       3u) == XGR_S_OK);
        uint32_t count = 0;
        OS_REQUIRE(XGameUIShowPlayerPickerResultCount(&ab, &count) == XGR_S_OK);
        OS_REQUIRE(count == 2u);
        uint64_t picked[4]{};
        uint32_t used = 0;
        OS_REQUIRE(XGameUIShowPlayerPickerResult(&ab, 4u, picked, &used) == XGR_S_OK);
        OS_REQUIRE(used == 2u && picked[0] == 1003ULL && picked[1] == 1001ULL);
    }

    // --- XGameUI Error Dialog & Notification Position ---
    {
        XAsyncBlock ab{};
        OS_REQUIRE(XGameUIShowErrorDialogAsync(&ab, XGR_E_FAIL, "TestErrorContext") == XGR_S_OK);
        OS_REQUIRE(XGameUIShowErrorDialogResult(&ab) == XGR_S_OK);
        OS_REQUIRE(XGameUISetNotificationPositionHint(XGameUINotificationPositionHint_TopRight) == XGR_S_OK);
    }

    // --- XGameInvite ---
    {
        XTaskQueueHandle q = nullptr;
        OS_REQUIRE(XTaskQueueCreate(
                       XTaskQueueDispatchMode_Manual,
                       XTaskQueueDispatchMode_Manual,
                       &q) == XGR_S_OK);
        InviteRecvCtx rctx{};
        XTaskQueueRegistrationToken tok{};
        OS_REQUIRE(XGameInviteRegisterForEvent(q, &rctx, &TestInviteCallback, &tok) == XGR_S_OK);
        OS_REQUIRE(tok.token != 0);

        OS_REQUIRE(XGameInviteDispatchTestEventForTest("ms-xbl-test://join?session=42") == XGR_S_OK);
        OS_REQUIRE(XTaskQueueDispatch(q, XTaskQueuePort_Completion, 100));
        OS_REQUIRE(rctx.hits.load() == 1);
        OS_REQUIRE(rctx.last_uri == "ms-xbl-test://join?session=42");

        OS_REQUIRE(XGameInviteUnregisterForEvent(tok, true));
        OS_REQUIRE(!XGameInviteUnregisterForEvent(tok, true));
        OS_REQUIRE(XGameInviteDispatchTestEventForTest("ms-xbl-test://join?session=99") == XGR_S_OK);
        (void)XTaskQueueDispatch(q, XTaskQueuePort_Completion, 0);
        OS_REQUIRE(rctx.hits.load() == 1);
        XTaskQueueCloseHandle(q);
    }

    // --- XAccessibility (Closed Caption & High Contrast) ---
    {
        OS_REQUIRE(XClosedCaptionSetEnabled(true) == XGR_S_OK);
        XClosedCaptionProperties props{};
        OS_REQUIRE(XClosedCaptionGetProperties(&props) == XGR_S_OK);
        OS_REQUIRE(props.Enabled);
        OS_REQUIRE(props.FontScale > 0.0f);
        OS_REQUIRE(XClosedCaptionSetEnabled(false) == XGR_S_OK);
        OS_REQUIRE(XClosedCaptionGetProperties(&props) == XGR_S_OK);
        OS_REQUIRE(!props.Enabled);

        SetEnvVarLocal("OPENSCARLETT_HIGH_CONTRAST", "dark");
        XHighContrastMode hc = XHighContrastMode_Off;
        OS_REQUIRE(XHighContrastGetMode(&hc) == XGR_S_OK);
        OS_REQUIRE(hc == XHighContrastMode_Dark);
        SetEnvVarLocal("OPENSCARLETT_HIGH_CONTRAST", "off");
        OS_REQUIRE(XHighContrastGetMode(&hc) == XGR_S_OK);
        OS_REQUIRE(hc == XHighContrastMode_Off);
        SetEnvVarLocal("OPENSCARLETT_HIGH_CONTRAST", nullptr);
    }

    // --- XSpeechSynthesizer (Voices + Deterministic PCM WAV Synthesis) ---
    {
        std::vector<std::string> voices;
        OS_REQUIRE(XSpeechSynthesizerEnumerateInstalledVoices(&voices, &EnumVoicesCb) == XGR_S_OK);
        OS_REQUIRE(voices.size() >= 2u);

        XSpeechSynthesizerHandle synth = nullptr;
        OS_REQUIRE(XSpeechSynthesizerCreate(&synth) == XGR_S_OK && synth != nullptr);
        OS_REQUIRE(XSpeechSynthesizerSetDefaultVoice(synth) == XGR_S_OK);

        XSpeechSynthesizerStreamHandle stream = nullptr;
        OS_REQUIRE(XSpeechSynthesizerCreateStreamFromText(synth, "Hello OpenScarlett", &stream) == XGR_S_OK);
        OS_REQUIRE(stream != nullptr);

        size_t wav_size = 0;
        OS_REQUIRE(XSpeechSynthesizerGetStreamDataSize(stream, &wav_size) == XGR_S_OK);
        OS_REQUIRE(wav_size > 44u);

        std::vector<uint8_t> wav(wav_size, 0u);
        size_t wav_used = 0;
        OS_REQUIRE(XSpeechSynthesizerGetStreamData(stream, wav.size(), wav.data(), &wav_used) == XGR_S_OK);
        OS_REQUIRE(wav_used == wav_size);

        // Validate RIFF/WAVE PCM16 header and non-zero sample energy
        OS_REQUIRE(std::memcmp(wav.data(), "RIFF", 4) == 0);
        OS_REQUIRE(std::memcmp(wav.data() + 8, "WAVE", 4) == 0);
        OS_REQUIRE(std::memcmp(wav.data() + 12, "fmt ", 4) == 0);
        OS_REQUIRE(ReadLe16(wav.data() + 20) == 1u);       // PCM
        OS_REQUIRE(ReadLe16(wav.data() + 22) == 1u);       // mono
        OS_REQUIRE(ReadLe32(wav.data() + 24) == 22050u);   // 22050 Hz
        OS_REQUIRE(ReadLe16(wav.data() + 34) == 16u);      // 16-bit
        OS_REQUIRE(std::memcmp(wav.data() + 36, "data", 4) == 0);
        const uint32_t data_bytes = ReadLe32(wav.data() + 40);
        OS_REQUIRE(data_bytes == wav_size - 44u && data_bytes > 0u);

        int64_t energy = 0;
        const auto* pcm = reinterpret_cast<const int16_t*>(wav.data() + 44);
        const size_t n_samples = data_bytes / sizeof(int16_t);
        for (size_t i = 0; i < n_samples; ++i) {
            energy += std::abs(static_cast<int32_t>(pcm[i]));
        }
        OS_REQUIRE(energy > 10000);

        OS_REQUIRE(XSpeechSynthesizerCloseStreamHandle(stream) == XGR_S_OK);
        OS_REQUIRE(XSpeechSynthesizerCloseHandle(synth) == XGR_S_OK);
    }

    // --- QueryApiImpl for new vtables ---
    {
        OS_REQUIRE(InitializeApiImpl() == XGR_S_OK);
        IXgrGameUiApiVtable* ui_vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_GAME_UI, reinterpret_cast<void**>(&ui_vt)) == XGR_S_OK);
        OS_REQUIRE(ui_vt != nullptr && ui_vt->XLaunchUri != nullptr);
        ui_vt->Release(ui_vt);

        IXgrGameInviteApiVtable* inv_vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_GAME_INVITE, reinterpret_cast<void**>(&inv_vt)) == XGR_S_OK);
        OS_REQUIRE(inv_vt != nullptr && inv_vt->XGameInviteRegisterForEvent != nullptr);
        inv_vt->Release(inv_vt);

        IXgrAccessibilityApiVtable* acc_vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_ACCESSIBILITY, reinterpret_cast<void**>(&acc_vt)) == XGR_S_OK);
        OS_REQUIRE(acc_vt != nullptr && acc_vt->XHighContrastGetMode != nullptr);
        acc_vt->Release(acc_vt);

        IXgrSpeechSynthesizerApiVtable* sp_vt = nullptr;
        OS_REQUIRE(QueryApiImpl(&XGR_API_GUID_SPEECH_SYNTHESIZER, reinterpret_cast<void**>(&sp_vt)) == XGR_S_OK);
        OS_REQUIRE(sp_vt != nullptr && sp_vt->XSpeechSynthesizerCreateStreamFromText != nullptr);
        sp_vt->Release(sp_vt);
        UninitializeApiImpl();
    }

    // Verify CallTelemetry recorded our new APIs
    uint64_t calls = 0, not_impl = 0;
    OS_REQUIRE(XgrGetTelemetryCallCountForTest("XSpeechSynthesizerCreateStreamFromText", &calls, &not_impl) == 1);
    OS_REQUIRE(calls >= 1u && not_impl == 0u);

    std::printf("[PASS] Extended GDK APIs (XLauncher, XGameUI, XGameInvite, XAccessibility, XSpeechSynthesizer WAV + QueryApiImpl)\n");
}

// ===========================================================================
// 3. X3DAudio & XAudio2FX Tests
// ===========================================================================
void TestX3dAudioAndXAudio2Fx() {
    X3DAUDIO_HANDLE h3d{};
    OS_REQUIRE(X3DAudioInitialize(OS_SPEAKER_STEREO, OS_X3DAUDIO_SPEED_OF_SOUND, h3d) == OS_XAUDIO2_S_OK);

    X3DAUDIO_LISTENER listener{};
    listener.OrientFront = {0.0f, 0.0f, 1.0f};
    listener.OrientTop = {0.0f, 1.0f, 0.0f};
    listener.Position = {0.0f, 0.0f, 0.0f};
    listener.Velocity = {0.0f, 0.0f, 0.0f};

    X3DAUDIO_EMITTER emitter{};
    emitter.OrientFront = {0.0f, 0.0f, -1.0f};
    emitter.OrientTop = {0.0f, 1.0f, 0.0f};
    emitter.ChannelCount = 1u;
    emitter.CurveDistanceScaler = 1.0f;
    emitter.DopplerScaler = 1.0f;

    float matrix[2]{};
    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.pMatrixCoefficients = matrix;
    dsp.SrcChannelCount = 1u;
    dsp.DstChannelCount = 2u;

    const uint32_t flags =
        OS_X3DAUDIO_CALCULATE_MATRIX |
        OS_X3DAUDIO_CALCULATE_DOPPLER |
        OS_X3DAUDIO_CALCULATE_LPF_DIRECT |
        OS_X3DAUDIO_CALCULATE_LPF_REVERB |
        OS_X3DAUDIO_CALCULATE_REVERB;

    // A) Left-panned emitter at (-4, 0, 0)
    emitter.Position = {-4.0f, 0.0f, 0.0f};
    emitter.Velocity = {0.0f, 0.0f, 0.0f};
    X3DAudioCalculate(h3d, &listener, &emitter, flags, &dsp);
    OS_REQUIRE(std::fabs(dsp.EmitterToListenerDistance - 4.0f) < 1e-4f);
    OS_REQUIRE(matrix[0] > 0.20f && matrix[1] < 0.02f);  // Strongly left-panned
    const float near_gain_l = matrix[0];

    // B) Right-panned emitter at (+4, 0, 0)
    emitter.Position = {4.0f, 0.0f, 0.0f};
    X3DAudioCalculate(h3d, &listener, &emitter, flags, &dsp);
    OS_REQUIRE(matrix[1] > 0.20f && matrix[0] < 0.02f);  // Strongly right-panned
    OS_REQUIRE(std::fabs(matrix[1] - near_gain_l) < 1e-4f);

    // C) Distance attenuation falloff: farther emitter at (-16, 0, 0) must be quieter
    emitter.Position = {-16.0f, 0.0f, 0.0f};
    X3DAudioCalculate(h3d, &listener, &emitter, flags, &dsp);
    OS_REQUIRE(std::fabs(dsp.EmitterToListenerDistance - 16.0f) < 1e-4f);
    OS_REQUIRE(matrix[0] > 0.0f && matrix[0] < near_gain_l * 0.5f);

    // D) Doppler: approaching emitter at (0, 0, 10) moving toward listener at (0, 0, -34.35) -> ratio > 1.0
    emitter.Position = {0.0f, 0.0f, 10.0f};
    emitter.Velocity = {0.0f, 0.0f, -34.35f};
    X3DAudioCalculate(h3d, &listener, &emitter, flags, &dsp);
    OS_REQUIRE(dsp.DopplerFactor > 1.08f && dsp.DopplerFactor < 1.15f);
    // Centered emitter must pan equally to L & R
    OS_REQUIRE(std::fabs(matrix[0] - matrix[1]) < 1e-4f && matrix[0] > 0.0f);

    // E) Doppler: receding emitter at (0, 0, 10) moving away at (0, 0, +34.35) -> ratio < 1.0
    emitter.Velocity = {0.0f, 0.0f, 34.35f};
    X3DAudioCalculate(h3d, &listener, &emitter, flags, &dsp);
    OS_REQUIRE(dsp.DopplerFactor < 0.94f && dsp.DopplerFactor > 0.87f);
    OS_REQUIRE(dsp.LPFDirectCoefficient > 0.0f && dsp.LPFDirectCoefficient <= 1.0f);
    OS_REQUIRE(dsp.LPFReverbCoefficient > 0.0f && dsp.LPFReverbCoefficient <= 1.0f);

    // F) 5.1 surround panning check (rear-left emitter)
    X3DAUDIO_HANDLE h51{};
    OS_REQUIRE(X3DAudioInitialize(OS_SPEAKER_5POINT1, OS_X3DAUDIO_SPEED_OF_SOUND, h51) == OS_XAUDIO2_S_OK);
    float mat51[6]{};
    X3DAUDIO_DSP_SETTINGS dsp51{};
    dsp51.pMatrixCoefficients = mat51;
    dsp51.SrcChannelCount = 1u;
    dsp51.DstChannelCount = 6u;
    emitter.Position = {-2.0f, 0.0f, -2.0f};  // Back-left azimuth (-3pi/4)
    emitter.Velocity = {0.0f, 0.0f, 0.0f};
    X3DAudioCalculate(h51, &listener, &emitter, OS_X3DAUDIO_CALCULATE_MATRIX, &dsp51);
    OS_REQUIRE(mat51[4] > 0.25f);  // Back-Left (slot 4) receives primary energy

    // G) XAudio2FX Reverb & VolumeMeter COM lifecycle
    {
        OS_IXAudio2Unknown* reverb_unk = nullptr;
        OS_REQUIRE(XAudio2CreateReverb(&reverb_unk, 0u) == OS_XAUDIO2_S_OK && reverb_unk != nullptr);
        OS_REQUIRE(reverb_unk->AddRef() == 2u);

        OS_IXAPOParameters* reverb_params = nullptr;
        OS_REQUIRE(reverb_unk->QueryInterface(&IID_OS_IXAPOParameters,
                                              reinterpret_cast<void**>(&reverb_params)) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(reverb_params != nullptr);

        OS_XAUDIO2FX_REVERB_PARAMETERS rp{};
        reverb_params->GetParameters(&rp, sizeof(rp));
        OS_REQUIRE(rp.DecayTime > 0.0f);
        rp.DecayTime = 2.5f;
        rp.WetDryMix = 60.0f;
        reverb_params->SetParameters(&rp, sizeof(rp));
        OS_XAUDIO2FX_REVERB_PARAMETERS rp2{};
        reverb_params->GetParameters(&rp2, sizeof(rp2));
        OS_REQUIRE(std::fabs(rp2.DecayTime - 2.5f) < 1e-5f && std::fabs(rp2.WetDryMix - 60.0f) < 1e-5f);

        OS_REQUIRE(reverb_params->Release() == 2u);
        OS_REQUIRE(reverb_unk->Release() == 1u);
        OS_REQUIRE(reverb_unk->Release() == 0u);
    }
    {
        OS_IXAudio2Unknown* vm_unk = nullptr;
        OS_REQUIRE(XAudio2CreateVolumeMeter(&vm_unk, 0u) == OS_XAUDIO2_S_OK && vm_unk != nullptr);
        OS_IXAPOParameters* vm_params = nullptr;
        OS_REQUIRE(vm_unk->QueryInterface(&IID_OS_IXAPOParameters,
                                          reinterpret_cast<void**>(&vm_params)) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(vm_params != nullptr);

        float peaks[2] = {0.75f, 0.5f};
        float rms[2] = {0.4f, 0.25f};
        OS_XAUDIO2FX_VOLUMEMETER_LEVELS set_lv{peaks, rms, 2u};
        vm_params->SetParameters(&set_lv, sizeof(set_lv));

        float out_peaks[2]{};
        float out_rms[2]{};
        OS_XAUDIO2FX_VOLUMEMETER_LEVELS get_lv{out_peaks, out_rms, 2u};
        vm_params->GetParameters(&get_lv, sizeof(get_lv));
        OS_REQUIRE(std::fabs(out_peaks[0] - 0.75f) < 1e-5f && std::fabs(out_rms[1] - 0.25f) < 1e-5f);

        OS_REQUIRE(vm_params->Release() == 1u);
        OS_REQUIRE(vm_unk->Release() == 0u);
    }

    std::printf("[PASS] X3DAudio (stereo/5.1 panning, distance attenuation, Doppler) + XAudio2FX (Reverb & VolumeMeter COM APOs)\n");
}

// ===========================================================================
// 4. Topological DLL Dependency Initialization Ordering (Unit + Live Multi-DLL)
// ===========================================================================
size_t IndexOfName(const std::vector<std::string>& v, const std::string& target) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == target) return i;
    }
    return static_cast<size_t>(-1);
}

std::vector<std::string> ReadLines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream ifs(p);
    std::string line;
    while (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

void TestTopologicalDllOrdering() {
    // A) Unit test: Diamond DAG (D imports B, C; B & C import A) supplied in reverse order
    {
        std::vector<openscarlett::DllDependencyNode> nodes = {
            {"D.dll", "", 1, {"B.dll", "C.dll"}},
            {"C.dll", "", 2, {"A.dll"}},
            {"B.dll", "", 1, {"A.dll"}},
            {"A.dll", "", 5, {}},
        };
        const auto res = openscarlett::ComputeDllInitOrder(nodes);
        OS_REQUIRE(!res.had_cycles);
        OS_REQUIRE(res.cycle_broken_names.empty());
        OS_REQUIRE(res.ordered_names.size() == 4u);
        const size_t ia = IndexOfName(res.ordered_names, "A.dll");
        const size_t ib = IndexOfName(res.ordered_names, "B.dll");
        const size_t ic = IndexOfName(res.ordered_names, "C.dll");
        const size_t id = IndexOfName(res.ordered_names, "D.dll");
        OS_REQUIRE(ia == 0u);
        OS_REQUIRE(ia < ib && ia < ic);
        OS_REQUIRE(ib < ic);  // B has lower load_priority (1) than C (2)
        OS_REQUIRE(ib < id && ic < id && id == 3u);
    }

    // B) Unit test: Cycle breaking without hanging (Base <- CycA <-> CycB <- Leaf)
    {
        std::vector<openscarlett::DllDependencyNode> nodes = {
            {"Leaf.dll", "", 1, {"CycB.dll"}},
            {"CycB.dll", "", 2, {"CycA.dll"}},
            {"CycA.dll", "", 1, {"CycB.dll", "Base.dll"}},
            {"Base.dll", "", 5, {}},
        };
        const auto res = openscarlett::ComputeDllInitOrder(nodes);
        OS_REQUIRE(res.had_cycles);
        OS_REQUIRE(!res.cycle_broken_names.empty());
        OS_REQUIRE(res.ordered_names.size() == 4u);
        const size_t ibase = IndexOfName(res.ordered_names, "Base.dll");
        const size_t ia = IndexOfName(res.ordered_names, "CycA.dll");
        const size_t ib = IndexOfName(res.ordered_names, "CycB.dll");
        const size_t ileaf = IndexOfName(res.ordered_names, "Leaf.dll");
        OS_REQUIRE(ibase == 0u);          // Non-cyclic dependency always initializes first
        OS_REQUIRE(ia < ib && ib < ileaf); // Cycle broken deterministically by priority, Leaf last
    }

    // C) Live Multi-DLL PE import inspection + real DllMain(DLL_PROCESS_ATTACH) order verification
#if defined(_WIN32)
    fs::path bin_dir;
    {
        wchar_t self_buf[MAX_PATH]{};
        const DWORD self_len = ::GetModuleFileNameW(nullptr, self_buf, static_cast<DWORD>(MAX_PATH));
        if (self_len > 0 && self_len < MAX_PATH) {
            const fs::path exe_dir = fs::path(self_buf).parent_path();
            if (fs::exists(exe_dir / "test_dep_base.dll")) {
                bin_dir = exe_dir;
            }
        }
        if (bin_dir.empty()) {
            bin_dir = fs::path(OPENSCARLETT_BINARY_DIR);
        }
    }
    const fs::path p_base = bin_dir / "test_dep_base.dll";
    const fs::path p_mid = bin_dir / "test_dep_mid.dll";
    const fs::path p_top = bin_dir / "test_dep_top.dll";
    OS_REQUIRE(fs::exists(p_base) && fs::exists(p_mid) && fs::exists(p_top));

    // Deliberately pass in reversed order: top, mid, base!
    const std::vector<std::string> reversed_paths = {
        p_top.string(),
        p_mid.string(),
        p_base.string(),
    };

    openscarlett::DllInitOrderResult file_order{};
    OS_REQUIRE(openscarlett::ComputeDllInitOrderFromFiles(reversed_paths, file_order).ok());
    OS_REQUIRE(!file_order.had_cycles);
    OS_REQUIRE(file_order.ordered_names.size() == 3u);
    OS_REQUIRE(file_order.ordered_names[0] == "test_dep_base.dll");
    OS_REQUIRE(file_order.ordered_names[1] == "test_dep_mid.dll");
    OS_REQUIRE(file_order.ordered_names[2] == "test_dep_top.dll");

    // Set up temp log file strictly under %TEMP% (with fallback to bin_dir)
    fs::path log_dir = bin_dir;
    {
        wchar_t tmp_buf[MAX_PATH]{};
        const DWORD tmp_len = ::GetEnvironmentVariableW(L"TEMP", tmp_buf, static_cast<DWORD>(MAX_PATH));
        if (tmp_len > 0 && tmp_len < MAX_PATH) {
            log_dir = fs::path(tmp_buf);
        }
    }
    const fs::path log_file = log_dir / "wave3_dep_init_order.log";
    std::error_code ec;
    fs::remove(log_file, ec);
    SetEnvVarLocal("OPENSCARLETT_DEP_INIT_LOG", log_file.string().c_str());

    // 1. Live load via LoadDllsInTopologicalOrder
    std::vector<std::string> loaded_order;
    std::vector<void*> handles;
    OS_REQUIRE(openscarlett::LoadDllsInTopologicalOrder(reversed_paths, &loaded_order, &handles).ok());
    OS_REQUIRE(loaded_order.size() == 3u && handles.size() == 3u);
    OS_REQUIRE(loaded_order[0] == "test_dep_base.dll");
    OS_REQUIRE(loaded_order[1] == "test_dep_mid.dll");
    OS_REQUIRE(loaded_order[2] == "test_dep_top.dll");

    // Call exported function on test_dep_top.dll to confirm full import chain works (10 + 30 + 30 = 70)
    using PfnGetVal = int(__cdecl*)();
    auto fn_top = reinterpret_cast<PfnGetVal>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handles[2]), "TestDepTop_GetValue"));
    OS_REQUIRE(fn_top != nullptr);
    OS_REQUIRE(fn_top() == 70);

    // Verify actual DllMain(DLL_PROCESS_ATTACH) execution order from log file!
    const auto log_lines_1 = ReadLines(log_file);
    OS_REQUIRE(log_lines_1.size() == 3u);
    OS_REQUIRE(log_lines_1[0] == "test_dep_base.dll");
    OS_REQUIRE(log_lines_1[1] == "test_dep_mid.dll");
    OS_REQUIRE(log_lines_1[2] == "test_dep_top.dll");

    // Unload in reverse order so all 3 DLLs detach cleanly
    ::FreeLibrary(reinterpret_cast<HMODULE>(handles[2]));
    ::FreeLibrary(reinterpret_cast<HMODULE>(handles[1]));
    ::FreeLibrary(reinterpret_cast<HMODULE>(handles[0]));
    OS_REQUIRE(::GetModuleHandleW(L"test_dep_base.dll") == nullptr);
    OS_REQUIRE(::GetModuleHandleW(L"test_dep_mid.dll") == nullptr);
    OS_REQUIRE(::GetModuleHandleW(L"test_dep_top.dll") == nullptr);

    // 2. Live load via openscarlett_hook.dll's OpenScarlettHook_LoadDllsTopologically
    fs::remove(log_file, ec);
    const fs::path hook_path = bin_dir / "openscarlett_hook.dll";
    OS_REQUIRE(fs::exists(hook_path));
    HMODULE h_hook = ::LoadLibraryW(hook_path.wstring().c_str());
    OS_REQUIRE(h_hook != nullptr);
    using PfnHookLoadTopo = int(__cdecl*)(uint32_t, const wchar_t* const*, char*, uint32_t, HMODULE*);
    auto fn_hook_topo = reinterpret_cast<PfnHookLoadTopo>(
        ::GetProcAddress(h_hook, "OpenScarlettHook_LoadDllsTopologically"));
    OS_REQUIRE(fn_hook_topo != nullptr);

    const std::wstring w_top = p_top.wstring();
    const std::wstring w_mid = p_mid.wstring();
    const std::wstring w_base = p_base.wstring();
    const wchar_t* rev_w[3] = {w_top.c_str(), w_mid.c_str(), w_base.c_str()};
    char order_csv[256]{};
    HMODULE hook_mods[3]{};
    OS_REQUIRE(fn_hook_topo(3u, rev_w, order_csv, sizeof(order_csv), hook_mods) == 1);
    OS_REQUIRE(std::strcmp(order_csv, "test_dep_base.dll,test_dep_mid.dll,test_dep_top.dll") == 0);

    const auto log_lines_2 = ReadLines(log_file);
    OS_REQUIRE(log_lines_2.size() == 3u);
    OS_REQUIRE(log_lines_2[0] == "test_dep_base.dll");
    OS_REQUIRE(log_lines_2[1] == "test_dep_mid.dll");
    OS_REQUIRE(log_lines_2[2] == "test_dep_top.dll");

    ::FreeLibrary(hook_mods[2]);
    ::FreeLibrary(hook_mods[1]);
    ::FreeLibrary(hook_mods[0]);
    SetEnvVarLocal("OPENSCARLETT_DEP_INIT_LOG", nullptr);
    fs::remove(log_file, ec);
#endif

    std::printf("[PASS] Topological DLL dependency init ordering (DAG + cycle breaking + live 3-DLL DllMain verification)\n");
}

}  // namespace

int main() {
    TestXDisplayHdrAndDeferral();
    TestExtendedGdkApis();
    TestX3dAudioAndXAudio2Fx();
    TestTopologicalDllOrdering();
    std::printf("ALL EXTENDED GDK / X3DAUDIO / XAUDIO2FX / TOPOLOGICAL LOADER TESTS PASSED!\n");
    return 0;
}
