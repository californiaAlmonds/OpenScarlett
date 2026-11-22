// MIT Microsoft Media Foundation Platform (mfplat_xs.dll) shim for OpenScarlett.
// Implements:
//  - IMFDXGIDeviceManager wrapper supporting both host D3D11/DXGI and Xbox D3D12 devices
//    (ResetDevice, OpenDeviceHandle, CloseDeviceHandle, LockDevice, UnlockDevice, TestDevice, GetVideoService)
//  - MFResetDXGIDeviceManagerX (Xbox Series X|S GDK extension)
//  - MFCreateMediaEngineClassFactory / IMFMediaEngineClassFactory / IMFMediaEngine / IMFMediaEngineEx
//    with exact event sequence: LOADSTART -> LOADEDMETADATA -> CANPLAY -> PLAYING -> TIMEUPDATE -> ENDED,
//    real D3D12/DXGI/IMFMediaBuffer TransferVideoFrame, and deterministic headless/CI NV12/RGBA synthesis
//  - MFTEnumEx with H.264 (MFVideoFormat_H264) and HEVC (MFVideoFormat_HEVC) decoder fallback activates
//  - Portable across MSVC, Clang-CL, and MinGW-w64 (zero __uuidof, zero mfuuid.lib/oleaut32.lib deps).

#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi.h>
#  include <mfobjects.h>
#  include <mftransform.h>
#  include <objbase.h>
#endif

#if defined(_WIN32)
#  define OS_MFPLAT_API __declspec(dllexport)
#  define OS_MFPLAT_CALL __stdcall
#else
#  define OS_MFPLAT_API __attribute__((visibility("default")))
#  define OS_MFPLAT_CALL
#endif

namespace {

constexpr const char* kModName = "mfplat_xs.dll";

constexpr int32_t kHrOk = 0x00000000;
constexpr int32_t kHrFalse = 0x00000001;
constexpr int32_t kHrNotImpl = static_cast<int32_t>(0x80004001u);
constexpr int32_t kHrNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kHrPointer = static_cast<int32_t>(0x80004003u);
[[maybe_unused]] constexpr int32_t kHrFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kHrHandle = static_cast<int32_t>(0x80070006u);
constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHrDxgiNewVideoDevice = static_cast<int32_t>(0xC00D4E88u);  // MF_E_DXGI_NEW_VIDEO_DEVICE
constexpr int32_t kHrDxgiStillDrawing = static_cast<int32_t>(0x887A000Au);    // DXGI_ERROR_WAS_STILL_DRAWING

inline int32_t RecordHr(const char* sym, int32_t hr) {
    openscarlett::CallOutcome outcome = openscarlett::CallOutcome::Ok;
    if (hr == kHrNotImpl) {
        outcome = openscarlett::CallOutcome::NotImplemented;
    } else if (hr < 0) {
        outcome = openscarlett::CallOutcome::Error;
    }
    openscarlett::CallTelemetry::Global().Record(kModName, sym, outcome, hr, -1);
    return hr;
}

inline void RecordOk(const char* sym) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, -1);
}

#if defined(_WIN32)

