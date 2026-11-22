#pragma once

// Microsoft XAudio2 2.9 (XAudio2_9.dll) COM ABI definitions for OpenScarlett.
// Supports PCM16, IEEE Float32, and Xbox XMA2 (wFormatTag == 0x0166) streams.
// Self-contained (does not include <windows.h> or <xaudio2.h>).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(OPENSCARLETT_BUILDING_XAUDIO2)
#    define OPENSCARLETT_XAUDIO2_API __declspec(dllexport)
#  else
#    define OPENSCARLETT_XAUDIO2_API __declspec(dllimport)
#  endif
#  define OPENSCARLETT_XAUDIO2_CALL __stdcall
#else
#  define OPENSCARLETT_XAUDIO2_API __attribute__((visibility("default")))
#  define OPENSCARLETT_XAUDIO2_CALL
#endif

// Standard XAudio2 HRESULT codes.
constexpr int32_t OS_XAUDIO2_S_OK = 0x00000000;
constexpr int32_t OS_XAUDIO2_E_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t OS_XAUDIO2_E_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t OS_XAUDIO2_E_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t OS_XAUDIO2_E_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t OS_XAUDIO2_E_INVALID_CALL = static_cast<int32_t>(0x88960001u);
constexpr int32_t OS_XAUDIO2_E_XMA_DECODER_ERROR = static_cast<int32_t>(0x88960002u);
constexpr int32_t OS_XAUDIO2_E_DEVICE_INVALIDATED = static_cast<int32_t>(0x88960004u);

constexpr uint16_t OS_WAVE_FORMAT_PCM = 0x0001u;
constexpr uint16_t OS_WAVE_FORMAT_IEEE_FLOAT = 0x0003u;
constexpr uint16_t OS_WAVE_FORMAT_IMA_ADPCM = 0x0011u;
constexpr uint16_t OS_WAVE_FORMAT_XBOX_ADPCM = 0x0069u;
constexpr uint16_t OS_WAVE_FORMAT_XMA2 = 0x0166u;
constexpr uint16_t OS_WAVE_FORMAT_OPUS = 0x704Fu;
constexpr uint16_t OS_WAVE_FORMAT_EXTENSIBLE = 0xFFFEu;

constexpr uint32_t OS_XAUDIO2_DEFAULT_CHANNELS = 0u;
constexpr uint32_t OS_XAUDIO2_DEFAULT_SAMPLERATE = 0u;
constexpr uint32_t OS_XAUDIO2_DEFAULT_PROCESSOR = 0x00000001u;
constexpr uint32_t OS_XAUDIO2_COMMIT_NOW = 0u;
constexpr uint32_t OS_XAUDIO2_END_OF_STREAM = 0x0040u;
constexpr uint32_t OS_XAUDIO2_LOOP_INFINITE = 255u;

struct XAudio2Guid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
static_assert(sizeof(XAudio2Guid) == 16, "XAudio2Guid must be 16 bytes");

constexpr XAudio2Guid IID_OS_XAudio2_IUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr XAudio2Guid IID_OS_IXAudio2 = {
    0x2B02E3CFu, 0x2E0Bu, 0x4EC3u, {0xBE, 0x45, 0x1B, 0x2A, 0x3F, 0xE7, 0x21, 0x0D}};
constexpr XAudio2Guid IID_OS_IXAPO = {
    0xA410B984u, 0x9839u, 0x4819u, {0xA0, 0xBE, 0x28, 0x56, 0xAE, 0x6B, 0x3A, 0xDB}};
constexpr XAudio2Guid IID_OS_IXAPOParameters = {
    0x26D95C66u, 0x80F2u, 0x499Au, {0xAD, 0x54, 0x5A, 0xE7, 0xF4, 0x3C, 0x6F, 0xC0}};

#pragma pack(push, 1)
struct OS_WAVEFORMATEX {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
};
static_assert(sizeof(OS_WAVEFORMATEX) == 18, "OS_WAVEFORMATEX must match 18-byte Windows ABI");

struct OS_WAVEFORMATEXTENSIBLE {
    OS_WAVEFORMATEX Format;
    uint16_t wValidBitsPerSample;
    uint32_t dwChannelMask;
    XAudio2Guid SubFormat;
};
static_assert(sizeof(OS_WAVEFORMATEXTENSIBLE) == 40,
              "OS_WAVEFORMATEXTENSIBLE must match 40-byte Windows ABI");

struct OS_XMA2WAVEFORMATEX {
    OS_WAVEFORMATEX wfx;
    uint16_t NumStreams;
    uint32_t ChannelMask;
    uint32_t SamplesEncoded;
    uint32_t BytesPerBlock;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint8_t LoopCount;
    uint8_t EncoderVersion;
    uint16_t BlockCount;
};
static_assert(sizeof(OS_XMA2WAVEFORMATEX) == 52,
              "OS_XMA2WAVEFORMATEX must match 52-byte Xbox XMA2WAVEFORMATEX ABI");

struct OS_IMAADPCMWAVEFORMAT {
    OS_WAVEFORMATEX wfx;
    uint16_t wSamplesPerBlock;
};
static_assert(sizeof(OS_IMAADPCMWAVEFORMAT) == 20,
              "OS_IMAADPCMWAVEFORMAT must match 20-byte Windows IMA-ADPCM ABI");

struct OS_OPUSWAVEFORMAT {
    OS_WAVEFORMATEX wfx;
    uint16_t PreSkip;
    uint16_t Version;
};
static_assert(sizeof(OS_OPUSWAVEFORMAT) == 22,
              "OS_OPUSWAVEFORMAT must match 22-byte Xbox Opus ABI");
#pragma pack(pop)

struct OS_XAUDIO2_BUFFER {
    uint32_t Flags;
    uint32_t AudioBytes;
    const uint8_t* pAudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    void* pContext;
};

struct OS_XAUDIO2_BUFFER_WMA {
    const uint32_t* pDecodedPacketCumulativeBytes;
    uint32_t PacketCount;
};

struct OS_XAUDIO2_VOICE_STATE {
    void* pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};

struct OS_XAUDIO2_VOICE_DETAILS {
    uint32_t CreationFlags;
    uint32_t ActiveFlags;
    uint32_t InputChannels;
    uint32_t InputSampleRate;
};

struct IXAudio2Voice;

struct OS_XAUDIO2_SEND_DESCRIPTOR {
    uint32_t Flags;
    IXAudio2Voice* pOutputVoice;
};

struct OS_XAUDIO2_VOICE_SENDS {
    uint32_t SendCount;
    OS_XAUDIO2_SEND_DESCRIPTOR* pSends;
};

struct OS_XAUDIO2_PERFORMANCE_DATA {
    uint64_t AudioCyclesSinceLastQuery;
    uint64_t TotalCyclesSinceLastQuery;
    uint32_t MinimumCyclesPerQuantum;
    uint32_t MaximumCyclesPerQuantum;
    uint32_t MemoryUsageInBytes;
    uint32_t CurrentLatencyInSamples;
    uint32_t GlitchesSinceEngineStarted;
    uint32_t ActiveSourceVoiceCount;
    uint32_t TotalSourceVoiceCount;
    uint32_t ActiveSubmixVoiceCount;
    uint32_t ActiveResamplerCount;
    uint32_t ActiveMatrixMixCount;
    uint32_t ActiveXmaSourceVoices;
    uint32_t ActiveXmaStreams;
};

struct OS_XAUDIO2_DEBUG_CONFIGURATION {
    uint32_t TraceMask;
    uint32_t BreakMask;
    int32_t LogThreadID;
    int32_t LogFileline;
    int32_t LogFunctionName;
    int32_t LogTiming;
};