// ===========================================================================
// Inline Standard COM / Media Foundation / DXGI / D3D12 GUIDs (No __uuidof)
// ===========================================================================
inline bool GuidEq(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

constexpr GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidImfDxgiDeviceManager = {
    0xEB533D5Du, 0x2DB6u, 0x40F8u, {0x97, 0xA9, 0x49, 0x46, 0x92, 0x01, 0x4F, 0x07}};
constexpr GUID kIidImfAttributes = {
    0x2CD2D921u, 0xC447u, 0x44A7u, {0xA1, 0x3Cu, 0x4A, 0xDA, 0xBF, 0xC2, 0x47, 0xE3}};
constexpr GUID kIidImfActivate = {
    0x7FEE9E9Au, 0x4A89u, 0x47A6u, {0x89, 0x9Cu, 0xB6, 0xA5, 0x3A, 0x70, 0xFB, 0x67}};
constexpr GUID kIidImfMediaBuffer = {
    0x045FA593u, 0x8799u, 0x42B8u, {0xBC, 0x8Du, 0x89, 0x68, 0xC6, 0x45, 0x35, 0x07}};
constexpr GUID kIidId3d12Device = {
    0x189819F1u, 0x1DB6u, 0x4B57u, {0xBE, 0x54u, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
constexpr GUID kIidId3d12Resource = {
    0x696442BEu, 0xA72Eu, 0x4059u, {0xBC, 0x79u, 0x5B, 0x5C, 0x98, 0x04, 0x0F, 0xAD}};
constexpr GUID kIidId3d12CommandQueue = {
    0x0EC870A6u, 0x5D7Eu, 0x4C22u, {0x8C, 0xFCu, 0x5B, 0xAA, 0xE0, 0x76, 0x16, 0xED}};
constexpr GUID kIidId3d12CommandAllocator = {
    0x6102DEE4u, 0xAF59u, 0x4B09u, {0xB9, 0x99u, 0xB4, 0x4D, 0x73, 0xF0, 0x9B, 0x24}};
constexpr GUID kIidId3d12GraphicsCommandList = {
    0x5B160D0Fu, 0xAC1Bu, 0x4185u, {0x8B, 0xA8u, 0xB3, 0xAE, 0x42, 0xA5, 0xA4, 0x55}};
constexpr GUID kIidId3d12Fence = {
    0x0A753DCFu, 0xC4D8u, 0x4B91u, {0xAD, 0xF6u, 0xBE, 0x5A, 0x60, 0xD9, 0x5A, 0x76}};
constexpr GUID kIidIdxgiSurface = {
    0xCAFCB56Cu, 0x6AC3u, 0x4889u, {0xBF, 0x47u, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC}};

// MediaEngine IIDs & Attribute GUIDs (exact Windows SDK mfmediaengine.h values)
constexpr GUID kIidImfMediaError = {
    0xFC0E10D2u, 0xAB2Au, 0x4501u, {0xA9, 0x51, 0x06, 0xBB, 0x10, 0x75, 0x18, 0x4C}};
constexpr GUID kIidImfMediaTimeRange = {
    0xDB71A2FCu, 0x078Au, 0x414Eu, {0x9D, 0xF9, 0x8C, 0x25, 0x31, 0xB0, 0xAA, 0x6C}};
constexpr GUID kIidImfMediaEngineNotify = {
    0xFEE7C112u, 0xE776u, 0x42B5u, {0x9B, 0xBF, 0x00, 0x48, 0x52, 0x4E, 0x2B, 0xD5}};
constexpr GUID kIidImfMediaEngine = {
    0x98A1B0BBu, 0x03EBu, 0x4935u, {0xAE, 0x7C, 0x93, 0xC1, 0xFA, 0x0E, 0x1C, 0x93}};
constexpr GUID kIidImfMediaEngineEx = {
    0x83015EADu, 0xB1E6u, 0x40D0u, {0xA9, 0x8A, 0x37, 0x14, 0x5F, 0xFE, 0x1A, 0xD1}};
constexpr GUID kIidImfMediaEngineClassFactory = {
    0x4D645ACEu, 0x26AAu, 0x4688u, {0x9B, 0xE1, 0xDF, 0x35, 0x16, 0x99, 0x0B, 0x93}};

constexpr GUID kAttrMediaEngineCallback = {
    0xC60381B8u, 0x83A4u, 0x41F8u, {0xA3, 0xD0, 0xDE, 0x05, 0x07, 0x68, 0x49, 0xA9}};
constexpr GUID kAttrMediaEngineDxgiManager = {
    0x065702DAu, 0x1094u, 0x486Du, {0x86, 0x17, 0xEE, 0x7C, 0xC4, 0xEE, 0x46, 0x48}};
constexpr GUID kAttrMediaEngineVideoOutputFormat = {
    0x5066893Cu, 0x8CF9u, 0x42BCu, {0x8B, 0x8A, 0x47, 0x22, 0x12, 0xE5, 0x27, 0x26}};

// Video MFT category & format subtypes
constexpr GUID kMftCategoryVideoDecoder = {
    0xD6C02D4Bu, 0x6833u, 0x45B4u, {0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91}};
constexpr GUID kMfVideoFormatH264 = {
    0x34363248u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMfVideoFormatHevc = {
    0x43564548u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMfVideoFormatNv12 = {
    0x3231564Eu, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMfVideoFormatArgb32 = {
    0x00000015u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMftFriendlyNameAttr = {
    0x314FFBAEu, 0x5B41u, 0x4C95u, {0x9C, 0x19, 0x4E, 0x7D, 0x58, 0x6F, 0xAC, 0xE3}};
constexpr GUID kMftTransformClsidAttr = {
    0x6821C42Bu, 0x65A4u, 0x4E82u, {0x99, 0xBC, 0x9A, 0x88, 0x20, 0x5E, 0xCD, 0x0C}};
constexpr GUID kClsidOsH264Decoder = {
    0x5CA02640u, 0x0001u, 0x4E00u, {0x80, 0x01, 0x48, 0x32, 0x36, 0x34, 0x00, 0x01}};
constexpr GUID kClsidOsHevcDecoder = {
    0x5CA02650u, 0x0001u, 0x4E00u, {0x80, 0x01, 0x48, 0x45, 0x56, 0x43, 0x00, 0x01}};

// Dynamic BSTR allocator (avoids linking oleaut32.lib on MinGW/MSVC)
using PfnSysAllocString = BSTR(WINAPI*)(const OLECHAR*);

BSTR AllocBstrPortable(const wchar_t* s) {
    if (!s) return nullptr;
    static PfnSysAllocString s_alloc = []() -> PfnSysAllocString {
        HMODULE mod = LoadLibraryW(L"oleaut32.dll");
        return mod ? reinterpret_cast<PfnSysAllocString>(
                         reinterpret_cast<void*>(GetProcAddress(mod, "SysAllocString")))
                   : nullptr;
    }();
    if (s_alloc) {
        return s_alloc(s);
    }
    const size_t len = std::wcslen(s);
    auto* raw = static_cast<uint8_t*>(CoTaskMemAlloc(sizeof(uint32_t) + (len + 1u) * sizeof(wchar_t)));
    if (!raw) return nullptr;
    const uint32_t byte_len = static_cast<uint32_t>(len * sizeof(wchar_t));
    std::memcpy(raw, &byte_len, sizeof(byte_len));
    auto* str = reinterpret_cast<wchar_t*>(raw + sizeof(uint32_t));
    std::memcpy(str, s, (len + 1u) * sizeof(wchar_t));
    return str;
}

// ===========================================================================
// Portable Self-Contained IMFMediaEngine / IMFMediaEngineEx ABI Interfaces
// (Matches Windows SDK mfmediaengine.h vtable layouts slot-for-slot)
// ===========================================================================
enum OsMediaEngineEvent : DWORD {
    kEvLoadStart = 1,
    kEvProgress = 2,
    kEvSuspend = 3,
    kEvAbort = 4,
    kEvError = 5,
    kEvEmptied = 6,
    kEvStalled = 7,
    kEvPlay = 8,
    kEvPause = 9,
    kEvLoadedMetadata = 10,
    kEvLoadedData = 11,
    kEvWaiting = 12,
    kEvPlaying = 13,
    kEvCanPlay = 14,
    kEvCanPlayThrough = 15,
    kEvSeeking = 16,
    kEvSeeked = 17,
    kEvTimeUpdate = 18,
    kEvEnded = 19,
    kEvRateChange = 20,
    kEvDurationChange = 21,
    kEvVolumeChange = 22,
    kEvFirstFrameReady = 1009,
};

struct OsMFVideoNormalizedRect {
    float left;
    float top;
    float right;
    float bottom;
};

struct OsIMFMediaError : public IUnknown {
    virtual USHORT STDMETHODCALLTYPE GetErrorCode(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetExtendedErrorCode(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetErrorCode(uint32_t error) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetExtendedErrorCode(HRESULT error) = 0;
};

struct OsIMFMediaTimeRange : public IUnknown {
    virtual DWORD STDMETHODCALLTYPE GetLength(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStart(DWORD index, double* pStart) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEnd(DWORD index, double* pEnd) = 0;
    virtual BOOL STDMETHODCALLTYPE ContainsTime(double time) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRange(double startTime, double endTime) = 0;
    virtual HRESULT STDMETHODCALLTYPE Clear(void) = 0;
};

struct OsIMFMediaEngineNotify : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EventNotify(
        DWORD event, DWORD_PTR param1, DWORD param2) = 0;
};

struct OsIMFMediaEngineSrcElements : public IUnknown {
    virtual DWORD STDMETHODCALLTYPE GetLength(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetURL(DWORD index, BSTR* pURL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetType(DWORD index, BSTR* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMedia(DWORD index, BSTR* pMedia) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddElement(BSTR pURL, BSTR pType, BSTR pMedia) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAllElements(void) = 0;
};

#if defined(GetCurrentTime)
#  undef GetCurrentTime
#endif

struct OsIMFMediaEngine : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetError(OsIMFMediaError** ppError) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetErrorCode(uint32_t error) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSourceElements(OsIMFMediaEngineSrcElements* pSrcElements) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSource(BSTR pUrl) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSource(BSTR* ppUrl) = 0;
    virtual USHORT STDMETHODCALLTYPE GetNetworkState(void) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetPreload(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreload(uint32_t Preload) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffered(OsIMFMediaTimeRange** ppBuffered) = 0;
    virtual HRESULT STDMETHODCALLTYPE Load(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanPlayType(BSTR type, uint32_t* pAnswer) = 0;
    virtual USHORT STDMETHODCALLTYPE GetReadyState(void) = 0;
    virtual BOOL STDMETHODCALLTYPE IsSeeking(void) = 0;
    virtual double STDMETHODCALLTYPE GetCurrentTime(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentTime(double seekTime) = 0;
    virtual double STDMETHODCALLTYPE GetStartTime(void) = 0;
    virtual double STDMETHODCALLTYPE GetDuration(void) = 0;
    virtual BOOL STDMETHODCALLTYPE IsPaused(void) = 0;
    virtual double STDMETHODCALLTYPE GetDefaultPlaybackRate(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultPlaybackRate(double Rate) = 0;
    virtual double STDMETHODCALLTYPE GetPlaybackRate(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPlaybackRate(double Rate) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlayed(OsIMFMediaTimeRange** ppPlayed) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSeekable(OsIMFMediaTimeRange** ppSeekable) = 0;
    virtual BOOL STDMETHODCALLTYPE IsEnded(void) = 0;
    virtual BOOL STDMETHODCALLTYPE GetAutoPlay(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAutoPlay(BOOL AutoPlay) = 0;
    virtual BOOL STDMETHODCALLTYPE GetLoop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLoop(BOOL Loop) = 0;
    virtual HRESULT STDMETHODCALLTYPE Play(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause(void) = 0;
    virtual BOOL STDMETHODCALLTYPE GetMuted(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMuted(BOOL Muted) = 0;
    virtual double STDMETHODCALLTYPE GetVolume(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(double Volume) = 0;
    virtual BOOL STDMETHODCALLTYPE HasVideo(void) = 0;
    virtual BOOL STDMETHODCALLTYPE HasAudio(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNativeVideoSize(DWORD* cx, DWORD* cy) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVideoAspectRatio(DWORD* cx, DWORD* cy) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE TransferVideoFrame(
        IUnknown* pDstSurf,
        const OsMFVideoNormalizedRect* pSrc,
        const RECT* pDst,
        const MFARGB* pBorderClr) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnVideoStreamTick(LONGLONG* pPts) = 0;
};

struct OsIMFMediaEngineEx : public OsIMFMediaEngine {
    virtual HRESULT STDMETHODCALLTYPE SetSourceFromByteStream(
        IMFByteStream* pByteStream, BSTR pURL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatistics(
        uint32_t StatisticID, PROPVARIANT* pStatistic) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateVideoStream(
        const OsMFVideoNormalizedRect* pSrc,
        const RECT* pDst,
        const MFARGB* pBorderClr) = 0;
    virtual double STDMETHODCALLTYPE GetBalance(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBalance(double balance) = 0;
    virtual BOOL STDMETHODCALLTYPE IsPlaybackRateSupported(double rate) = 0;
    virtual HRESULT STDMETHODCALLTYPE FrameStep(BOOL Forward) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResourceCharacteristics(DWORD* pCharacteristics) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPresentationAttribute(
        REFGUID guidMFAttribute, PROPVARIANT* pvValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfStreams(DWORD* pdwStreamCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamAttribute(
        DWORD dwStreamIndex, REFGUID guidMFAttribute, PROPVARIANT* pvValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamSelection(DWORD dwStreamIndex, BOOL* pEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStreamSelection(DWORD dwStreamIndex, BOOL Enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE ApplyStreamSelections(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsProtected(BOOL* pProtected) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertVideoEffect(IUnknown* pEffect, BOOL fOptional) = 0;
    virtual HRESULT STDMETHODCALLTYPE InsertAudioEffect(IUnknown* pEffect, BOOL fOptional) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveAllEffects(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTimelineMarkerTimer(double timeToFire) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineMarkerTimer(double* pTimeToFire) = 0;
    virtual HRESULT STDMETHODCALLTYPE CancelTimelineMarkerTimer(void) = 0;
    virtual BOOL STDMETHODCALLTYPE IsStereo3D(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStereo3DFramePackingMode(uint32_t* packMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStereo3DFramePackingMode(uint32_t packMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStereo3DRenderMode(uint32_t* outputType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStereo3DRenderMode(uint32_t outputType) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableWindowlessSwapchainMode(BOOL fEnable) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVideoSwapchainHandle(HANDLE* phSwapchain) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableHorizontalMirrorMode(BOOL fEnable) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAudioStreamCategory(UINT32* pCategory) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAudioStreamCategory(UINT32 category) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAudioEndpointRole(UINT32* pRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAudioEndpointRole(UINT32 role) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRealTimeMode(BOOL* pfEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRealTimeMode(BOOL fEnable) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentTimeEx(double seekTime, uint32_t seekMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnableTimeUpdateTimer(BOOL fEnableTimer) = 0;
};

struct OsIMFMediaEngineClassFactory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(
        DWORD dwFlags, IMFAttributes* pAttr, OsIMFMediaEngine** ppPlayer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTimeRange(OsIMFMediaTimeRange** ppTimeRange) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateError(OsIMFMediaError** ppError) = 0;
};

// ===========================================================================
// Host mfplat.dll Dynamic Loader
// ===========================================================================
using PfnMFStartup = HRESULT(STDAPICALLTYPE*)(ULONG, DWORD);
using PfnMFShutdown = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFCreateDXGIDeviceManager = HRESULT(STDAPICALLTYPE*)(UINT*, IMFDXGIDeviceManager**);
using PfnMFCreateMediaType = HRESULT(STDAPICALLTYPE*)(IMFMediaType**);
using PfnMFCreateSample = HRESULT(STDAPICALLTYPE*)(IMFSample**);
using PfnMFCreateMemoryBuffer = HRESULT(STDAPICALLTYPE*)(DWORD, IMFMediaBuffer**);
using PfnMFCreateAttributes = HRESULT(STDAPICALLTYPE*)(IMFAttributes**, UINT32);
using PfnMFCreateAsyncResult = HRESULT(STDAPICALLTYPE*)(IUnknown*, IMFAsyncCallback*, IUnknown*, IMFAsyncResult**);
using PfnMFAllocateWorkQueue = HRESULT(STDAPICALLTYPE*)(DWORD*);
using PfnMFUnlockWorkQueue = HRESULT(STDAPICALLTYPE*)(DWORD);
using PfnMFLockPlatform = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFUnlockPlatform = HRESULT(STDAPICALLTYPE*)(void);
using PfnMFGetSystemTime = LONGLONG(STDAPICALLTYPE*)(void);
using PfnMFTEnumEx = HRESULT(STDAPICALLTYPE*)(
    GUID,
    UINT32,
    const MFT_REGISTER_TYPE_INFO*,
    const MFT_REGISTER_TYPE_INFO*,
    IMFActivate***,
    UINT32*);

struct HostMfPlat {
    std::mutex mutex;
    bool attempted = false;
    HMODULE mod = nullptr;

    PfnMFStartup mf_startup = nullptr;
    PfnMFShutdown mf_shutdown = nullptr;
    PfnMFCreateDXGIDeviceManager mf_create_dxgi_mgr = nullptr;
    PfnMFCreateMediaType mf_create_media_type = nullptr;
    PfnMFCreateSample mf_create_sample = nullptr;
    PfnMFCreateMemoryBuffer mf_create_memory_buffer = nullptr;
    PfnMFCreateAttributes mf_create_attributes = nullptr;
    PfnMFCreateAsyncResult mf_create_async_result = nullptr;
    PfnMFAllocateWorkQueue mf_allocate_wq = nullptr;
    PfnMFUnlockWorkQueue mf_unlock_wq = nullptr;
    PfnMFLockPlatform mf_lock_platform = nullptr;
    PfnMFUnlockPlatform mf_unlock_platform = nullptr;
    PfnMFGetSystemTime mf_get_system_time = nullptr;
    PfnMFTEnumEx mft_enum_ex = nullptr;

    std::atomic<uint32_t> fallback_token_gen{0x5CA01000u};
    std::atomic<uint32_t> fallback_wq_gen{0x4000u};
    std::atomic<uint32_t> startup_refcount{0u};
    std::unordered_set<uint32_t> fallback_wqs;

    void EnsureLoaded() {
        std::lock_guard<std::mutex> lk(mutex);
        if (attempted) return;
        attempted = true;

        wchar_t sys_dir[MAX_PATH]{};
        const UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
        if (len > 0u && len < MAX_PATH - 16u) {
            std::wstring full_path(sys_dir, len);
            if (!full_path.empty() && full_path.back() != L'\\') {
                full_path.push_back(L'\\');
            }
            full_path += L"mfplat.dll";
            mod = LoadLibraryW(full_path.c_str());
        }
        if (!mod) {
            mod = LoadLibraryW(L"mfplat.dll");
        }
        if (!mod) {
            return;
        }

        auto load = [&](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(mod, name));
        };
        mf_startup = reinterpret_cast<PfnMFStartup>(load("MFStartup"));
        mf_shutdown = reinterpret_cast<PfnMFShutdown>(load("MFShutdown"));
        mf_create_dxgi_mgr = reinterpret_cast<PfnMFCreateDXGIDeviceManager>(load("MFCreateDXGIDeviceManager"));
        mf_create_media_type = reinterpret_cast<PfnMFCreateMediaType>(load("MFCreateMediaType"));
        mf_create_sample = reinterpret_cast<PfnMFCreateSample>(load("MFCreateSample"));
        mf_create_memory_buffer = reinterpret_cast<PfnMFCreateMemoryBuffer>(load("MFCreateMemoryBuffer"));
        mf_create_attributes = reinterpret_cast<PfnMFCreateAttributes>(load("MFCreateAttributes"));
        mf_create_async_result = reinterpret_cast<PfnMFCreateAsyncResult>(load("MFCreateAsyncResult"));
        mf_allocate_wq = reinterpret_cast<PfnMFAllocateWorkQueue>(load("MFAllocateWorkQueue"));
        mf_unlock_wq = reinterpret_cast<PfnMFUnlockWorkQueue>(load("MFUnlockWorkQueue"));
        mf_lock_platform = reinterpret_cast<PfnMFLockPlatform>(load("MFLockPlatform"));
        mf_unlock_platform = reinterpret_cast<PfnMFUnlockPlatform>(load("MFUnlockPlatform"));
        mf_get_system_time = reinterpret_cast<PfnMFGetSystemTime>(load("MFGetSystemTime"));
        mft_enum_ex = reinterpret_cast<PfnMFTEnumEx>(load("MFTEnumEx"));
    }
};

HostMfPlat& GetHostMf() {
    static HostMfPlat* s = new HostMfPlat();
    return *s;
}

// ===========================================================================
// IMFDXGIDeviceManager Wrapper (Supports D3D12 + D3D11 + DXGI)
// ===========================================================================
class OpenScarlettDxgiDeviceManager final : public IMFDXGIDeviceManager {
public:
    OpenScarlettDxgiDeviceManager(UINT token, IMFDXGIDeviceManager* host_mgr, UINT host_token)
        : reset_token_(token), host_mgr_(host_mgr), host_token_(host_token) {}

    ~OpenScarlettDxgiDeviceManager() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (host_mgr_) {
            for (const auto& kv : handles_) {
                if (kv.second.host_handle) {
                    host_mgr_->CloseDeviceHandle(kv.second.host_handle);
                }
            }
            host_mgr_->Release();
            host_mgr_ = nullptr;
        }
        handles_.clear();
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfDxgiDeviceManager)) {
            AddRef();
            *ppvObject = static_cast<IMFDXGIDeviceManager*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    HRESULT STDMETHODCALLTYPE CloseDeviceHandle(HANDLE hDevice) override {
        if (!hDevice) return static_cast<HRESULT>(kHrHandle);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = handles_.find(hDevice);
        if (it == handles_.end()) {
            return static_cast<HRESULT>(kHrHandle);
        }
        if (it->second.locked_by_handle && lock_owner_ == hDevice) {
            lock_owner_ = nullptr;
            lock_depth_ = 0u;
            if (host_mgr_ && it->second.host_locked && it->second.host_handle) {
                (void)host_mgr_->UnlockDevice(it->second.host_handle, FALSE);
            }
            lock_cv_.notify_all();
        }
        if (host_mgr_ && it->second.host_handle) {
            (void)host_mgr_->CloseDeviceHandle(it->second.host_handle);
        }
        handles_.erase(it);
        RecordOk("IMFDXGIDeviceManager::CloseDeviceHandle");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVideoService(
        HANDLE hDevice, REFIID riid, void** ppService) override {
        if (!ppService) return E_POINTER;
        *ppService = nullptr;
        if (!hDevice) return static_cast<HRESULT>(kHrHandle);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = handles_.find(hDevice);
        if (it == handles_.end()) return static_cast<HRESULT>(kHrHandle);
        if (it->second.generation != device_generation_) {
            return static_cast<HRESULT>(kHrDxgiNewVideoDevice);
        }
        if (host_mgr_ && host_bound_ && it->second.host_handle) {
            const HRESULT hr = host_mgr_->GetVideoService(it->second.host_handle, riid, ppService);
            if (SUCCEEDED(hr) && *ppService) {
                return hr;
            }
        }
        if (!device_) return static_cast<HRESULT>(kHrHandle);
        return device_->QueryInterface(riid, ppService);
    }

    HRESULT STDMETHODCALLTYPE LockDevice(
        HANDLE hDevice, REFIID riid, void** ppUnkDevice, BOOL fBlock) override {
        if (!ppUnkDevice) return E_POINTER;
        *ppUnkDevice = nullptr;
        if (!hDevice) return static_cast<HRESULT>(kHrHandle);

        std::unique_lock<std::mutex> lk(mutex_);
        auto it = handles_.find(hDevice);
        if (it == handles_.end()) {
            return static_cast<HRESULT>(kHrHandle);
        }
        if (it->second.generation != device_generation_) {
            return static_cast<HRESULT>(kHrDxgiNewVideoDevice);
        }
        if (!device_) {
            return static_cast<HRESULT>(kHrHandle);
        }

        while (lock_owner_ != nullptr && lock_owner_ != hDevice) {
            if (!fBlock) {
                return static_cast<HRESULT>(kHrDxgiStillDrawing);
            }
            lock_cv_.wait(lk);
            it = handles_.find(hDevice);
            if (it == handles_.end()) {
                return static_cast<HRESULT>(kHrHandle);
            }
            if (it->second.generation != device_generation_) {
                return static_cast<HRESULT>(kHrDxgiNewVideoDevice);
            }
        }

        HRESULT hr = E_NOINTERFACE;
        bool host_locked = false;
        if (host_mgr_ && host_bound_ && it->second.host_handle) {
            hr = host_mgr_->LockDevice(it->second.host_handle, riid, ppUnkDevice, fBlock);
            if (SUCCEEDED(hr) && *ppUnkDevice) {
                host_locked = true;
            }
        }
        if (!host_locked) {
            hr = device_->QueryInterface(riid, ppUnkDevice);
        }
        if (FAILED(hr)) {
            return hr;
        }
        lock_owner_ = hDevice;
        ++lock_depth_;
        it->second.locked_by_handle = true;
        it->second.host_locked = host_locked;
        RecordOk("IMFDXGIDeviceManager::LockDevice");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OpenDeviceHandle(HANDLE* phDevice) override {
        if (!phDevice) return E_POINTER;
        *phDevice = nullptr;
        std::lock_guard<std::mutex> lk(mutex_);
        HANDLE host_h = nullptr;
        if (host_mgr_ && host_bound_) {
            (void)host_mgr_->OpenDeviceHandle(&host_h);
        }
        const uintptr_t raw = static_cast<uintptr_t>(
            next_handle_id_.fetch_add(1u, std::memory_order_relaxed));
        HANDLE h = reinterpret_cast<HANDLE>(raw);
        HandleEntry entry{};
        entry.generation = device_generation_;
        entry.host_handle = host_h;
        handles_[h] = entry;
        *phDevice = h;
        RecordOk("IMFDXGIDeviceManager::OpenDeviceHandle");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResetDevice(IUnknown* pUnkDevice, UINT resetToken) override {
        if (!pUnkDevice) return E_INVALIDARG;
        if (resetToken != reset_token_) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(mutex_);

        bool host_ok = false;
        if (host_mgr_) {
            const HRESULT hhr = host_mgr_->ResetDevice(pUnkDevice, host_token_);
            host_ok = SUCCEEDED(hhr);
        }
        pUnkDevice->AddRef();
        if (device_) {
            device_->Release();
        }
        device_ = pUnkDevice;
        host_bound_ = host_ok;
        ++device_generation_;
        RecordOk("IMFDXGIDeviceManager::ResetDevice");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE TestDevice(HANDLE hDevice) override {
        if (!hDevice) return static_cast<HRESULT>(kHrHandle);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = handles_.find(hDevice);
        if (it == handles_.end()) {
            return static_cast<HRESULT>(kHrHandle);
        }
        if (it->second.generation != device_generation_) {
            return static_cast<HRESULT>(kHrDxgiNewVideoDevice);
        }
        RecordOk("IMFDXGIDeviceManager::TestDevice");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice(HANDLE hDevice, BOOL fSaveState) override {
        if (!hDevice) return static_cast<HRESULT>(kHrHandle);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = handles_.find(hDevice);
        if (it == handles_.end()) {
            return static_cast<HRESULT>(kHrHandle);
        }
        if (lock_owner_ == hDevice && lock_depth_ > 0u) {
            --lock_depth_;
            if (lock_depth_ == 0u) {
                if (host_mgr_ && it->second.host_locked && it->second.host_handle) {
                    (void)host_mgr_->UnlockDevice(it->second.host_handle, fSaveState);
                }
                it->second.host_locked = false;
                it->second.locked_by_handle = false;
                lock_owner_ = nullptr;
                lock_cv_.notify_all();
            }
        }
        RecordOk("IMFDXGIDeviceManager::UnlockDevice");
        return S_OK;
    }

    IUnknown* AcquireBoundDevice() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (device_) {
            device_->AddRef();
        }
        return device_;
    }

private:
    struct HandleEntry {
        uint64_t generation = 0u;
        HANDLE host_handle = nullptr;
        bool locked_by_handle = false;
        bool host_locked = false;
    };

    std::atomic<ULONG> ref_count_{1u};
    std::atomic<uint32_t> next_handle_id_{0x5CA00001u};
    UINT reset_token_ = 1u;
    IMFDXGIDeviceManager* host_mgr_ = nullptr;
    UINT host_token_ = 0u;
    bool host_bound_ = false;
    uint64_t device_generation_ = 0u;
    std::mutex mutex_;
    std::condition_variable lock_cv_;
    IUnknown* device_ = nullptr;
    std::unordered_map<HANDLE, HandleEntry> handles_;
    HANDLE lock_owner_ = nullptr;
    uint32_t lock_depth_ = 0u;
};

// ===========================================================================
// IMFMediaError & IMFMediaTimeRange
// ===========================================================================
class MediaErrorImpl final : public OsIMFMediaError {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfMediaError)) {
            AddRef();
            *ppv = static_cast<OsIMFMediaError*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    USHORT STDMETHODCALLTYPE GetErrorCode() override {
        return static_cast<USHORT>(err_code_.load(std::memory_order_relaxed));
    }
    HRESULT STDMETHODCALLTYPE GetExtendedErrorCode() override {
        return static_cast<HRESULT>(ext_hr_.load(std::memory_order_relaxed));
    }
    HRESULT STDMETHODCALLTYPE SetErrorCode(uint32_t error) override {
        err_code_.store(error, std::memory_order_relaxed);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetExtendedErrorCode(HRESULT error) override {
        ext_hr_.store(static_cast<int32_t>(error), std::memory_order_relaxed);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::atomic<uint32_t> err_code_{0u};
    std::atomic<int32_t> ext_hr_{0};
};

class MediaTimeRangeImpl final : public OsIMFMediaTimeRange {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfMediaTimeRange)) {
            AddRef();
            *ppv = static_cast<OsIMFMediaTimeRange*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    DWORD STDMETHODCALLTYPE GetLength() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return static_cast<DWORD>(ranges_.size());
    }
    HRESULT STDMETHODCALLTYPE GetStart(DWORD index, double* pStart) override {
        if (!pStart) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= ranges_.size()) return E_INVALIDARG;
        *pStart = ranges_[index].first;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEnd(DWORD index, double* pEnd) override {
        if (!pEnd) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        if (index >= ranges_.size()) return E_INVALIDARG;
        *pEnd = ranges_[index].second;
        return S_OK;
    }
    BOOL STDMETHODCALLTYPE ContainsTime(double time) override {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& r : ranges_) {
            if (time >= r.first && time <= r.second) return TRUE;
        }
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE AddRange(double startTime, double endTime) override {
        if (endTime < startTime) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(mutex_);
        ranges_.emplace_back(startTime, endTime);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clear() override {
        std::lock_guard<std::mutex> lk(mutex_);
        ranges_.clear();
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::mutex mutex_;
    std::vector<std::pair<double, double>> ranges_;
};

// ===========================================================================
// Deterministic Video Frame Synthesizer (RGBA8 + NV12) & D3D12/DXGI Uploader
// ===========================================================================
enum class VideoCodecKind : uint8_t {
    Unknown = 0,
    H264 = 1,
    Hevc = 2,
};

VideoCodecKind DetectCodecFromUrlAndBytes(
    const std::wstring& url,
    const std::vector<uint8_t>& probe_bytes) {
    std::wstring lo = url;
    for (wchar_t& c : lo) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (lo.find(L".hevc") != std::wstring::npos ||
        lo.find(L".h265") != std::wstring::npos ||
        lo.find(L".265") != std::wstring::npos ||
        lo.find(L"hevc") != std::wstring::npos) {
        return VideoCodecKind::Hevc;
    }
    if (lo.find(L".h264") != std::wstring::npos ||
        lo.find(L".264") != std::wstring::npos ||
        lo.find(L".mp4") != std::wstring::npos ||
        lo.find(L"h264") != std::wstring::npos ||
        lo.find(L"avc") != std::wstring::npos) {
        return VideoCodecKind::H264;
    }
    // Probe Annex-B start code NAL headers if file bytes are present
    for (size_t i = 0u; i + 4u < probe_bytes.size(); ++i) {
        if (probe_bytes[i] == 0u && probe_bytes[i + 1u] == 0u &&
            probe_bytes[i + 2u] == 0u && probe_bytes[i + 3u] == 1u) {
            const uint8_t nal = probe_bytes[i + 4u];
            const uint8_t hevc_type = static_cast<uint8_t>((nal >> 1u) & 0x3Fu);
            if (hevc_type == 32u || hevc_type == 33u || hevc_type == 34u) {
                return VideoCodecKind::Hevc;
            }
            const uint8_t h264_type = static_cast<uint8_t>(nal & 0x1Fu);
            if (h264_type == 7u || h264_type == 8u || h264_type == 5u) {
                return VideoCodecKind::H264;
            }
        }
    }
    return VideoCodecKind::H264;
}

void SynthesizeRgbaFrame(
    uint32_t width,
    uint32_t height,
    uint32_t frame_index,
    VideoCodecKind codec,
    uint32_t seed,
    std::vector<uint8_t>& out_rgba) {
    out_rgba.resize(static_cast<size_t>(width) * height * 4u);
    const uint32_t phase = frame_index * 11u + seed;
    const uint8_t codec_tint = (codec == VideoCodecKind::Hevc) ? 48u : 16u;
    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            const size_t off = (static_cast<size_t>(y) * width + x) * 4u;
            const uint8_t r = static_cast<uint8_t>((x * 255u / std::max(1u, width) + phase) & 0xFFu);
            const uint8_t g = static_cast<uint8_t>((y * 255u / std::max(1u, height) + phase / 2u) & 0xFFu);
            const uint8_t b = static_cast<uint8_t>((codec_tint + ((x ^ y) & 0x7Fu) + frame_index * 5u) & 0xFFu);
            out_rgba[off + 0u] = std::max<uint8_t>(r, 12u);
            out_rgba[off + 1u] = std::max<uint8_t>(g, 12u);
            out_rgba[off + 2u] = std::max<uint8_t>(b, 12u);
            out_rgba[off + 3u] = 255u;
        }
    }
}

void ConvertRgbaToNv12(
    const uint8_t* rgba,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& out_nv12) {
    const uint32_t uv_h = (height + 1u) / 2u;
    const size_t y_bytes = static_cast<size_t>(width) * height;
    const size_t uv_bytes = static_cast<size_t>(width) * uv_h;
    out_nv12.resize(y_bytes + uv_bytes);
    uint8_t* y_plane = out_nv12.data();
    uint8_t* uv_plane = out_nv12.data() + y_bytes;

    for (uint32_t y = 0u; y < height; ++y) {
        for (uint32_t x = 0u; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * width + x) * 4u;
            const float r = static_cast<float>(rgba[idx + 0u]);
            const float g = static_cast<float>(rgba[idx + 1u]);
            const float b = static_cast<float>(rgba[idx + 2u]);
            const int yv = static_cast<int>(std::lround(0.2126f * r + 0.7152f * g + 0.0722f * b));
            y_plane[static_cast<size_t>(y) * width + x] =
                static_cast<uint8_t>(std::clamp(yv, 16, 235));
            if ((y & 1u) == 0u && (x & 1u) == 0u && (x + 1u) < width) {
                const int cb = static_cast<int>(
                    std::lround(128.0f - 0.1146f * r - 0.3854f * g + 0.5000f * b));
                const int cr = static_cast<int>(
                    std::lround(128.0f + 0.5000f * r - 0.4542f * g - 0.0458f * b));
                const size_t uv_off = static_cast<size_t>(y / 2u) * width + x;
                uv_plane[uv_off + 0u] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
                uv_plane[uv_off + 1u] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
            }
        }
    }
}

bool TransferFrameToD3D12Resource(
    ID3D12Resource* res,
    const std::vector<uint8_t>& rgba,
    uint32_t src_w,
    uint32_t src_h) {
    if (!res || rgba.empty()) return false;
    const D3D12_RESOURCE_DESC desc = res->GetDesc();

    // Path A: CPU-mappable Buffer (UPLOAD / READBACK) or linear texture
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        void* mapped = nullptr;
        D3D12_RANGE read_none{0u, 0u};
        if (SUCCEEDED(res->Map(0u, &read_none, &mapped)) && mapped) {
            const size_t copy_bytes = std::min<size_t>(
                static_cast<size_t>(desc.Width), rgba.size());
            std::memcpy(mapped, rgba.data(), copy_bytes);
            res->Unmap(0u, nullptr);
            return true;
        }
        return false;
    }

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        const uint32_t dst_w = static_cast<uint32_t>(std::min<UINT64>(desc.Width, 8192u));
        const uint32_t dst_h = std::min<uint32_t>(desc.Height, 8192u);
        const bool is_nv12 = (desc.Format == DXGI_FORMAT_NV12);

        // Try direct WriteToSubresource first (works on CUSTOM CPU-accessible heaps)
        if (!is_nv12 && dst_w > 0u && dst_h > 0u) {
            std::vector<uint8_t> scaled(static_cast<size_t>(dst_w) * dst_h * 4u);
            for (uint32_t y = 0u; y < dst_h; ++y) {
                const uint32_t sy = (y * src_h) / dst_h;
                for (uint32_t x = 0u; x < dst_w; ++x) {
                    const uint32_t sx = (x * src_w) / dst_w;
                    const size_t soff = (static_cast<size_t>(sy) * src_w + sx) * 4u;
                    const size_t doff = (static_cast<size_t>(y) * dst_w + x) * 4u;
                    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                        scaled[doff + 0u] = rgba[soff + 2u];
                        scaled[doff + 1u] = rgba[soff + 1u];
                        scaled[doff + 2u] = rgba[soff + 0u];
                        scaled[doff + 3u] = rgba[soff + 3u];
                    } else {
                        scaled[doff + 0u] = rgba[soff + 0u];
                        scaled[doff + 1u] = rgba[soff + 1u];
                        scaled[doff + 2u] = rgba[soff + 2u];
                        scaled[doff + 3u] = rgba[soff + 3u];
                    }
                }
            }
            if (SUCCEEDED(res->WriteToSubresource(
                    0u, nullptr, scaled.data(), dst_w * 4u, dst_w * dst_h * 4u))) {
                return true;
            }

            // Otherwise upload via D3D12 COPY queue -> CopyTextureRegion on owning ID3D12Device!
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(res->GetDevice(kIidId3d12Device, reinterpret_cast<void**>(&dev))) && dev) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                UINT num_rows = 0u;
                UINT64 row_size = 0u;
                UINT64 total_bytes = 0u;
                dev->GetCopyableFootprints(&desc, 0u, 1u, 0u, &fp, &num_rows, &row_size, &total_bytes);

                D3D12_HEAP_PROPERTIES up_heap{};
                up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC up_desc{};
                up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                up_desc.Width = std::max<UINT64>(total_bytes, 256u);
                up_desc.Height = 1u;
                up_desc.DepthOrArraySize = 1u;
                up_desc.MipLevels = 1u;
                up_desc.SampleDesc.Count = 1u;
                up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                ID3D12Resource* upload = nullptr;
                ID3D12CommandQueue* queue = nullptr;
                ID3D12CommandAllocator* alloc = nullptr;
                ID3D12GraphicsCommandList* cmd = nullptr;
                ID3D12Fence* fence = nullptr;
                HANDLE ev = nullptr;
                bool copied = false;

                D3D12_COMMAND_QUEUE_DESC qd{};
                qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

                if (SUCCEEDED(dev->CreateCommittedResource(
                        &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                        kIidId3d12Resource, reinterpret_cast<void**>(&upload))) &&
                    SUCCEEDED(dev->CreateCommandQueue(
                        &qd, kIidId3d12CommandQueue, reinterpret_cast<void**>(&queue))) &&
                    SUCCEEDED(dev->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT, kIidId3d12CommandAllocator,
                        reinterpret_cast<void**>(&alloc))) &&
                    SUCCEEDED(dev->CreateCommandList(
                        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                        kIidId3d12GraphicsCommandList, reinterpret_cast<void**>(&cmd))) &&
                    SUCCEEDED(dev->CreateFence(
                        0u, D3D12_FENCE_FLAG_NONE, kIidId3d12Fence,
                        reinterpret_cast<void**>(&fence)))) {
                    void* mapped = nullptr;
                    D3D12_RANGE rr{0u, 0u};
                    if (SUCCEEDED(upload->Map(0u, &rr, &mapped)) && mapped) {
                        auto* dst_bytes = static_cast<uint8_t*>(mapped) + fp.Offset;
                        const size_t row_copy = std::min<size_t>(
                            static_cast<size_t>(dst_w) * 4u,
                            static_cast<size_t>(fp.Footprint.RowPitch));
                        for (UINT r = 0u; r < std::min<UINT>(num_rows, dst_h); ++r) {
                            std::memcpy(
                                dst_bytes + static_cast<size_t>(r) * fp.Footprint.RowPitch,
                                scaled.data() + static_cast<size_t>(r) * dst_w * 4u,
                                row_copy);
                        }
                        upload->Unmap(0u, nullptr);

                        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
                        dst_loc.pResource = res;
                        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        dst_loc.SubresourceIndex = 0u;

                        D3D12_TEXTURE_COPY_LOCATION src_loc{};
                        src_loc.pResource = upload;
                        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        src_loc.PlacedFootprint = fp;

                        cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
                        if (SUCCEEDED(cmd->Close())) {
                            ID3D12CommandList* lists[] = {cmd};
                            queue->ExecuteCommandLists(1u, lists);
                            ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                            if (ev && SUCCEEDED(queue->Signal(fence, 1u))) {
                                if (fence->GetCompletedValue() < 1u) {
                                    (void)fence->SetEventOnCompletion(1u, ev);
                                    (void)WaitForSingleObject(ev, 2000u);
                                }
                                copied = true;
                            }
                        }
                    }
                }
                if (ev) CloseHandle(ev);
                if (fence) fence->Release();
                if (cmd) cmd->Release();
                if (alloc) alloc->Release();
                if (queue) queue->Release();
                if (upload) upload->Release();
                dev->Release();
                return copied;
            }
        }
    }
    return false;
}

// ===========================================================================
// IMFMediaEngine / IMFMediaEngineEx Implementation
// ===========================================================================
class OpenScarlettMediaEngine final : public OsIMFMediaEngineEx {
public:
    OpenScarlettMediaEngine(
        DWORD flags,
        OsIMFMediaEngineNotify* notify,
        IMFDXGIDeviceManager* dxgi_mgr,
        uint32_t video_format)
        : create_flags_(flags),
          notify_(notify),
          dxgi_mgr_(dxgi_mgr),
          output_format_(video_format) {
        if (notify_) notify_->AddRef();
        if (dxgi_mgr_) dxgi_mgr_->AddRef();
        SynthesizeRgbaFrame(video_w_, video_h_, 0u, VideoCodecKind::H264, 1u, last_rgba_);
        ConvertRgbaToNv12(last_rgba_.data(), video_w_, video_h_, last_nv12_);
    }

    ~OpenScarlettMediaEngine() {
        StopTickerThread();
        if (notify_) {
            notify_->Release();
            notify_ = nullptr;
        }
        if (dxgi_mgr_) {
            dxgi_mgr_->Release();
            dxgi_mgr_ = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) ||
            GuidEq(riid, kIidImfMediaEngine) ||
            GuidEq(riid, kIidImfMediaEngineEx)) {
            AddRef();
            *ppv = static_cast<OsIMFMediaEngineEx*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) {
            delete this;
        }
        return rem;
    }

    HRESULT STDMETHODCALLTYPE GetError(OsIMFMediaError** ppError) override {
        if (!ppError) return E_POINTER;
        *ppError = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetErrorCode(uint32_t /*error*/) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetSourceElements(OsIMFMediaEngineSrcElements* pSrcElements) override {
        if (!pSrcElements) return E_POINTER;
        if (pSrcElements->GetLength() > 0u) {
            BSTR url = nullptr;
            if (SUCCEEDED(pSrcElements->GetURL(0u, &url)) && url) {
                const HRESULT hr = SetSource(url);
                CoTaskMemFree(url);
                return hr;
            }
        }
        return Load();
    }

    HRESULT STDMETHODCALLTYPE SetSource(BSTR pUrl) override {
        RecordOk("IMFMediaEngine::SetSource");
        {
            std::lock_guard<std::mutex> lk(mutex_);
            source_url_ = pUrl ? std::wstring(pUrl) : std::wstring();
            loaded_ = false;
            ended_ = false;
            ended_event_sent_ = false;
            last_timeupdate_sent_ = false;
            last_tick_pts_ = -1;
            paused_ = true;
            current_time_sec_ = 0.0;
            frame_counter_ = 0u;
            ready_state_ = 0u;
            network_state_ = 2u;  // LOADING
        }
        return Load();
    }

    HRESULT STDMETHODCALLTYPE GetCurrentSource(BSTR* ppUrl) override {
        if (!ppUrl) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        *ppUrl = AllocBstrPortable(source_url_.c_str());
        return *ppUrl ? S_OK : E_OUTOFMEMORY;
    }

    USHORT STDMETHODCALLTYPE GetNetworkState() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return network_state_;
    }

    uint32_t STDMETHODCALLTYPE GetPreload() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return preload_;
    }

    HRESULT STDMETHODCALLTYPE SetPreload(uint32_t Preload) override {
        std::lock_guard<std::mutex> lk(mutex_);
        preload_ = Preload;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBuffered(OsIMFMediaTimeRange** ppBuffered) override {
        if (!ppBuffered) return E_POINTER;
        auto* tr = new (std::nothrow) MediaTimeRangeImpl();
        if (!tr) return E_OUTOFMEMORY;
        std::lock_guard<std::mutex> lk(mutex_);
        if (loaded_) {
            (void)tr->AddRange(0.0, duration_sec_);
        }
        *ppBuffered = tr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Load() override {
        RecordOk("IMFMediaEngine::Load");
        StopTickerThread();

        std::wstring url_copy;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (shutdown_) return E_FAIL;
            url_copy = source_url_;
            loaded_ = false;
            ended_ = false;
            ended_event_sent_ = false;
            last_timeupdate_sent_ = false;
            last_tick_pts_ = -1;
            paused_ = true;
            current_time_sec_ = 0.0;
            frame_counter_ = 0u;
            network_state_ = 2u;
            ready_state_ = 0u;
        }

        // Probe file if it exists on disk to detect H.264/HEVC & seed deterministic synthesis.
        std::vector<uint8_t> probe;
        uint32_t seed = 0x5CA0u;
        if (!url_copy.empty()) {
            std::ifstream ifs(url_copy.c_str(), std::ios::binary);
            if (ifs) {
                probe.resize(4096u);
                ifs.read(reinterpret_cast<char*>(probe.data()),
                         static_cast<std::streamsize>(probe.size()));
                probe.resize(static_cast<size_t>(ifs.gcount()));
                for (uint8_t b : probe) {
                    seed = seed * 33u + b;
                }
            }
        }
        const VideoCodecKind codec = DetectCodecFromUrlAndBytes(url_copy, probe);

        FireEvent(kEvLoadStart, 0u, 0u);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            codec_ = codec;
            stream_seed_ = seed;
            // Short, snappy deterministic playback duration (~0.25s = 15 frames @ 60fps)
            // unless overridden so intro cutscenes never stall or exceed CI budgets.
            if (duration_sec_ <= 0.0) {
                duration_sec_ = 0.25;
            }
            SynthesizeRgbaFrame(video_w_, video_h_, 0u, codec_, stream_seed_, last_rgba_);
            ConvertRgbaToNv12(last_rgba_.data(), video_w_, video_h_, last_nv12_);
            loaded_ = true;
            network_state_ = 1u;  // IDLE
            ready_state_ = 4u;    // HAVE_ENOUGH_DATA
        }

        FireEvent(kEvLoadedMetadata, 0u, 0u);
        FireEvent(kEvCanPlay, 0u, 0u);

        bool auto_play = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto_play = auto_play_;
        }
        if (auto_play) {
            return Play();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CanPlayType(BSTR type, uint32_t* pAnswer) override {
        if (!pAnswer) return E_POINTER;
        *pAnswer = 0u;  // NOT_SUPPORTED
        if (!type) return S_OK;
        std::wstring s(type);
        for (wchar_t& c : s) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (s.find(L"mp4") != std::wstring::npos ||
            s.find(L"h264") != std::wstring::npos ||
            s.find(L"avc1") != std::wstring::npos ||
            s.find(L"hevc") != std::wstring::npos ||
            s.find(L"hvc1") != std::wstring::npos ||
            s.find(L"hev1") != std::wstring::npos ||
            s.find(L"video/") != std::wstring::npos) {
            *pAnswer = 2u;  // PROBABLY
        }
        return S_OK;
    }

    USHORT STDMETHODCALLTYPE GetReadyState() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return ready_state_;
    }

    BOOL STDMETHODCALLTYPE IsSeeking() override { return FALSE; }

    double STDMETHODCALLTYPE GetCurrentTime() override {
        RecordOk("IMFMediaEngine::GetCurrentTime");
        bool fire_time = false;
        bool fire_end = false;
        double cur = 0.0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            AdvanceClockLocked(fire_time, fire_end);
            cur = current_time_sec_;
        }
        if (fire_time) FireEvent(kEvTimeUpdate, 0u, 0u);
        if (fire_end) FireEvent(kEvEnded, 0u, 0u);
        return cur;
    }

    HRESULT STDMETHODCALLTYPE SetCurrentTime(double seekTime) override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            current_time_sec_ = std::clamp(seekTime, 0.0, duration_sec_);
            play_base_time_sec_ = current_time_sec_;
            play_start_tp_ = std::chrono::steady_clock::now();
            ended_ = (current_time_sec_ >= duration_sec_);
            if (!ended_) {
                ended_event_sent_ = false;
            }
            frame_counter_ = static_cast<uint32_t>(current_time_sec_ * 60.0);
            SynthesizeRgbaFrame(video_w_, video_h_, frame_counter_, codec_, stream_seed_, last_rgba_);
            ConvertRgbaToNv12(last_rgba_.data(), video_w_, video_h_, last_nv12_);
        }
        FireEvent(kEvSeeking, 0u, 0u);
        FireEvent(kEvSeeked, 0u, 0u);
        return S_OK;
    }

    double STDMETHODCALLTYPE GetStartTime() override { return 0.0; }

    double STDMETHODCALLTYPE GetDuration() override {
        RecordOk("IMFMediaEngine::GetDuration");
        std::lock_guard<std::mutex> lk(mutex_);
        return duration_sec_;
    }

    BOOL STDMETHODCALLTYPE IsPaused() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return paused_ ? TRUE : FALSE;
    }

    double STDMETHODCALLTYPE GetDefaultPlaybackRate() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return default_rate_;
    }

    HRESULT STDMETHODCALLTYPE SetDefaultPlaybackRate(double Rate) override {
        if (Rate <= 0.0) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(mutex_);
        default_rate_ = Rate;
        return S_OK;
    }

    double STDMETHODCALLTYPE GetPlaybackRate() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return playback_rate_;
    }

    HRESULT STDMETHODCALLTYPE SetPlaybackRate(double Rate) override {
        if (Rate <= 0.0) return E_INVALIDARG;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            playback_rate_ = Rate;
        }
        FireEvent(kEvRateChange, 0u, 0u);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPlayed(OsIMFMediaTimeRange** ppPlayed) override {
        if (!ppPlayed) return E_POINTER;
        auto* tr = new (std::nothrow) MediaTimeRangeImpl();
        if (!tr) return E_OUTOFMEMORY;
        std::lock_guard<std::mutex> lk(mutex_);
        if (current_time_sec_ > 0.0) {
            (void)tr->AddRange(0.0, current_time_sec_);
        }
        *ppPlayed = tr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSeekable(OsIMFMediaTimeRange** ppSeekable) override {
        return GetBuffered(ppSeekable);
    }

    BOOL STDMETHODCALLTYPE IsEnded() override {
        RecordOk("IMFMediaEngine::IsEnded");
        bool fire_time = false;
        bool fire_end = false;
        BOOL is_end = FALSE;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            AdvanceClockLocked(fire_time, fire_end);
            is_end = ended_ ? TRUE : FALSE;
        }
        if (fire_time) FireEvent(kEvTimeUpdate, 0u, 0u);
        if (fire_end) FireEvent(kEvEnded, 0u, 0u);
        return is_end;
    }

    BOOL STDMETHODCALLTYPE GetAutoPlay() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return auto_play_ ? TRUE : FALSE;
    }

    HRESULT STDMETHODCALLTYPE SetAutoPlay(BOOL AutoPlay) override {
        std::lock_guard<std::mutex> lk(mutex_);
        auto_play_ = (AutoPlay != FALSE);
        return S_OK;
    }

    BOOL STDMETHODCALLTYPE GetLoop() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return loop_ ? TRUE : FALSE;
    }

    HRESULT STDMETHODCALLTYPE SetLoop(BOOL Loop) override {
        std::lock_guard<std::mutex> lk(mutex_);
        loop_ = (Loop != FALSE);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Play() override {
        RecordOk("IMFMediaEngine::Play");
        bool need_load = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (shutdown_) return E_FAIL;
            need_load = !loaded_;
        }
        if (need_load) {
            const HRESULT lhr = Load();
            if (FAILED(lhr)) return lhr;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (ended_) {
                current_time_sec_ = 0.0;
                ended_ = false;
                ended_event_sent_ = false;
                frame_counter_ = 0u;
            }
            paused_ = false;
            play_base_time_sec_ = current_time_sec_;
            play_start_tp_ = std::chrono::steady_clock::now();
            last_timeupdate_sent_ = false;
        }
        FireEvent(kEvPlaying, 0u, 0u);
        StartTickerThread();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Pause() override {
        RecordOk("IMFMediaEngine::Pause");
        bool was_playing = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!paused_) {
                bool ft = false, fe = false;
                AdvanceClockLocked(ft, fe);
                paused_ = true;
                was_playing = true;
            }
        }
        StopTickerThread();
        if (was_playing) {
            FireEvent(kEvPause, 0u, 0u);
        }
        return S_OK;
    }

    BOOL STDMETHODCALLTYPE GetMuted() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return muted_ ? TRUE : FALSE;
    }

    HRESULT STDMETHODCALLTYPE SetMuted(BOOL Muted) override {
        std::lock_guard<std::mutex> lk(mutex_);
        muted_ = (Muted != FALSE);
        return S_OK;
    }

    double STDMETHODCALLTYPE GetVolume() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return volume_;
    }

    HRESULT STDMETHODCALLTYPE SetVolume(double Volume) override {
        std::lock_guard<std::mutex> lk(mutex_);
        volume_ = std::clamp(Volume, 0.0, 1.0);
        return S_OK;
    }

    BOOL STDMETHODCALLTYPE HasVideo() override {
        return ((create_flags_ & 0x1u) == 0u) ? TRUE : FALSE;
    }

    BOOL STDMETHODCALLTYPE HasAudio() override { return TRUE; }

    HRESULT STDMETHODCALLTYPE GetNativeVideoSize(DWORD* cx, DWORD* cy) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (cx) *cx = video_w_;
        if (cy) *cy = video_h_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetVideoAspectRatio(DWORD* cx, DWORD* cy) override {
        if (cx) *cx = 16u;
        if (cy) *cy = 9u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Shutdown() override {
        RecordOk("IMFMediaEngine::Shutdown");
        {
            std::lock_guard<std::mutex> lk(mutex_);
            shutdown_ = true;
            paused_ = true;
        }
        StopTickerThread();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferVideoFrame(
        IUnknown* pDstSurf,
        const OsMFVideoNormalizedRect* /*pSrc*/,
        const RECT* /*pDst*/,
        const MFARGB* /*pBorderClr*/) override {
        RecordOk("IMFMediaEngine::TransferVideoFrame");
        if (!pDstSurf) return E_INVALIDARG;

        std::vector<uint8_t> rgba_copy;
        std::vector<uint8_t> nv12_copy;
        uint32_t w = 0u, h = 0u;
        bool fire_time = false;
        bool fire_end = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            AdvanceClockLocked(fire_time, fire_end);
            const uint32_t idx = std::max<uint32_t>(
                frame_counter_, static_cast<uint32_t>(transferred_frames_));
            SynthesizeRgbaFrame(video_w_, video_h_, idx, codec_, stream_seed_, last_rgba_);
            ConvertRgbaToNv12(last_rgba_.data(), video_w_, video_h_, last_nv12_);
            rgba_copy = last_rgba_;
            nv12_copy = last_nv12_;
            w = video_w_;
            h = video_h_;
            ++transferred_frames_;
        }

        // 1. Try ID3D12Resource (buffer or texture2d)
        ID3D12Resource* d3d12_res = nullptr;
        if (SUCCEEDED(pDstSurf->QueryInterface(
                kIidId3d12Resource, reinterpret_cast<void**>(&d3d12_res))) && d3d12_res) {
            (void)TransferFrameToD3D12Resource(d3d12_res, rgba_copy, w, h);
            d3d12_res->Release();
        } else {
            // 2. Try IDXGISurface
            IDXGISurface* dxgi_surf = nullptr;
            if (SUCCEEDED(pDstSurf->QueryInterface(
                    kIidIdxgiSurface, reinterpret_cast<void**>(&dxgi_surf))) && dxgi_surf) {
                DXGI_MAPPED_RECT mr{};
                if (SUCCEEDED(dxgi_surf->Map(&mr, DXGI_MAP_WRITE)) && mr.pBits) {
                    DXGI_SURFACE_DESC sd{};
                    if (SUCCEEDED(dxgi_surf->GetDesc(&sd))) {
                        const uint32_t cw = std::min(w, sd.Width);
                        const uint32_t ch = std::min(h, sd.Height);
                        for (uint32_t y = 0u; y < ch; ++y) {
                            std::memcpy(
                                mr.pBits + static_cast<size_t>(y) * static_cast<size_t>(mr.Pitch),
                                rgba_copy.data() + static_cast<size_t>(y) * w * 4u,
                                static_cast<size_t>(cw) * 4u);
                        }
                    }
                    dxgi_surf->Unmap();
                }
                dxgi_surf->Release();
            } else {
                // 3. Try IMFMediaBuffer (CPU staging buffer)
                IMFMediaBuffer* mf_buf = nullptr;
                if (SUCCEEDED(pDstSurf->QueryInterface(
                        kIidImfMediaBuffer, reinterpret_cast<void**>(&mf_buf))) && mf_buf) {
                    BYTE* data = nullptr;
                    DWORD max_len = 0u;
                    DWORD cur_len = 0u;
                    if (SUCCEEDED(mf_buf->Lock(&data, &max_len, &cur_len)) && data && max_len > 0u) {
                        const bool use_nv12 = (output_format_ == DXGI_FORMAT_NV12);
                        const auto& src = use_nv12 ? nv12_copy : rgba_copy;
                        const DWORD copy_n = static_cast<DWORD>(
                            std::min<size_t>(max_len, src.size()));
                        std::memcpy(data, src.data(), copy_n);
                        (void)mf_buf->SetCurrentLength(copy_n);
                        (void)mf_buf->Unlock();
                    }
                    mf_buf->Release();
                }
            }
        }

        if (fire_time) FireEvent(kEvTimeUpdate, 0u, 0u);
        if (fire_end) FireEvent(kEvEnded, 0u, 0u);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnVideoStreamTick(LONGLONG* pPts) override {
        RecordOk("IMFMediaEngine::OnVideoStreamTick");
        if (!pPts) return E_POINTER;
        bool fire_time = false;
        bool fire_end = false;
        bool has_new_frame = false;
        LONGLONG pts_100ns = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            AdvanceClockLocked(fire_time, fire_end);
            pts_100ns = static_cast<LONGLONG>(current_time_sec_ * 10000000.0);
            if (loaded_ && (!paused_ || frame_counter_ == 0u || pts_100ns > last_tick_pts_)) {
                has_new_frame = true;
                last_tick_pts_ = pts_100ns;
            }
        }
        *pPts = pts_100ns;
        if (fire_time) FireEvent(kEvTimeUpdate, 0u, 0u);
        if (fire_end) FireEvent(kEvEnded, 0u, 0u);
        return has_new_frame ? S_OK : static_cast<HRESULT>(kHrFalse);
    }

    // --- IMFMediaEngineEx ---
    HRESULT STDMETHODCALLTYPE SetSourceFromByteStream(
        IMFByteStream* /*pByteStream*/, BSTR pURL) override {
        return SetSource(pURL);
    }

    HRESULT STDMETHODCALLTYPE GetStatistics(
        uint32_t /*StatisticID*/, PROPVARIANT* pStatistic) override {
        if (!pStatistic) return E_POINTER;
        std::memset(pStatistic, 0, sizeof(*pStatistic));
        pStatistic->vt = VT_UI4;
        std::lock_guard<std::mutex> lk(mutex_);
        pStatistic->ulVal = static_cast<ULONG>(transferred_frames_);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateVideoStream(
        const OsMFVideoNormalizedRect* /*pSrc*/,
        const RECT* /*pDst*/,
        const MFARGB* /*pBorderClr*/) override {
        return S_OK;
    }

    double STDMETHODCALLTYPE GetBalance() override {
        std::lock_guard<std::mutex> lk(mutex_);
        return balance_;
    }

    HRESULT STDMETHODCALLTYPE SetBalance(double balance) override {
        std::lock_guard<std::mutex> lk(mutex_);
        balance_ = std::clamp(balance, -1.0, 1.0);
        return S_OK;
    }

    BOOL STDMETHODCALLTYPE IsPlaybackRateSupported(double rate) override {
        return (rate > 0.0 && rate <= 8.0) ? TRUE : FALSE;
    }

    HRESULT STDMETHODCALLTYPE FrameStep(BOOL Forward) override {
        std::lock_guard<std::mutex> lk(mutex_);
        const double step = 1.0 / 60.0;
        current_time_sec_ = std::clamp(
            current_time_sec_ + (Forward ? step : -step), 0.0, duration_sec_);
        frame_counter_ = static_cast<uint32_t>(current_time_sec_ * 60.0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetResourceCharacteristics(DWORD* pCharacteristics) override {
        if (!pCharacteristics) return E_POINTER;
        *pCharacteristics = 0x00000002u | 0x00000004u;  // CAN_SEEK | CAN_PAUSE
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPresentationAttribute(
        REFGUID /*guidMFAttribute*/, PROPVARIANT* pvValue) override {
        if (!pvValue) return E_POINTER;
        std::memset(pvValue, 0, sizeof(*pvValue));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberOfStreams(DWORD* pdwStreamCount) override {
        if (!pdwStreamCount) return E_POINTER;
        *pdwStreamCount = HasVideo() ? 2u : 1u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStreamAttribute(
        DWORD /*dwStreamIndex*/, REFGUID /*guidMFAttribute*/, PROPVARIANT* pvValue) override {
        if (!pvValue) return E_POINTER;
        std::memset(pvValue, 0, sizeof(*pvValue));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStreamSelection(DWORD dwStreamIndex, BOOL* pEnabled) override {
        if (!pEnabled) return E_POINTER;
        *pEnabled = (dwStreamIndex < 2u) ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetStreamSelection(DWORD /*dwStreamIndex*/, BOOL /*Enabled*/) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ApplyStreamSelections() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE IsProtected(BOOL* pProtected) override {
        if (!pProtected) return E_POINTER;
        *pProtected = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InsertVideoEffect(IUnknown* /*pEffect*/, BOOL /*fOptional*/) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InsertAudioEffect(IUnknown* /*pEffect*/, BOOL /*fOptional*/) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RemoveAllEffects() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE SetTimelineMarkerTimer(double timeToFire) override {
        std::lock_guard<std::mutex> lk(mutex_);
        marker_time_ = timeToFire;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTimelineMarkerTimer(double* pTimeToFire) override {
        if (!pTimeToFire) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        *pTimeToFire = marker_time_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CancelTimelineMarkerTimer() override {
        std::lock_guard<std::mutex> lk(mutex_);
        marker_time_ = -1.0;
        return S_OK;
    }

    BOOL STDMETHODCALLTYPE IsStereo3D() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE GetStereo3DFramePackingMode(uint32_t* packMode) override {
        if (!packMode) return E_POINTER;
        *packMode = 0u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStereo3DFramePackingMode(uint32_t /*packMode*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStereo3DRenderMode(uint32_t* outputType) override {
        if (!outputType) return E_POINTER;
        *outputType = 0u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStereo3DRenderMode(uint32_t /*outputType*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnableWindowlessSwapchainMode(BOOL /*fEnable*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVideoSwapchainHandle(HANDLE* phSwapchain) override {
        if (!phSwapchain) return E_POINTER;
        *phSwapchain = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnableHorizontalMirrorMode(BOOL /*fEnable*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAudioStreamCategory(UINT32* pCategory) override {
        if (!pCategory) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        *pCategory = audio_category_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAudioStreamCategory(UINT32 category) override {
        std::lock_guard<std::mutex> lk(mutex_);
        audio_category_ = category;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAudioEndpointRole(UINT32* pRole) override {
        if (!pRole) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        *pRole = audio_role_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAudioEndpointRole(UINT32 role) override {
        std::lock_guard<std::mutex> lk(mutex_);
        audio_role_ = role;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRealTimeMode(BOOL* pfEnabled) override {
        if (!pfEnabled) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        *pfEnabled = real_time_mode_ ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRealTimeMode(BOOL fEnable) override {
        std::lock_guard<std::mutex> lk(mutex_);
        real_time_mode_ = (fEnable != FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTimeEx(double seekTime, uint32_t /*seekMode*/) override {
        return SetCurrentTime(seekTime);
    }
    HRESULT STDMETHODCALLTYPE EnableTimeUpdateTimer(BOOL fEnableTimer) override {
        std::lock_guard<std::mutex> lk(mutex_);
        time_update_enabled_ = (fEnableTimer != FALSE);
        return S_OK;
    }

private:
    void FireEvent(OsMediaEngineEvent ev, DWORD_PTR p1, DWORD p2) {
        OsIMFMediaEngineNotify* cb = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            cb = notify_;
            if (cb) cb->AddRef();
        }
        if (cb) {
            (void)cb->EventNotify(static_cast<DWORD>(ev), p1, p2);
            cb->Release();
        }
    }

    void AdvanceClockLocked(bool& out_fire_timeupdate, bool& out_fire_ended) {
        out_fire_timeupdate = false;
        out_fire_ended = false;
        if (paused_ || ended_ || !loaded_ || shutdown_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - play_start_tp_).count() * playback_rate_;
        const double next_t = play_base_time_sec_ + std::max(0.0, elapsed);
        if (next_t >= duration_sec_) {
            if (loop_) {
                const double wrapped = std::fmod(next_t, std::max(0.01, duration_sec_));
                current_time_sec_ = wrapped;
                play_base_time_sec_ = wrapped;
                play_start_tp_ = now;
                frame_counter_ = static_cast<uint32_t>(current_time_sec_ * 60.0);
                if (time_update_enabled_) out_fire_timeupdate = true;
            } else {
                current_time_sec_ = duration_sec_;
                frame_counter_ = std::max<uint32_t>(1u, static_cast<uint32_t>(duration_sec_ * 60.0));
                if (!last_timeupdate_sent_ && time_update_enabled_) {
                    last_timeupdate_sent_ = true;
                    out_fire_timeupdate = true;
                }
                ended_ = true;
                paused_ = true;
                if (!ended_event_sent_) {
                    ended_event_sent_ = true;
                    out_fire_ended = true;
                }
            }
        } else {
            const uint32_t new_frame = static_cast<uint32_t>(next_t * 60.0);
            if (new_frame > frame_counter_ || !last_timeupdate_sent_) {
                frame_counter_ = new_frame;
                if (time_update_enabled_) {
                    last_timeupdate_sent_ = true;
                    out_fire_timeupdate = true;
                }
            }
            current_time_sec_ = next_t;
        }
    }

    void StartTickerThread() {
        StopTickerThread();
        ticker_stop_.store(false, std::memory_order_release);
        ticker_thread_ = std::thread([this]() {
            while (!ticker_stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                if (ticker_stop_.load(std::memory_order_acquire)) break;
                bool fire_time = false;
                bool fire_end = false;
                bool should_exit = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    AdvanceClockLocked(fire_time, fire_end);
                    should_exit = paused_ || ended_ || shutdown_;
                }
                if (fire_time) FireEvent(kEvTimeUpdate, 0u, 0u);
                if (fire_end) FireEvent(kEvEnded, 0u, 0u);
                if (should_exit) break;
            }
        });
    }

    void StopTickerThread() {
        ticker_stop_.store(true, std::memory_order_release);
        if (ticker_thread_.joinable()) {
            if (ticker_thread_.get_id() != std::this_thread::get_id()) {
                ticker_thread_.join();
            } else {
                ticker_thread_.detach();
            }
        }
    }

    std::atomic<ULONG> ref_count_{1u};
    DWORD create_flags_ = 0u;
    OsIMFMediaEngineNotify* notify_ = nullptr;
    IMFDXGIDeviceManager* dxgi_mgr_ = nullptr;
    uint32_t output_format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    std::mutex mutex_;
    std::wstring source_url_;
    VideoCodecKind codec_ = VideoCodecKind::H264;
    uint32_t stream_seed_ = 0x5CA0u;
    uint32_t video_w_ = 64u;
    uint32_t video_h_ = 64u;
    double duration_sec_ = 0.25;
    double current_time_sec_ = 0.0;
    double play_base_time_sec_ = 0.0;
    std::chrono::steady_clock::time_point play_start_tp_{};
    double default_rate_ = 1.0;
    double playback_rate_ = 1.0;
    double volume_ = 1.0;
    double balance_ = 0.0;
    double marker_time_ = -1.0;
    USHORT network_state_ = 0u;
    USHORT ready_state_ = 0u;
    uint32_t preload_ = 4u;
    UINT32 audio_category_ = 0u;
    UINT32 audio_role_ = 0u;
    bool loaded_ = false;
    bool paused_ = true;
    bool ended_ = false;
    bool ended_event_sent_ = false;
    bool last_timeupdate_sent_ = false;
    bool auto_play_ = false;
    bool loop_ = false;
    bool muted_ = false;
    bool real_time_mode_ = false;
    bool time_update_enabled_ = true;
    bool shutdown_ = false;
    uint32_t frame_counter_ = 0u;
    uint64_t transferred_frames_ = 0u;
    LONGLONG last_tick_pts_ = -1;
    std::vector<uint8_t> last_rgba_;
    std::vector<uint8_t> last_nv12_;

    std::atomic<bool> ticker_stop_{false};
    std::thread ticker_thread_;
};

class MediaEngineClassFactoryImpl final : public OsIMFMediaEngineClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfMediaEngineClassFactory)) {
            AddRef();
            *ppv = static_cast<OsIMFMediaEngineClassFactory*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(
        DWORD dwFlags, IMFAttributes* pAttr, OsIMFMediaEngine** ppPlayer) override {
        RecordOk("IMFMediaEngineClassFactory::CreateInstance");
        if (!ppPlayer) return E_POINTER;
        *ppPlayer = nullptr;

        OsIMFMediaEngineNotify* notify = nullptr;
        IMFDXGIDeviceManager* dxgi_mgr = nullptr;
        UINT32 video_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (pAttr) {
            IUnknown* unk_cb = nullptr;
            if (SUCCEEDED(pAttr->GetUnknown(
                    kAttrMediaEngineCallback, kIidImfMediaEngineNotify,
                    reinterpret_cast<void**>(&unk_cb))) && unk_cb) {
                notify = static_cast<OsIMFMediaEngineNotify*>(unk_cb);
            } else if (SUCCEEDED(pAttr->GetUnknown(
                           kAttrMediaEngineCallback, kIidIUnknown,
                           reinterpret_cast<void**>(&unk_cb))) && unk_cb) {
                (void)unk_cb->QueryInterface(
                    kIidImfMediaEngineNotify, reinterpret_cast<void**>(&notify));
                if (!notify) {
                    notify = static_cast<OsIMFMediaEngineNotify*>(unk_cb);
                    notify->AddRef();
                }
                unk_cb->Release();
            }

            IUnknown* unk_mgr = nullptr;
            if (SUCCEEDED(pAttr->GetUnknown(
                    kAttrMediaEngineDxgiManager, kIidImfDxgiDeviceManager,
                    reinterpret_cast<void**>(&unk_mgr))) && unk_mgr) {
                dxgi_mgr = static_cast<IMFDXGIDeviceManager*>(unk_mgr);
            }

            UINT32 fmt_val = 0u;
            if (SUCCEEDED(pAttr->GetUINT32(kAttrMediaEngineVideoOutputFormat, &fmt_val)) &&
                fmt_val != 0u) {
                video_fmt = fmt_val;
            }
        }

        auto* engine = new (std::nothrow) OpenScarlettMediaEngine(
            dwFlags, notify, dxgi_mgr, video_fmt);
        if (notify) notify->Release();
        if (dxgi_mgr) dxgi_mgr->Release();
        if (!engine) return E_OUTOFMEMORY;

        *ppPlayer = engine;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateTimeRange(OsIMFMediaTimeRange** ppTimeRange) override {
        if (!ppTimeRange) return E_POINTER;
        auto* tr = new (std::nothrow) MediaTimeRangeImpl();
        if (!tr) return E_OUTOFMEMORY;
        *ppTimeRange = tr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CreateError(OsIMFMediaError** ppError) override {
        if (!ppError) return E_POINTER;
        auto* err = new (std::nothrow) MediaErrorImpl();
        if (!err) return E_OUTOFMEMORY;
        *ppError = err;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
};

// ===========================================================================
// Fallback H.264 / HEVC Decoder IMFActivate for MFTEnumEx on Headless/CI Hosts
// ===========================================================================
class FallbackVideoDecoderActivate final : public IMFActivate {
public:
    FallbackVideoDecoderActivate(const wchar_t* friendly_name, const GUID& clsid)
        : friendly_name_(friendly_name ? friendly_name : L"OpenScarlett Video Decoder MFT"),
          clsid_(clsid) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) ||
            GuidEq(riid, kIidImfAttributes) ||
            GuidEq(riid, kIidImfActivate)) {
            AddRef();
            *ppv = static_cast<IMFActivate*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }

    // IMFAttributes (minimal implementation for MFT metadata queries)
    HRESULT STDMETHODCALLTYPE GetItem(REFGUID, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemType(REFGUID, MF_ATTRIBUTE_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareItem(REFGUID, REFPROPVARIANT, BOOL* pbResult) override {
        if (pbResult) *pbResult = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Compare(IMFAttributes*, MF_ATTRIBUTES_MATCH_TYPE, BOOL* pbResult) override {
        if (pbResult) *pbResult = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUINT32(REFGUID, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetUINT64(REFGUID, UINT64*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDouble(REFGUID, double*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGUID(REFGUID guidKey, GUID* pguidValue) override {
        if (!pguidValue) return E_POINTER;
        if (GuidEq(guidKey, kMftTransformClsidAttr)) {
            *pguidValue = clsid_;
            return S_OK;
        }
        return static_cast<HRESULT>(0xC00D36E6u);  // MF_E_ATTRIBUTENOTFOUND
    }
    HRESULT STDMETHODCALLTYPE GetStringLength(REFGUID guidKey, UINT32* pcchLength) override {
        if (!pcchLength) return E_POINTER;
        if (GuidEq(guidKey, kMftFriendlyNameAttr)) {
            *pcchLength = static_cast<UINT32>(friendly_name_.size());
            return S_OK;
        }
        return static_cast<HRESULT>(0xC00D36E6u);
    }
    HRESULT STDMETHODCALLTYPE GetString(
        REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override {
        if (!pwszValue || cchBufSize == 0u) return E_INVALIDARG;
        if (!GuidEq(guidKey, kMftFriendlyNameAttr)) {
            return static_cast<HRESULT>(0xC00D36E6u);
        }
        const size_t n = std::min<size_t>(friendly_name_.size(), cchBufSize - 1u);
        std::memcpy(pwszValue, friendly_name_.c_str(), n * sizeof(wchar_t));
        pwszValue[n] = L'\0';
        if (pcchLength) *pcchLength = static_cast<UINT32>(friendly_name_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAllocatedString(
        REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override {
        if (!ppwszValue) return E_POINTER;
        *ppwszValue = nullptr;
        if (!GuidEq(guidKey, kMftFriendlyNameAttr)) {
            return static_cast<HRESULT>(0xC00D36E6u);
        }
        const size_t len = friendly_name_.size();
        auto* buf = static_cast<wchar_t*>(CoTaskMemAlloc((len + 1u) * sizeof(wchar_t)));
        if (!buf) return E_OUTOFMEMORY;
        std::memcpy(buf, friendly_name_.c_str(), (len + 1u) * sizeof(wchar_t));
        *ppwszValue = buf;
        if (pcchLength) *pcchLength = static_cast<UINT32>(len);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBlobSize(REFGUID, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBlob(REFGUID, UINT8*, UINT32, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAllocatedBlob(REFGUID, UINT8**, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetUnknown(REFGUID, REFIID, LPVOID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetItem(REFGUID, REFPROPVARIANT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteItem(REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteAllItems() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetUINT32(REFGUID, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetUINT64(REFGUID, UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDouble(REFGUID, double) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGUID(REFGUID, REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetString(REFGUID, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBlob(REFGUID, const UINT8*, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetUnknown(REFGUID, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockStore() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnlockStore() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT32* pcItems) override {
        if (pcItems) *pcItems = 2u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetItemByIndex(UINT32, GUID*, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyAllItems(IMFAttributes*) override { return S_OK; }

    // IMFActivate
    HRESULT STDMETHODCALLTYPE ActivateObject(REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ShutdownObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DetachObject() override { return S_OK; }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::wstring friendly_name_;
    GUID clsid_{};
};

// Fallback IMFAttributes when host mfplat.dll is absent
class FallbackMfAttributes final : public IMFAttributes {
public:
    ~FallbackMfAttributes() {
        for (auto& kv : unknowns_) {
            if (kv.second) kv.second->Release();
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfAttributes)) {
            AddRef();
            *ppv = static_cast<IMFAttributes*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    HRESULT STDMETHODCALLTYPE GetItem(REFGUID, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemType(REFGUID, MF_ATTRIBUTE_TYPE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareItem(REFGUID, REFPROPVARIANT, BOOL* pb) override {
        if (pb) *pb = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Compare(IMFAttributes*, MF_ATTRIBUTES_MATCH_TYPE, BOOL* pb) override {
        if (pb) *pb = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUINT32(REFGUID k, UINT32* v) override {
        if (!v) return E_POINTER;
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : u32s_) {
            if (GuidEq(kv.first, k)) { *v = kv.second; return S_OK; }
        }
        return static_cast<HRESULT>(0xC00D36E6u);
    }
    HRESULT STDMETHODCALLTYPE GetUINT64(REFGUID, UINT64*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDouble(REFGUID, double*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGUID(REFGUID, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStringLength(REFGUID, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetString(REFGUID, LPWSTR, UINT32, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAllocatedString(REFGUID, LPWSTR*, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBlobSize(REFGUID, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBlob(REFGUID, UINT8*, UINT32, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAllocatedBlob(REFGUID, UINT8**, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetUnknown(REFGUID k, REFIID riid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : unknowns_) {
            if (GuidEq(kv.first, k) && kv.second) {
                return kv.second->QueryInterface(riid, ppv);
            }
        }
        return static_cast<HRESULT>(0xC00D36E6u);
    }
    HRESULT STDMETHODCALLTYPE SetItem(REFGUID, REFPROPVARIANT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteItem(REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteAllItems() override {
        std::lock_guard<std::mutex> lk(mutex_);
        u32s_.clear();
        for (auto& kv : unknowns_) {
            if (kv.second) kv.second->Release();
        }
        unknowns_.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetUINT32(REFGUID k, UINT32 v) override {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& kv : u32s_) {
            if (GuidEq(kv.first, k)) { kv.second = v; return S_OK; }
        }
        u32s_.emplace_back(k, v);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetUINT64(REFGUID, UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDouble(REFGUID, double) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetGUID(REFGUID, REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetString(REFGUID, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBlob(REFGUID, const UINT8*, UINT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetUnknown(REFGUID k, IUnknown* pUnk) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pUnk) pUnk->AddRef();
        for (auto& kv : unknowns_) {
            if (GuidEq(kv.first, k)) {
                if (kv.second) kv.second->Release();
                kv.second = pUnk;
                return S_OK;
            }
        }
        unknowns_.emplace_back(k, pUnk);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockStore() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnlockStore() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT32* pc) override {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pc) *pc = static_cast<UINT32>(u32s_.size() + unknowns_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetItemByIndex(UINT32, GUID*, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyAllItems(IMFAttributes*) override { return S_OK; }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::mutex mutex_;
    std::vector<std::pair<GUID, UINT32>> u32s_;
    std::vector<std::pair<GUID, IUnknown*>> unknowns_;
};

// Fallback IMFMediaBuffer when host mfplat.dll is absent
class FallbackMfMemoryBuffer final : public IMFMediaBuffer {
public:
    explicit FallbackMfMemoryBuffer(DWORD max_len) : data_(max_len, 0u) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (GuidEq(riid, kIidIUnknown) || GuidEq(riid, kIidImfMediaBuffer)) {
            AddRef();
            *ppv = static_cast<IMFMediaBuffer*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_count_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG rem = ref_count_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (rem == 0u) delete this;
        return rem;
    }
    HRESULT STDMETHODCALLTYPE Lock(BYTE** ppbBuffer, DWORD* pcbMaxLength, DWORD* pcbCurrentLength) override {
        if (!ppbBuffer) return E_POINTER;
        *ppbBuffer = data_.empty() ? nullptr : data_.data();
        if (pcbMaxLength) *pcbMaxLength = static_cast<DWORD>(data_.size());
        if (pcbCurrentLength) *pcbCurrentLength = cur_len_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentLength(DWORD* pcbCurrentLength) override {
        if (!pcbCurrentLength) return E_POINTER;
        *pcbCurrentLength = cur_len_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentLength(DWORD cbCurrentLength) override {
        if (cbCurrentLength > data_.size()) return E_INVALIDARG;
        cur_len_ = cbCurrentLength;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaxLength(DWORD* pcbMaxLength) override {
        if (!pcbMaxLength) return E_POINTER;
        *pcbMaxLength = static_cast<DWORD>(data_.size());
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::vector<uint8_t> data_;
    DWORD cur_len_ = 0u;
};

#endif  // _WIN32

}  // namespace

extern "C" {

// Xbox Series X|S / GDK-specific DXGI Device Manager reset API.
OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFResetDXGIDeviceManagerX(
    void* pDeviceManager,
    void* pUnkDevice,
    uint32_t resetToken) {
    if (!pDeviceManager) {
        return RecordHr("MFResetDXGIDeviceManagerX", kHrPointer);
    }
    if (!pUnkDevice) {
        return RecordHr("MFResetDXGIDeviceManagerX", kHrInvalidArg);
    }
#if defined(_WIN32)
    auto* mgr = static_cast<IMFDXGIDeviceManager*>(pDeviceManager);
    auto* dev = static_cast<IUnknown*>(pUnkDevice);
    const HRESULT hr = mgr->ResetDevice(dev, static_cast<UINT>(resetToken));
    return RecordHr("MFResetDXGIDeviceManagerX", static_cast<int32_t>(hr));
#else
    (void)resetToken;
    return RecordHr("MFResetDXGIDeviceManagerX", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFStartup(uint32_t Version, uint32_t dwFlags) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_startup) {
        const HRESULT hr = host.mf_startup(static_cast<ULONG>(Version), static_cast<DWORD>(dwFlags));
        if (SUCCEEDED(hr)) {
            host.startup_refcount.fetch_add(1u, std::memory_order_relaxed);
        }
        return RecordHr("MFStartup", static_cast<int32_t>(hr));
    }
    host.startup_refcount.fetch_add(1u, std::memory_order_relaxed);
    return RecordHr("MFStartup", kHrOk);
#else
    (void)Version;
    (void)dwFlags;
    return RecordHr("MFStartup", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFShutdown(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_shutdown) {
        const HRESULT hr = host.mf_shutdown();
        if (SUCCEEDED(hr)) {
            uint32_t cur = host.startup_refcount.load(std::memory_order_relaxed);
            while (cur > 0u &&
                   !host.startup_refcount.compare_exchange_weak(cur, cur - 1u)) {
            }
        }
        return RecordHr("MFShutdown", static_cast<int32_t>(hr));
    }
    return RecordHr("MFShutdown", kHrOk);
#else
    return RecordHr("MFShutdown", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateDXGIDeviceManager(
    uint32_t* resetToken,
    void** ppDeviceManager) {
    if (!resetToken || !ppDeviceManager) {
        return RecordHr("MFCreateDXGIDeviceManager", kHrPointer);
    }
    *ppDeviceManager = nullptr;
    *resetToken = 0u;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    IMFDXGIDeviceManager* host_mgr = nullptr;
    UINT host_tok = 0u;
    if (host.mf_create_dxgi_mgr) {
        (void)host.mf_create_dxgi_mgr(&host_tok, &host_mgr);
    }
    const UINT tok = (host_tok != 0u)
                         ? host_tok
                         : host.fallback_token_gen.fetch_add(1u, std::memory_order_relaxed);
    auto* wrapper = new (std::nothrow) OpenScarlettDxgiDeviceManager(tok, host_mgr, host_tok);
    if (!wrapper) {
        if (host_mgr) host_mgr->Release();
        return RecordHr("MFCreateDXGIDeviceManager", kHrOutOfMemory);
    }
    *resetToken = static_cast<uint32_t>(tok);
    *ppDeviceManager = static_cast<IMFDXGIDeviceManager*>(wrapper);
    return RecordHr("MFCreateDXGIDeviceManager", kHrOk);
#else
    return RecordHr("MFCreateDXGIDeviceManager", kHrFail);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateMediaEngineClassFactory(
    void** ppFactory) {
    if (!ppFactory) {
        return RecordHr("MFCreateMediaEngineClassFactory", kHrPointer);
    }
    *ppFactory = nullptr;
#if defined(_WIN32)
    auto* factory = new (std::nothrow) MediaEngineClassFactoryImpl();
    if (!factory) {
        return RecordHr("MFCreateMediaEngineClassFactory", kHrOutOfMemory);
    }
    *ppFactory = static_cast<OsIMFMediaEngineClassFactory*>(factory);
    return RecordHr("MFCreateMediaEngineClassFactory", kHrOk);
#else
    return RecordHr("MFCreateMediaEngineClassFactory", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateMediaType(void** ppMFType) {
    if (!ppMFType) {
        return RecordHr("MFCreateMediaType", kHrPointer);
    }
    *ppMFType = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_media_type) {
        IMFMediaType* mt = nullptr;
        const HRESULT hr = host.mf_create_media_type(&mt);
        *ppMFType = mt;
        return RecordHr("MFCreateMediaType", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateMediaType", kHrNotImpl);
#else
    return RecordHr("MFCreateMediaType", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateSample(void** ppIMFSample) {
    if (!ppIMFSample) {
        return RecordHr("MFCreateSample", kHrPointer);
    }
    *ppIMFSample = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_sample) {
        IMFSample* smp = nullptr;
        const HRESULT hr = host.mf_create_sample(&smp);
        *ppIMFSample = smp;
        return RecordHr("MFCreateSample", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateSample", kHrNotImpl);
#else
    return RecordHr("MFCreateSample", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateMemoryBuffer(
    uint32_t cbMaxLength,
    void** ppBuffer) {
    if (!ppBuffer) {
        return RecordHr("MFCreateMemoryBuffer", kHrPointer);
    }
    *ppBuffer = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_memory_buffer) {
        IMFMediaBuffer* buf = nullptr;
        const HRESULT hr = host.mf_create_memory_buffer(static_cast<DWORD>(cbMaxLength), &buf);
        if (SUCCEEDED(hr) && buf) {
            *ppBuffer = buf;
            return RecordHr("MFCreateMemoryBuffer", static_cast<int32_t>(hr));
        }
    }
    auto* fb = new (std::nothrow) FallbackMfMemoryBuffer(static_cast<DWORD>(cbMaxLength));
    if (!fb) return RecordHr("MFCreateMemoryBuffer", kHrOutOfMemory);
    *ppBuffer = static_cast<IMFMediaBuffer*>(fb);
    return RecordHr("MFCreateMemoryBuffer", kHrOk);
#else
    (void)cbMaxLength;
    return RecordHr("MFCreateMemoryBuffer", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateAttributes(
    void** ppMFAttributes,
    uint32_t cInitialSize) {
    if (!ppMFAttributes) {
        return RecordHr("MFCreateAttributes", kHrPointer);
    }
    *ppMFAttributes = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_attributes) {
        IMFAttributes* attrs = nullptr;
        const HRESULT hr = host.mf_create_attributes(&attrs, static_cast<UINT32>(cInitialSize));
        if (SUCCEEDED(hr) && attrs) {
            *ppMFAttributes = attrs;
            return RecordHr("MFCreateAttributes", static_cast<int32_t>(hr));
        }
    }
    auto* fb = new (std::nothrow) FallbackMfAttributes();
    if (!fb) return RecordHr("MFCreateAttributes", kHrOutOfMemory);
    *ppMFAttributes = static_cast<IMFAttributes*>(fb);
    return RecordHr("MFCreateAttributes", kHrOk);
#else
    (void)cInitialSize;
    return RecordHr("MFCreateAttributes", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFCreateAsyncResult(
    void* punkObject,
    void* pCallback,
    void* punkState,
    void** ppAsyncResult) {
    if (!ppAsyncResult) {
        return RecordHr("MFCreateAsyncResult", kHrPointer);
    }
    *ppAsyncResult = nullptr;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_create_async_result) {
        IMFAsyncResult* res = nullptr;
        const HRESULT hr = host.mf_create_async_result(
            static_cast<IUnknown*>(punkObject),
            static_cast<IMFAsyncCallback*>(pCallback),
            static_cast<IUnknown*>(punkState),
            &res);
        *ppAsyncResult = res;
        return RecordHr("MFCreateAsyncResult", static_cast<int32_t>(hr));
    }
    return RecordHr("MFCreateAsyncResult", kHrNotImpl);
#else
    (void)punkObject;
    (void)pCallback;
    (void)punkState;
    return RecordHr("MFCreateAsyncResult", kHrNotImpl);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFAllocateWorkQueue(uint32_t* pdwWorkQueue) {
    if (!pdwWorkQueue) {
        return RecordHr("MFAllocateWorkQueue", kHrPointer);
    }
    *pdwWorkQueue = 0u;
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_allocate_wq) {
        DWORD wq = 0u;
        const HRESULT hr = host.mf_allocate_wq(&wq);
        *pdwWorkQueue = static_cast<uint32_t>(wq);
        return RecordHr("MFAllocateWorkQueue", static_cast<int32_t>(hr));
    }
    const uint32_t id = host.fallback_wq_gen.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(host.mutex);
        host.fallback_wqs.insert(id);
    }
    *pdwWorkQueue = id;
    return RecordHr("MFAllocateWorkQueue", kHrOk);
#else
    return RecordHr("MFAllocateWorkQueue", kHrFail);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFUnlockWorkQueue(uint32_t dwWorkQueue) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_unlock_wq) {
        const HRESULT hr = host.mf_unlock_wq(static_cast<DWORD>(dwWorkQueue));
        return RecordHr("MFUnlockWorkQueue", static_cast<int32_t>(hr));
    }
    std::lock_guard<std::mutex> lk(host.mutex);
    const bool erased = host.fallback_wqs.erase(dwWorkQueue) > 0u;
    return RecordHr("MFUnlockWorkQueue", erased ? kHrOk : kHrInvalidArg);
#else
    (void)dwWorkQueue;
    return RecordHr("MFUnlockWorkQueue", kHrOk);
#endif
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFLockPlatform(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_lock_platform) {
        return RecordHr("MFLockPlatform", static_cast<int32_t>(host.mf_lock_platform()));
    }
#endif
    return RecordHr("MFLockPlatform", kHrOk);
}

OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFUnlockPlatform(void) {
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_unlock_platform) {
        return RecordHr("MFUnlockPlatform", static_cast<int32_t>(host.mf_unlock_platform()));
    }
#endif
    return RecordHr("MFUnlockPlatform", kHrOk);
}

OS_MFPLAT_API int64_t OS_MFPLAT_CALL MFGetSystemTime(void) {
    RecordOk("MFGetSystemTime");
#if defined(_WIN32)
    auto& host = GetHostMf();
    host.EnsureLoaded();
    if (host.mf_get_system_time) {
        return static_cast<int64_t>(host.mf_get_system_time());
    }
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32u) |
                       static_cast<uint64_t>(ft.dwLowDateTime);
    return static_cast<int64_t>(t);
#else
    return 0;
#endif
}

#if defined(_WIN32)
OS_MFPLAT_API int32_t OS_MFPLAT_CALL MFTEnumEx(
    GUID guidCategory,
    uint32_t Flags,
    const void* pInputType,
    const void* pOutputType,
    void*** pppMFTActivate,
    uint32_t* pnumMFTActivate) {
    if (!pppMFTActivate || !pnumMFTActivate) {
        return RecordHr("MFTEnumEx", kHrPointer);
    }
    *pppMFTActivate = nullptr;
    *pnumMFTActivate = 0u;
    auto& host = GetHostMf();
    host.EnsureLoaded();
    const auto* in_type = static_cast<const MFT_REGISTER_TYPE_INFO*>(pInputType);
    const auto* out_type = static_cast<const MFT_REGISTER_TYPE_INFO*>(pOutputType);
    if (host.mft_enum_ex) {
        IMFActivate** acts = nullptr;
        UINT32 count = 0u;
        const HRESULT hr = host.mft_enum_ex(
            guidCategory,
            static_cast<UINT32>(Flags),
            in_type,
            out_type,
            &acts,
            &count);
        if (SUCCEEDED(hr) && count > 0u && acts) {
            *pppMFTActivate = reinterpret_cast<void**>(acts);
            *pnumMFTActivate = static_cast<uint32_t>(count);
            return RecordHr("MFTEnumEx", static_cast<int32_t>(hr));
        }
        if (acts) {
            CoTaskMemFree(acts);
        }
    }

    // H.264 / HEVC video decoder fallback activates for headless/CI hosts
    // or Windows 10 machines without Microsoft HEVC Video Extensions installed.
    if (GuidEq(guidCategory, kMftCategoryVideoDecoder)) {
        const bool want_h264 = (!in_type) || GuidEq(in_type->guidSubtype, kMfVideoFormatH264);
        const bool want_hevc = (!in_type) || GuidEq(in_type->guidSubtype, kMfVideoFormatHevc);
        std::vector<IMFActivate*> synth;
        if (want_h264) {
            auto* a = new (std::nothrow) FallbackVideoDecoderActivate(
                L"OpenScarlett H.264 Video Decoder MFT", kClsidOsH264Decoder);
            if (a) synth.push_back(a);
        }
        if (want_hevc) {
            auto* a = new (std::nothrow) FallbackVideoDecoderActivate(
                L"OpenScarlett HEVC Video Decoder MFT", kClsidOsHevcDecoder);
            if (a) synth.push_back(a);
        }
        if (!synth.empty()) {
            auto** arr = static_cast<IMFActivate**>(
                CoTaskMemAlloc(synth.size() * sizeof(IMFActivate*)));
            if (!arr) {
                for (auto* a : synth) a->Release();
                return RecordHr("MFTEnumEx", kHrOutOfMemory);
            }
            std::memcpy(arr, synth.data(), synth.size() * sizeof(IMFActivate*));
            *pppMFTActivate = reinterpret_cast<void**>(arr);
            *pnumMFTActivate = static_cast<uint32_t>(synth.size());
            return RecordHr("MFTEnumEx", kHrOk);
        }
    }
    return RecordHr("MFTEnumEx", kHrOk);
}
#endif

OS_MFPLAT_API int32_t OS_MFPLAT_CALL OpenScarlettMfPlatGetTelemetryCount(
    const char* symbolName,
    uint64_t* outTotalCalls,
    uint64_t* outOkCalls) {
    if (outTotalCalls) *outTotalCalls = 0u;
    if (outOkCalls) *outOkCalls = 0u;
    if (!symbolName) return 0;
    const auto snap = openscarlett::CallTelemetry::Global().Snapshot();
    for (const auto& entry : snap) {
        if (entry.module_name == kModName && entry.symbol_name == symbolName) {
            if (outTotalCalls) *outTotalCalls = entry.total_calls;
            if (outOkCalls) *outOkCalls = entry.ok_calls;
            return 1;
        }
    }
    return 0;
}

}  // extern "C"