struct IXAudio2VoiceCallback {
    virtual void OPENSCARLETT_XAUDIO2_CALL OnVoiceProcessingPassStart(uint32_t BytesRequired) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnVoiceProcessingPassEnd() = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnStreamEnd() = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnBufferStart(void* pBufferContext) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnBufferEnd(void* pBufferContext) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnLoopEnd(void* pBufferContext) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnVoiceError(void* pBufferContext, int32_t Error) = 0;
};

struct IXAudio2EngineCallback {
    virtual void OPENSCARLETT_XAUDIO2_CALL OnProcessingPassStart() = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnProcessingPassEnd() = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL OnCriticalError(int32_t Error) = 0;
};

struct IXAudio2Voice {
    virtual void OPENSCARLETT_XAUDIO2_CALL GetVoiceDetails(
        OS_XAUDIO2_VOICE_DETAILS* pVoiceDetails) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetOutputVoices(
        const OS_XAUDIO2_VOICE_SENDS* pSendList) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetVolume(
        float Volume, uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetVolume(float* pVolume) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetChannelVolumes(
        uint32_t Channels,
        const float* pVolumes,
        uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetChannelVolumes(
        uint32_t Channels, float* pVolumes) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL DestroyVoice() = 0;
};

struct IXAudio2SourceVoice : public IXAudio2Voice {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Start(
        uint32_t Flags = 0, uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Stop(
        uint32_t Flags = 0, uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SubmitSourceBuffer(
        const OS_XAUDIO2_BUFFER* pBuffer,
        const OS_XAUDIO2_BUFFER_WMA* pBufferWMA = nullptr) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL FlushSourceBuffers() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL Discontinuity() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL ExitLoop(
        uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetState(
        OS_XAUDIO2_VOICE_STATE* pVoiceState, uint32_t Flags = 0) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetFrequencyRatio(
        float Ratio, uint32_t OperationSet = OS_XAUDIO2_COMMIT_NOW) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetFrequencyRatio(float* pRatio) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL SetSourceSampleRate(uint32_t NewSourceSampleRate) = 0;
};

struct IXAudio2SubmixVoice : public IXAudio2Voice {};

struct IXAudio2MasteringVoice : public IXAudio2Voice {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL GetChannelMask(uint32_t* pChannelMask) = 0;
};

struct IXAudio2 {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_XAUDIO2_CALL Release() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL RegisterForCallbacks(
        IXAudio2EngineCallback* pCallback) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL UnregisterForCallbacks(
        IXAudio2EngineCallback* pCallback) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL CreateSourceVoice(
        IXAudio2SourceVoice** ppSourceVoice,
        const OS_WAVEFORMATEX* pSourceFormat,
        uint32_t Flags = 0,
        float MaxFrequencyRatio = 2.0f,
        IXAudio2VoiceCallback* pCallback = nullptr,
        const OS_XAUDIO2_VOICE_SENDS* pSendList = nullptr,
        const void* pEffectChain = nullptr) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL CreateSubmixVoice(
        IXAudio2SubmixVoice** ppSubmixVoice,
        uint32_t InputChannels,
        uint32_t InputSampleRate,
        uint32_t Flags = 0,
        uint32_t ProcessingStage = 0,
        const OS_XAUDIO2_VOICE_SENDS* pSendList = nullptr,
        const void* pEffectChain = nullptr) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL CreateMasteringVoice(
        IXAudio2MasteringVoice** ppMasteringVoice,
        uint32_t InputChannels = OS_XAUDIO2_DEFAULT_CHANNELS,
        uint32_t InputSampleRate = OS_XAUDIO2_DEFAULT_SAMPLERATE,
        uint32_t Flags = 0,
        const wchar_t* szDeviceId = nullptr,
        const void* pEffectChain = nullptr,
        uint32_t StreamCategory = 0) = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL StartEngine() = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL StopEngine() = 0;
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL CommitChanges(uint32_t OperationSet) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetPerformanceData(
        OS_XAUDIO2_PERFORMANCE_DATA* pPerfData) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL SetDebugConfiguration(
        const OS_XAUDIO2_DEBUG_CONFIGURATION* pDebugConfiguration,
        void* pReserved = nullptr) = 0;
};

// ---------------------------------------------------------------------------
// X3DAudio (3D Spatial Audio ABI matching XAudio2_9.dll)
// ---------------------------------------------------------------------------
constexpr float OS_X3DAUDIO_SPEED_OF_SOUND = 343.5f;
constexpr float OS_X3DAUDIO_PI = 3.141592654f;

constexpr uint32_t OS_SPEAKER_FRONT_LEFT = 0x00000001u;
constexpr uint32_t OS_SPEAKER_FRONT_RIGHT = 0x00000002u;
constexpr uint32_t OS_SPEAKER_FRONT_CENTER = 0x00000004u;
constexpr uint32_t OS_SPEAKER_LOW_FREQUENCY = 0x00000008u;
constexpr uint32_t OS_SPEAKER_BACK_LEFT = 0x00000010u;
constexpr uint32_t OS_SPEAKER_BACK_RIGHT = 0x00000020u;
constexpr uint32_t OS_SPEAKER_SIDE_LEFT = 0x00000200u;
constexpr uint32_t OS_SPEAKER_SIDE_RIGHT = 0x00000400u;

constexpr uint32_t OS_SPEAKER_MONO = OS_SPEAKER_FRONT_CENTER;
constexpr uint32_t OS_SPEAKER_STEREO = OS_SPEAKER_FRONT_LEFT | OS_SPEAKER_FRONT_RIGHT;
constexpr uint32_t OS_SPEAKER_QUAD =
    OS_SPEAKER_FRONT_LEFT | OS_SPEAKER_FRONT_RIGHT |
    OS_SPEAKER_BACK_LEFT | OS_SPEAKER_BACK_RIGHT;
constexpr uint32_t OS_SPEAKER_5POINT1 =
    OS_SPEAKER_FRONT_LEFT | OS_SPEAKER_FRONT_RIGHT | OS_SPEAKER_FRONT_CENTER |
    OS_SPEAKER_LOW_FREQUENCY | OS_SPEAKER_BACK_LEFT | OS_SPEAKER_BACK_RIGHT;
constexpr uint32_t OS_SPEAKER_5POINT1_SURROUND =
    OS_SPEAKER_FRONT_LEFT | OS_SPEAKER_FRONT_RIGHT | OS_SPEAKER_FRONT_CENTER |
    OS_SPEAKER_LOW_FREQUENCY | OS_SPEAKER_SIDE_LEFT | OS_SPEAKER_SIDE_RIGHT;
constexpr uint32_t OS_SPEAKER_7POINT1_SURROUND =
    OS_SPEAKER_FRONT_LEFT | OS_SPEAKER_FRONT_RIGHT | OS_SPEAKER_FRONT_CENTER |
    OS_SPEAKER_LOW_FREQUENCY | OS_SPEAKER_BACK_LEFT | OS_SPEAKER_BACK_RIGHT |
    OS_SPEAKER_SIDE_LEFT | OS_SPEAKER_SIDE_RIGHT;

constexpr uint32_t OS_X3DAUDIO_CALCULATE_MATRIX = 0x00000001u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_DELAY = 0x00000002u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_LPF_DIRECT = 0x00000004u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_LPF_REVERB = 0x00000008u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_REVERB = 0x00000010u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_DOPPLER = 0x00000020u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_EMITTER_ANGLE = 0x00000040u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_ZEROCENTER = 0x00010000u;
constexpr uint32_t OS_X3DAUDIO_CALCULATE_REDIRECT_TO_LFE = 0x00020000u;

constexpr uint32_t OS_X3DAUDIO_HANDLE_BYTESIZE = 20u;
typedef uint8_t X3DAUDIO_HANDLE[OS_X3DAUDIO_HANDLE_BYTESIZE];
static_assert(sizeof(X3DAUDIO_HANDLE) == 20, "X3DAUDIO_HANDLE must be 20 bytes");

struct X3DAUDIO_VECTOR {
    float x;
    float y;
    float z;
};
static_assert(sizeof(X3DAUDIO_VECTOR) == 12, "X3DAUDIO_VECTOR must be 12 bytes");

struct X3DAUDIO_DISTANCE_CURVE_POINT {
    float Distance;
    float DSPSetting;
};

struct X3DAUDIO_DISTANCE_CURVE {
    X3DAUDIO_DISTANCE_CURVE_POINT* pPoints;
    uint32_t PointCount;
};

struct X3DAUDIO_CONE {
    float InnerAngle;
    float OuterAngle;
    float InnerVolume;
    float OuterVolume;
    float InnerLPF;
    float OuterLPF;
    float InnerReverb;
    float OuterReverb;
};

struct X3DAUDIO_LISTENER {
    X3DAUDIO_VECTOR OrientFront;
    X3DAUDIO_VECTOR OrientTop;
    X3DAUDIO_VECTOR Position;
    X3DAUDIO_VECTOR Velocity;
    X3DAUDIO_CONE* pCone;
};

struct X3DAUDIO_EMITTER {
    X3DAUDIO_CONE* pCone;
    X3DAUDIO_VECTOR OrientFront;
    X3DAUDIO_VECTOR OrientTop;
    X3DAUDIO_VECTOR Position;
    X3DAUDIO_VECTOR Velocity;
    float InnerRadius;
    float InnerRadiusAngle;
    uint32_t ChannelCount;
    float ChannelRadius;
    float* pChannelAzimuths;
    X3DAUDIO_DISTANCE_CURVE* pVolumeCurve;
    X3DAUDIO_DISTANCE_CURVE* pLFECurve;
    X3DAUDIO_DISTANCE_CURVE* pLPFDirectCurve;
    X3DAUDIO_DISTANCE_CURVE* pLPFReverbCurve;
    X3DAUDIO_DISTANCE_CURVE* pReverbCurve;
    float CurveDistanceScaler;
    float DopplerScaler;
};

struct X3DAUDIO_DSP_SETTINGS {
    float* pMatrixCoefficients;
    float* pDelayTimes;
    uint32_t SrcChannelCount;
    uint32_t DstChannelCount;
    float LPFDirectCoefficient;
    float LPFReverbCoefficient;
    float ReverbLevel;
    float DopplerFactor;
    float EmitterToListenerAngle;
    float EmitterToListenerDistance;
    float EmitterVelocityComponent;
    float ListenerVelocityComponent;
};

// ---------------------------------------------------------------------------
// XAudio2FX (Reverb & VolumeMeter COM APOs)
// ---------------------------------------------------------------------------
struct OS_IXAudio2Unknown {
    virtual int32_t OPENSCARLETT_XAUDIO2_CALL QueryInterface(
        const XAudio2Guid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_XAUDIO2_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_XAUDIO2_CALL Release() = 0;
};

struct OS_IXAPOParameters : public OS_IXAudio2Unknown {
    virtual void OPENSCARLETT_XAUDIO2_CALL SetParameters(
        const void* pParameters, uint32_t ParameterByteSize) = 0;
    virtual void OPENSCARLETT_XAUDIO2_CALL GetParameters(
        void* pParameters, uint32_t ParameterByteSize) = 0;
};

#pragma pack(push, 1)
struct OS_XAUDIO2FX_REVERB_PARAMETERS {
    float WetDryMix;
    uint32_t ReflectionsDelay;
    uint8_t ReverbDelay;
    uint8_t RearDelay;
    uint8_t SideDelay;
    uint8_t PositionLeft;
    uint8_t PositionRight;
    uint8_t PositionMatrixLeft;
    uint8_t PositionMatrixRight;
    uint8_t EarlyDiffusion;
    uint8_t LateDiffusion;
    uint8_t LowEQGain;
    uint8_t LowEQCutoff;
    uint8_t HighEQGain;
    uint8_t HighEQCutoff;
    float RoomFilterFreq;
    float RoomFilterMain;
    float RoomFilterHF;
    float ReflectionsGain;
    float ReverbGain;
    float DecayTime;
    float Density;
    float RoomSize;
    int32_t DisableLateField;
};
#pragma pack(pop)

struct OS_XAUDIO2FX_VOLUMEMETER_LEVELS {
    float* pPeakLevels;
    float* pRMSLevels;
    uint32_t ChannelCount;
};

#ifdef __cplusplus
extern "C" {
#endif

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2Create(
    IXAudio2** ppXAudio2,
    uint32_t Flags,
    uint32_t XAudio2Processor);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateWithVersionInfo(
    IXAudio2** ppXAudio2,
    uint32_t Flags,
    uint32_t XAudio2Processor,
    uint32_t ntddiVersion);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL X3DAudioInitialize(
    uint32_t SpeakerChannelMask,
    float SpeedOfSound,
    X3DAUDIO_HANDLE Instance);

OPENSCARLETT_XAUDIO2_API void OPENSCARLETT_XAUDIO2_CALL X3DAudioCalculate(
    const X3DAUDIO_HANDLE Instance,
    const X3DAUDIO_LISTENER* pListener,
    const X3DAUDIO_EMITTER* pEmitter,
    uint32_t Flags,
    X3DAUDIO_DSP_SETTINGS* pDSPSettings);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateReverb(
    OS_IXAudio2Unknown** ppApo,
    uint32_t Flags);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL XAudio2CreateVolumeMeter(
    OS_IXAudio2Unknown** ppApo,
    uint32_t Flags);

// Deterministic mixer readback helper for headless / automated verification.
// Renders `frame_count` interleaved float32 frames (frame_count * master_channels floats).
OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2RenderMixForTest(
    IXAudio2* pXAudio2,
    float* out_frames,
    uint32_t frame_count);

// Diagnostic backend status snapshot (WASAPI active vs null audio, sample rate, rendered frames, Opus MFT).
struct OS_XAUDIO2_BACKEND_INFO {
    uint32_t null_audio_forced;
    uint32_t wasapi_active;
    uint32_t wasapi_event_driven;
    uint32_t wasapi_sample_rate;
    uint32_t wasapi_channels;
    uint32_t wasapi_bits_per_sample;
    uint32_t wasapi_buffer_frames;
    uint64_t wasapi_frames_rendered;
    int32_t wasapi_last_hresult;
    uint32_t opus_mft_active_voices;
};

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2GetBackendInfo(
    IXAudio2* pXAudio2,
    OS_XAUDIO2_BACKEND_INFO* out_info);

// WASAPI MMDevice endpoint ID validation, normalization, and probing helpers.
// Accepts NULL/empty/"default", canonical IMMDevice IDs "{0.0.0.00000000}.{guid}",
// WinRT/PnP device paths "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface-guid}",
// and bare "{guid}" strings (both wide and narrow).
struct OS_XAUDIO2_ENDPOINT_PROBE {
    uint32_t format_valid;
    uint32_t is_default;
    uint32_t exact_device_found;
    uint32_t fallback_used;
    int32_t wasapi_hresult;
    uint32_t mix_sample_rate;
    uint32_t mix_channels;
    uint32_t mix_bits_per_sample;
    wchar_t canonical_id[256];
};

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ValidateEndpointIdW(
    const wchar_t* deviceId,
    wchar_t* outCanonicalId,
    uint32_t outCanonicalChars);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ValidateEndpointIdA(
    const char* deviceId,
    char* outCanonicalId,
    uint32_t outCanonicalChars);

OPENSCARLETT_XAUDIO2_API int32_t OPENSCARLETT_XAUDIO2_CALL OpenScarlettXAudio2ProbeWasapiEndpointW(
    const wchar_t* deviceId,
    OS_XAUDIO2_ENDPOINT_PROBE* outProbe);

#ifdef __cplusplus
}
#endif
