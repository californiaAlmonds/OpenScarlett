// OpenScarlett Sprint 9 + Sprint 10 (v0.5.0-alpha) — Comprehensive Media / Video /
// Multi-Voice XAudio2_9 / Multi-Frame Bink2 / Complete GameInput Verification Suite.
//
// Validates:
//  1. mfplat_xs: IMFDXGIDeviceManager (ResetDevice, Open/Close/Lock/Unlock/TestDevice)
//     with live D3D12 device, MFTEnumEx H.264 + HEVC decoder queries, and
//     IMFMediaEngine / IMFMediaEngineEx lifecycle (LOADSTART -> LOADEDMETADATA ->
//     CANPLAY -> PLAYING -> TIMEUPDATE -> ENDED within < 2s) + TransferVideoFrame
//     into D3D12 resource & IMFMediaBuffer!
//  2. XAudio2_9: 64-voice concurrent stress test (PCM16, Float32, IMA-ADPCM, Xbox ADPCM,
//     XMA2, Opus 0x704F) routed through submix + Reverb APO + VolumeMeter APO effect chain,
//     exact loop points & ExitLoop, frame-accurate voice callbacks, and monotonic
//     SamplesPlayed audio-clock sync (works in BOTH normal AND OPENSCARLETT_AUDIO_NULL=1 mode!).
//  3. Bink2: 36-frame sequential streaming + seek + loop + keyframe reset + planar YCbCr
//     + RGBA/BGRA output + audio packet framing + monotonic timestamps!
//  4. GameInput: simultaneous Gamepad + Keyboard + Mouse + UiNavigation reading ring buffers
//     (GetCurrentReading / GetNextReading / GetPreviousReading), RegisterDeviceCallback /
//     StopCallback / UnregisterCallback hot-plug connect/disconnect delivery, and
//     LT/RT impulse-trigger SetRumbleState forwarding + telemetry!

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <d3d12.h>
#  include <dxgi1_4.h>
#  include <mfobjects.h>
#  include <mftransform.h>
#  include <objbase.h>
#endif

#include "openscarlett/audio/opus_adpcm_decoder.h"
#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/audio/xma2_decoder.h"
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/middleware/bink2_decoder.h"
#include "os_check.h"

namespace fs = std::filesystem;

namespace {

constexpr float kPi = 3.14159265358979323846f;

fs::path GetExeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0u && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::current_path();
}

fs::path GetScratchDir() {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec || base.empty()) {
        base = GetExeDir();
    }
    const fs::path dir = base / "openscarlett_media_video_gameinput_test";
    fs::create_directories(dir, ec);
    return dir;
}

#if defined(_WIN32)

// Portable inline GUIDs (zero __uuidof)
constexpr GUID kIidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidId3d12Device = {
    0x189819F1u, 0x1DB6u, 0x4B57u, {0xBE, 0x54u, 0x18, 0x21, 0x33, 0x9B, 0x85, 0xF7}};
constexpr GUID kIidId3d12Resource = {
    0x696442BEu, 0xA72Eu, 0x4059u, {0xBC, 0x79u, 0x5B, 0x5C, 0x98, 0x04, 0x0F, 0xAD}};
constexpr GUID kIidIdxgifactory4 = {
    0x1BC6EA02u, 0xEF36u, 0x464Fu, {0xBF, 0x0Cu, 0x21, 0xCA, 0x39, 0xE5, 0x16, 0x8A}};
constexpr GUID kIidIdxgiAdapter = {
    0x2411E7E1u, 0x12ACu, 0x4CCFu, {0xBD, 0x14u, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}};
constexpr GUID kIidImfMediaEngineNotify = {
    0xFEE7C112u, 0xE776u, 0x42B5u, {0x9B, 0xBF, 0x00, 0x48, 0x52, 0x4E, 0x2B, 0xD5}};
constexpr GUID kIidImfMediaEngineEx = {
    0x83015EADu, 0xB1E6u, 0x40D0u, {0xA9, 0x8A, 0x37, 0x14, 0x5F, 0xFE, 0x1A, 0xD1}};
constexpr GUID kAttrMediaEngineCallback = {
    0xC60381B8u, 0x83A4u, 0x41F8u, {0xA3, 0xD0, 0xDE, 0x05, 0x07, 0x68, 0x49, 0xA9}};
constexpr GUID kAttrMediaEngineDxgiManager = {
    0x065702DAu, 0x1094u, 0x486Du, {0x86, 0x17, 0xEE, 0x7C, 0xC4, 0xEE, 0x46, 0x48}};
constexpr GUID kAttrMediaEngineVideoOutputFormat = {
    0x5066893Cu, 0x8CF9u, 0x42BCu, {0x8B, 0x8A, 0x47, 0x22, 0x12, 0xE5, 0x27, 0x26}};
constexpr GUID kMftCategoryVideoDecoder = {
    0xD6C02D4Bu, 0x6833u, 0x45B4u, {0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91}};
constexpr GUID kMfMajorTypeVideo = {
    0x73646976u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMfVideoFormatH264 = {
    0x34363248u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMfVideoFormatHevc = {
    0x43564548u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

constexpr DWORD kEvLoadStart = 1u;
constexpr DWORD kEvLoadedMetadata = 10u;
constexpr DWORD kEvPlaying = 13u;
constexpr DWORD kEvCanPlay = 14u;
constexpr DWORD kEvTimeUpdate = 18u;
constexpr DWORD kEvEnded = 19u;
constexpr HRESULT kHrDxgiNewVideoDevice = static_cast<HRESULT>(0xC00D4E88u);

struct TestMFVideoNormalizedRect {
    float left, top, right, bottom;
};

struct TestIMFMediaEngineNotify : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) = 0;
};

#if defined(GetCurrentTime)
#  undef GetCurrentTime
#endif

struct TestIMFMediaEngine : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetError(IUnknown** ppError) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetErrorCode(uint32_t error) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSourceElements(IUnknown* pSrcElements) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSource(BSTR pUrl) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSource(BSTR* ppUrl) = 0;
    virtual USHORT STDMETHODCALLTYPE GetNetworkState(void) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetPreload(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreload(uint32_t Preload) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffered(IUnknown** ppBuffered) = 0;
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
    virtual HRESULT STDMETHODCALLTYPE GetPlayed(IUnknown** ppPlayed) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSeekable(IUnknown** ppSeekable) = 0;
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
        const TestMFVideoNormalizedRect* pSrc,
        const RECT* pDst,
        const MFARGB* pBorderClr) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnVideoStreamTick(LONGLONG* pPts) = 0;
};

struct TestIMFMediaEngineClassFactory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(
        DWORD dwFlags, IMFAttributes* pAttr, TestIMFMediaEngine** ppPlayer) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTimeRange(IUnknown** ppTimeRange) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateError(IUnknown** ppError) = 0;
};

class RecordingMediaEngineNotify final : public TestIMFMediaEngineNotify {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (std::memcmp(&riid, &kIidIUnknown, sizeof(GUID)) == 0 ||
            std::memcmp(&riid, &kIidImfMediaEngineNotify, sizeof(GUID)) == 0) {
            AddRef();
            *ppv = static_cast<TestIMFMediaEngineNotify*>(this);
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
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            events_.push_back(event);
            if (event == kEvEnded) {
                ended_ = true;
            }
        }
        cv_.notify_all();
        return S_OK;
    }

    bool WaitForEnded(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, timeout, [&]() { return ended_; });
    }

    std::vector<DWORD> SnapshotEvents() {
        std::lock_guard<std::mutex> lk(mutex_);
        return events_;
    }

    void Reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        events_.clear();
        ended_ = false;
    }

private:
    std::atomic<ULONG> ref_count_{1u};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<DWORD> events_;
    bool ended_ = false;
};

ID3D12Device* CreateLiveOrWarpD3D12Device() {
    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(
            nullptr, D3D_FEATURE_LEVEL_11_0, kIidId3d12Device, reinterpret_cast<void**>(&dev))) &&
        dev) {
        return dev;
    }
    IDXGIFactory4* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(kIidIdxgifactory4, reinterpret_cast<void**>(&fac))) && fac) {
        IDXGIAdapter* warp = nullptr;
        if (SUCCEEDED(fac->EnumWarpAdapter(kIidIdxgiAdapter, reinterpret_cast<void**>(&warp))) && warp) {
            (void)D3D12CreateDevice(
                warp, D3D_FEATURE_LEVEL_11_0, kIidId3d12Device, reinterpret_cast<void**>(&dev));
            warp->Release();
        }
        fac->Release();
    }
    return dev;
}

// ===========================================================================
// TEST 1: mfplat_xs Hardware Video Decode, IMFDXGIDeviceManager & IMFMediaEngine
// ===========================================================================
void TestMfPlatVideoAndMediaEngine() {
    std::printf("[Test 1] mfplat_xs: IMFDXGIDeviceManager + MFTEnumEx (H.264/HEVC) + IMFMediaEngine...\n");

    const fs::path dll_path = GetExeDir() / "mfplat_xs.dll";
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    OS_REQUIRE(mod != nullptr);

    using PfnMFStartup = int32_t(__stdcall*)(uint32_t, uint32_t);
    using PfnMFShutdown = int32_t(__stdcall*)(void);
    using PfnMFCreateDxgiMgr = int32_t(__stdcall*)(uint32_t*, void**);
    using PfnMFResetDxgiX = int32_t(__stdcall*)(void*, void*, uint32_t);
    using PfnMFCreateFactory = int32_t(__stdcall*)(void**);
    using PfnMFCreateAttrs = int32_t(__stdcall*)(void**, uint32_t);
    using PfnMFCreateMemBuf = int32_t(__stdcall*)(uint32_t, void**);
    using PfnMFTEnumEx = int32_t(__stdcall*)(
        GUID, uint32_t, const void*, const void*, void***, uint32_t*);

    auto fn_start = reinterpret_cast<PfnMFStartup>(reinterpret_cast<void*>(GetProcAddress(mod, "MFStartup")));
    auto fn_stop = reinterpret_cast<PfnMFShutdown>(reinterpret_cast<void*>(GetProcAddress(mod, "MFShutdown")));
    auto fn_create_mgr = reinterpret_cast<PfnMFCreateDxgiMgr>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateDXGIDeviceManager")));
    auto fn_reset_x = reinterpret_cast<PfnMFResetDxgiX>(reinterpret_cast<void*>(GetProcAddress(mod, "MFResetDXGIDeviceManagerX")));
    auto fn_create_fac = reinterpret_cast<PfnMFCreateFactory>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateMediaEngineClassFactory")));
    auto fn_create_attrs = reinterpret_cast<PfnMFCreateAttrs>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateAttributes")));
    auto fn_create_buf = reinterpret_cast<PfnMFCreateMemBuf>(reinterpret_cast<void*>(GetProcAddress(mod, "MFCreateMemoryBuffer")));
    auto fn_enum_mft = reinterpret_cast<PfnMFTEnumEx>(reinterpret_cast<void*>(GetProcAddress(mod, "MFTEnumEx")));

    OS_REQUIRE(fn_start && fn_stop && fn_create_mgr && fn_reset_x);
    OS_REQUIRE(fn_create_fac && fn_create_attrs && fn_create_buf && fn_enum_mft);

    constexpr uint32_t kMfVer = (0x0002u << 16u) | 0x0070u;
    OS_REQUIRE(fn_start(kMfVer, 0x1u) == 0);

    // 1A. IMFDXGIDeviceManager full lifecycle bound to live D3D12 device
    uint32_t reset_tok = 0u;
    IMFDXGIDeviceManager* mgr = nullptr;
    OS_REQUIRE(fn_create_mgr(&reset_tok, reinterpret_cast<void**>(&mgr)) == 0);
    OS_REQUIRE(mgr != nullptr && reset_tok != 0u);

    ID3D12Device* d3d_dev = CreateLiveOrWarpD3D12Device();
    OS_REQUIRE(d3d_dev != nullptr);

    OS_REQUIRE(mgr->ResetDevice(d3d_dev, reset_tok) == S_OK);
    HANDLE hdev = nullptr;
    OS_REQUIRE(mgr->OpenDeviceHandle(&hdev) == S_OK && hdev != nullptr);
    OS_REQUIRE(mgr->TestDevice(hdev) == S_OK);

    ID3D12Device* locked_dev = nullptr;
    OS_REQUIRE(mgr->LockDevice(hdev, kIidId3d12Device, reinterpret_cast<void**>(&locked_dev), TRUE) == S_OK);
    OS_REQUIRE(locked_dev != nullptr);
    locked_dev->Release();
    OS_REQUIRE(mgr->UnlockDevice(hdev, FALSE) == S_OK);

    // Reset via Xbox MFResetDXGIDeviceManagerX invalidates old handle with MF_E_DXGI_NEW_VIDEO_DEVICE
    OS_REQUIRE(fn_reset_x(mgr, d3d_dev, reset_tok) == 0);
    OS_REQUIRE(mgr->TestDevice(hdev) == kHrDxgiNewVideoDevice);
    OS_REQUIRE(mgr->CloseDeviceHandle(hdev) == S_OK);

    // 1B. MFTEnumEx queries for H.264 & HEVC video decoders
    for (const GUID& subtype : {kMfVideoFormatH264, kMfVideoFormatHevc}) {
        MFT_REGISTER_TYPE_INFO in_t{};
        in_t.guidMajorType = kMfMajorTypeVideo;
        in_t.guidSubtype = subtype;
        void** acts = nullptr;
        uint32_t count = 0u;
        OS_REQUIRE(fn_enum_mft(kMftCategoryVideoDecoder, 0x3Fu, &in_t, nullptr, &acts, &count) == 0);
        OS_REQUIRE(count >= 1u && acts != nullptr);
        for (uint32_t i = 0u; i < count; ++i) {
            if (acts[i]) static_cast<IUnknown*>(acts[i])->Release();
        }
        CoTaskMemFree(acts);
    }

    // 1C. Write synthetic H.264 and HEVC intro clip files in portable temp_directory_path
    const fs::path scratch = GetScratchDir();
    const fs::path h264_path = scratch / "intro_clip.h264";
    const fs::path hevc_path = scratch / "intro_clip.hevc";
    {
        const uint8_t h264_nal[] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E, 0, 0, 0, 1, 0x68, 0xCE, 0x38, 0x80};
        std::ofstream ofs(h264_path, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(h264_nal), sizeof(h264_nal));
        const uint8_t hevc_nal[] = {0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x01, 0, 0, 0, 1, 0x42, 0x01, 0x01, 0x01};
        std::ofstream ofs2(hevc_path, std::ios::binary | std::ios::trunc);
        ofs2.write(reinterpret_cast<const char*>(hevc_nal), sizeof(hevc_nal));
    }

    // 1D. IMFMediaEngineClassFactory -> IMFMediaEngine / IMFMediaEngineEx lifecycle
    TestIMFMediaEngineClassFactory* factory = nullptr;
    OS_REQUIRE(fn_create_fac(reinterpret_cast<void**>(&factory)) == 0 && factory != nullptr);

    IMFAttributes* attrs = nullptr;
    OS_REQUIRE(fn_create_attrs(reinterpret_cast<void**>(&attrs), 4u) == 0 && attrs != nullptr);
    auto* notify = new RecordingMediaEngineNotify();
    OS_REQUIRE(attrs->SetUnknown(kAttrMediaEngineCallback, notify) == S_OK);
    OS_REQUIRE(attrs->SetUnknown(kAttrMediaEngineDxgiManager, mgr) == S_OK);
    OS_REQUIRE(attrs->SetUINT32(kAttrMediaEngineVideoOutputFormat, DXGI_FORMAT_B8G8R8A8_UNORM) == S_OK);

    TestIMFMediaEngine* engine = nullptr;
    OS_REQUIRE(factory->CreateInstance(0u, attrs, &engine) == S_OK && engine != nullptr);

    // Verify IMFMediaEngineEx QI succeeds
    IUnknown* ex_unk = nullptr;
    OS_REQUIRE(engine->QueryInterface(kIidImfMediaEngineEx, reinterpret_cast<void**>(&ex_unk)) == S_OK);
    OS_REQUIRE(ex_unk != nullptr);
    ex_unk->Release();

    // Create a real D3D12 upload buffer resource to receive TransferVideoFrame pixels!
    D3D12_HEAP_PROPERTIES up_hp{};
    up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = 64u * 64u * 4u;
    buf_desc.Height = 1u;
    buf_desc.DepthOrArraySize = 1u;
    buf_desc.MipLevels = 1u;
    buf_desc.SampleDesc.Count = 1u;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* d3d_surf = nullptr;
    OS_REQUIRE(SUCCEEDED(d3d_dev->CreateCommittedResource(
        &up_hp, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, kIidId3d12Resource, reinterpret_cast<void**>(&d3d_surf))));

    // Also test TransferVideoFrame into an IMFMediaBuffer
    IMFMediaBuffer* mf_buf = nullptr;
    OS_REQUIRE(fn_create_buf(64u * 64u * 4u, reinterpret_cast<void**>(&mf_buf)) == 0 && mf_buf != nullptr);

    for (const fs::path& clip_path : {h264_path, hevc_path}) {
        notify->Reset();
        const auto t0 = std::chrono::steady_clock::now();
        std::wstring wurl = clip_path.wstring();
        OS_REQUIRE(engine->SetSource(const_cast<BSTR>(wurl.c_str())) == S_OK);
        OS_REQUIRE(engine->GetDuration() > 0.0);
        OS_REQUIRE(engine->Play() == S_OK);

        LONGLONG pts = -1;
        OS_REQUIRE(SUCCEEDED(engine->OnVideoStreamTick(&pts)));
        OS_REQUIRE(pts >= 0);

        const RECT dst_rc{0, 0, 64, 64};
        OS_REQUIRE(engine->TransferVideoFrame(d3d_surf, nullptr, &dst_rc, nullptr) == S_OK);
        OS_REQUIRE(engine->TransferVideoFrame(mf_buf, nullptr, &dst_rc, nullptr) == S_OK);

        // Verify non-empty pixel data landed in BOTH the D3D12 resource AND the IMFMediaBuffer!
        {
            void* mapped = nullptr;
            D3D12_RANGE rd{0u, 64u * 64u * 4u};
            OS_REQUIRE(SUCCEEDED(d3d_surf->Map(0u, &rd, &mapped)) && mapped != nullptr);
            const auto* bytes = static_cast<const uint8_t*>(mapped);
            uint64_t sum = 0ull;
            for (size_t i = 0u; i < 64u * 64u * 4u; ++i) sum += bytes[i];
            d3d_surf->Unmap(0u, nullptr);
            OS_REQUIRE(sum > 100000ull);
        }
        {
            BYTE* ptr = nullptr;
            DWORD max_l = 0u, cur_l = 0u;
            OS_REQUIRE(mf_buf->Lock(&ptr, &max_l, &cur_l) == S_OK && ptr != nullptr);
            OS_REQUIRE(cur_l == 64u * 64u * 4u);
            uint64_t sum = 0ull;
            for (DWORD i = 0u; i < cur_l; ++i) sum += ptr[i];
            (void)mf_buf->Unlock();
            OS_REQUIRE(sum > 100000ull);
        }

        OS_REQUIRE(notify->WaitForEnded(std::chrono::milliseconds(2000)));
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        OS_REQUIRE(elapsed_ms < 2000);
        OS_REQUIRE(engine->IsEnded() == TRUE);
        OS_REQUIRE(engine->GetCurrentTime() >= engine->GetDuration() - 1e-3);

        // Verify EXACT event ordering: LOADSTART -> LOADEDMETADATA -> CANPLAY -> PLAYING -> TIMEUPDATE -> ENDED
        const auto evs = notify->SnapshotEvents();
        const DWORD required_seq[] = {
            kEvLoadStart, kEvLoadedMetadata, kEvCanPlay, kEvPlaying, kEvTimeUpdate, kEvEnded};
        size_t pos = 0u;
        for (DWORD e : evs) {
            if (pos < 6u && e == required_seq[pos]) {
                ++pos;
            }
        }
        OS_REQUIRE(pos == 6u);
        std::printf("  Clip %-14s: ENDED in %lld ms | events=%zu | D3D12+MFBuf TransferVideoFrame OK\n",
                    clip_path.filename().string().c_str(),
                    static_cast<long long>(elapsed_ms),
                    evs.size());
    }

    OS_REQUIRE(engine->Shutdown() == S_OK);
    mf_buf->Release();
    d3d_surf->Release();
    engine->Release();
    notify->Release();
    attrs->Release();
    factory->Release();
    mgr->Release();
    d3d_dev->Release();
    OS_REQUIRE(fn_stop() == 0);
    FreeLibrary(mod);

    std::error_code ec;
    fs::remove(h264_path, ec);
    fs::remove(hevc_path, ec);
}

#endif  // _WIN32

// ===========================================================================
// TEST 2: XAudio2_9 64-Voice Concurrent Stress + Submix/Reverb/Loop/Clock
// ===========================================================================
struct TestXAudioEffectDescriptor {
    OS_IXAudio2Unknown* pEffect;
    int32_t InitialState;
    uint32_t OutputChannels;
};

struct TestXAudioEffectChain {
    uint32_t EffectCount;
    TestXAudioEffectDescriptor* pEffectDescriptors;
};

class CountingVoiceCallback final : public IXAudio2VoiceCallback {
public:
    void OPENSCARLETT_XAUDIO2_CALL OnVoiceProcessingPassStart(uint32_t) override {
        pass_starts.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnVoiceProcessingPassEnd() override {
        pass_ends.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnStreamEnd() override {
        stream_ends.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnBufferStart(void*) override {
        buffer_starts.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnBufferEnd(void*) override {
        buffer_ends.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnLoopEnd(void*) override {
        loop_ends.fetch_add(1u, std::memory_order_relaxed);
    }
    void OPENSCARLETT_XAUDIO2_CALL OnVoiceError(void*, int32_t) override {
        errors.fetch_add(1u, std::memory_order_relaxed);
    }

    std::atomic<uint32_t> pass_starts{0u};
    std::atomic<uint32_t> pass_ends{0u};
    std::atomic<uint32_t> stream_ends{0u};
    std::atomic<uint32_t> buffer_starts{0u};
    std::atomic<uint32_t> buffer_ends{0u};
    std::atomic<uint32_t> loop_ends{0u};
    std::atomic<uint32_t> errors{0u};
};

void TestXAudio2Concurrent64VoicesAndEffects() {
    std::printf("[Test 2] XAudio2_9: 64 concurrent voices (PCM/Float/ADPCM/XMA2/Opus) + Submix/Reverb/Loops...\n");

    // Load effect-chain & clock helpers dynamically from XAudio2_9.dll
#if defined(_WIN32)
    HMODULE xa_mod = GetModuleHandleW(L"XAudio2_9.dll");
    if (!xa_mod) {
        const fs::path p = GetExeDir() / "XAudio2_9.dll";
        xa_mod = LoadLibraryW(p.c_str());
    }
    OS_REQUIRE(xa_mod != nullptr);
    using PfnEnableEff = int32_t(__stdcall*)(IXAudio2Voice*, uint32_t, int32_t);
    using PfnGetEffState = int32_t(__stdcall*)(IXAudio2Voice*, uint32_t, int32_t*);
    using PfnSetEffParams = int32_t(__stdcall*)(IXAudio2Voice*, uint32_t, const void*, uint32_t);
    using PfnGetEffParams = int32_t(__stdcall*)(IXAudio2Voice*, uint32_t, void*, uint32_t);
    using PfnAdvanceClock = int32_t(__stdcall*)(IXAudio2*, uint32_t, uint64_t*);

    auto fn_enable_eff = reinterpret_cast<PfnEnableEff>(reinterpret_cast<void*>(GetProcAddress(xa_mod, "OpenScarlettXAudio2EnableEffect")));
    auto fn_get_eff_st = reinterpret_cast<PfnGetEffState>(reinterpret_cast<void*>(GetProcAddress(xa_mod, "OpenScarlettXAudio2GetEffectState")));
    auto fn_set_eff_p = reinterpret_cast<PfnSetEffParams>(reinterpret_cast<void*>(GetProcAddress(xa_mod, "OpenScarlettXAudio2SetEffectParameters")));
    auto fn_get_eff_p = reinterpret_cast<PfnGetEffParams>(reinterpret_cast<void*>(GetProcAddress(xa_mod, "OpenScarlettXAudio2GetEffectParameters")));
    auto fn_adv_clock = reinterpret_cast<PfnAdvanceClock>(reinterpret_cast<void*>(GetProcAddress(xa_mod, "OpenScarlettXAudio2AdvanceClock")));
    OS_REQUIRE(fn_enable_eff && fn_get_eff_st && fn_set_eff_p && fn_get_eff_p && fn_adv_clock);
#endif

    IXAudio2* xa = nullptr;
    OS_REQUIRE(XAudio2Create(&xa, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) == OS_XAUDIO2_S_OK && xa != nullptr);

    IXAudio2MasteringVoice* master = nullptr;
    OS_REQUIRE(xa->CreateMasteringVoice(&master, 2u, 48000u) == OS_XAUDIO2_S_OK && master != nullptr);
    OS_REQUIRE(master->SetVolume(0.05f) == OS_XAUDIO2_S_OK);  // Keep live audio quiet

    // Create Reverb APO + VolumeMeter APO and attach to reverb_submix!
    OS_IXAudio2Unknown* reverb_apo = nullptr;
    OS_IXAudio2Unknown* meter_apo = nullptr;
    OS_REQUIRE(XAudio2CreateReverb(&reverb_apo, 0u) == OS_XAUDIO2_S_OK && reverb_apo != nullptr);
    OS_REQUIRE(XAudio2CreateVolumeMeter(&meter_apo, 0u) == OS_XAUDIO2_S_OK && meter_apo != nullptr);

    TestXAudioEffectDescriptor eff_descs[2] = {
        {reverb_apo, 1, 2u},
        {meter_apo, 1, 2u},
    };
    TestXAudioEffectChain eff_chain{2u, eff_descs};

    IXAudio2SubmixVoice* reverb_submix = nullptr;
    OS_REQUIRE(xa->CreateSubmixVoice(&reverb_submix, 2u, 48000u, 0u, 0u, nullptr, &eff_chain) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(reverb_submix != nullptr);
    OS_REQUIRE(reverb_submix->SetVolume(0.75f) == OS_XAUDIO2_S_OK);

    IXAudio2SubmixVoice* dry_submix = nullptr;
    OS_REQUIRE(xa->CreateSubmixVoice(&dry_submix, 2u, 48000u, 0u, 1u, nullptr, nullptr) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(dry_submix != nullptr);

#if defined(_WIN32)
    OS_XAUDIO2FX_REVERB_PARAMETERS rev_p{};
    OS_REQUIRE(fn_get_eff_p(reverb_submix, 0u, &rev_p, sizeof(rev_p)) == OS_XAUDIO2_S_OK);
    rev_p.WetDryMix = 80.0f;
    rev_p.ReflectionsDelay = 5u;
    rev_p.ReverbDelay = 5u;
    rev_p.DecayTime = 1.5f;
    rev_p.RoomSize = 90.0f;
    OS_REQUIRE(fn_set_eff_p(reverb_submix, 0u, &rev_p, sizeof(rev_p)) == OS_XAUDIO2_S_OK);
    int32_t eff_en = 0;
    OS_REQUIRE(fn_get_eff_st(reverb_submix, 0u, &eff_en) == OS_XAUDIO2_S_OK && eff_en == 1);
    OS_REQUIRE(fn_enable_eff(reverb_submix, 0u, 0) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(fn_get_eff_st(reverb_submix, 0u, &eff_en) == OS_XAUDIO2_S_OK && eff_en == 0);
    OS_REQUIRE(fn_enable_eff(reverb_submix, 0u, 1) == OS_XAUDIO2_S_OK);
#endif

    // Prepare reusable encoded payloads for all 4 codec families!
    constexpr uint32_t kPcmFrames = 1024u;
    std::vector<int16_t> pcm16_buf(kPcmFrames * 2u);
    std::vector<float> f32_buf(kPcmFrames * 2u);
    for (uint32_t i = 0u; i < kPcmFrames; ++i) {
        const float s = 0.25f * std::sin(2.0f * kPi * 440.0f * static_cast<float>(i) / 48000.0f);
        f32_buf[i * 2u + 0u] = s;
        f32_buf[i * 2u + 1u] = s;
        const int16_t s16 = static_cast<int16_t>(s * 32767.0f);
        pcm16_buf[i * 2u + 0u] = s16;
        pcm16_buf[i * 2u + 1u] = s16;
    }

    // IMA-ADPCM 0x0011 payload
    std::vector<uint8_t> ima_blocks;
    uint16_t ima_align = 0u, ima_spb = 0u;
    {
        openscarlett::ImaAdpcmConfig icfg{};
        icfg.sample_rate = 48000u;
        icfg.channels = 2u;
        OS_REQUIRE(openscarlett::ImaAdpcmEncodePcmToBlocks(
                       f32_buf.data(), kPcmFrames, icfg, ima_blocks, &ima_align, &ima_spb)
                       .ok());
    }

    // XMA2 0x0166 payload
    std::vector<uint8_t> xma_packets;
    {
        std::vector<float> mono(1024u);
        for (size_t i = 0u; i < mono.size(); ++i) {
            mono[i] = 0.3f * std::sin(2.0f * kPi * 520.0f * static_cast<float>(i) / 48000.0f);
        }
        openscarlett::Xma2Config xcfg{};
        xcfg.sample_rate = 48000u;
        xcfg.channels = 1u;
        OS_REQUIRE(openscarlett::Xma2EncodePcmToPackets(mono.data(), mono.size(), xcfg, xma_packets).ok());
    }

    // Opus 0x704F payload
    std::vector<uint8_t> opus_stream;
    {
        std::vector<std::vector<uint8_t>> pkts;
        for (uint32_t i = 0u; i < 2u; ++i) {
            pkts.push_back(openscarlett::BuildOpusTestPacket(2u, i + 1u, 64u));
        }
        opus_stream = openscarlett::PackOpusLengthPrefixedStream(pkts, false);
    }

    // Spawn 64 simultaneous concurrent source voices!
    constexpr uint32_t kVoiceCount = 64u;
    std::vector<IXAudio2SourceVoice*> voices(kVoiceCount, nullptr);
    std::vector<std::unique_ptr<CountingVoiceCallback>> cbs;
    cbs.reserve(kVoiceCount);

    OS_XAUDIO2_SEND_DESCRIPTOR rev_send{0u, reverb_submix};
    OS_XAUDIO2_VOICE_SENDS rev_sends{1u, &rev_send};
    OS_XAUDIO2_SEND_DESCRIPTOR dry_send{0u, dry_submix};
    OS_XAUDIO2_VOICE_SENDS dry_sends{1u, &dry_send};

    for (uint32_t i = 0u; i < kVoiceCount; ++i) {
        cbs.push_back(std::make_unique<CountingVoiceCallback>());
        const OS_XAUDIO2_VOICE_SENDS* sends = (i % 2u == 0u) ? &rev_sends : &dry_sends;
        IXAudio2SourceVoice* v = nullptr;
        OS_XAUDIO2_BUFFER buf{};
        buf.Flags = OS_XAUDIO2_END_OF_STREAM;
        buf.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1u));

        if (i < 16u) {
            OS_WAVEFORMATEX fmt{};
            fmt.wFormatTag = OS_WAVE_FORMAT_PCM;
            fmt.nChannels = 2u;
            fmt.nSamplesPerSec = 48000u;
            fmt.wBitsPerSample = 16u;
            fmt.nBlockAlign = 4u;
            fmt.nAvgBytesPerSec = 48000u * 4u;
            OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt, 0u, 2.0f, cbs[i].get(), sends) == OS_XAUDIO2_S_OK);
            buf.AudioBytes = static_cast<uint32_t>(pcm16_buf.size() * sizeof(int16_t));
            buf.pAudioData = reinterpret_cast<const uint8_t*>(pcm16_buf.data());
            if (i == 0u) {
                // Finite loop test: 3 loops of 128 frames starting at frame 64
                buf.LoopBegin = 64u;
                buf.LoopLength = 128u;
                buf.LoopCount = 3u;
            } else if (i == 1u) {
                // Infinite loop test exited via ExitLoop() mid-stream!
                buf.LoopBegin = 0u;
                buf.LoopLength = 256u;
                buf.LoopCount = OS_XAUDIO2_LOOP_INFINITE;
            }
        } else if (i < 32u) {
            OS_WAVEFORMATEX fmt{};
            fmt.wFormatTag = OS_WAVE_FORMAT_IEEE_FLOAT;
            fmt.nChannels = 2u;
            fmt.nSamplesPerSec = 48000u;
            fmt.wBitsPerSample = 32u;
            fmt.nBlockAlign = 8u;
            fmt.nAvgBytesPerSec = 48000u * 8u;
            OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt, 0u, 2.0f, cbs[i].get(), sends) == OS_XAUDIO2_S_OK);
            buf.AudioBytes = static_cast<uint32_t>(f32_buf.size() * sizeof(float));
            buf.pAudioData = reinterpret_cast<const uint8_t*>(f32_buf.data());
        } else if (i < 48u) {
            OS_IMAADPCMWAVEFORMAT fmt{};
            fmt.wfx.wFormatTag = OS_WAVE_FORMAT_IMA_ADPCM;
            fmt.wfx.nChannels = 2u;
            fmt.wfx.nSamplesPerSec = 48000u;
            fmt.wfx.nBlockAlign = ima_align;
            fmt.wfx.wBitsPerSample = 4u;
            fmt.wfx.cbSize = 2u;
            fmt.wSamplesPerBlock = ima_spb;
            OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt.wfx, 0u, 2.0f, cbs[i].get(), sends) == OS_XAUDIO2_S_OK);
            buf.AudioBytes = static_cast<uint32_t>(ima_blocks.size());
            buf.pAudioData = ima_blocks.data();
        } else if (i < 56u) {
            OS_XMA2WAVEFORMATEX fmt{};
            fmt.wfx.wFormatTag = OS_WAVE_FORMAT_XMA2;
            fmt.wfx.nChannels = 1u;
            fmt.wfx.nSamplesPerSec = 48000u;
            fmt.wfx.wBitsPerSample = 16u;
            fmt.wfx.cbSize = sizeof(OS_XMA2WAVEFORMATEX) - sizeof(OS_WAVEFORMATEX);
            fmt.NumStreams = 1u;
            fmt.SamplesEncoded = 1024u;
            fmt.BytesPerBlock = openscarlett::kXma2PacketSize;
            fmt.BlockCount = static_cast<uint16_t>(xma_packets.size() / openscarlett::kXma2PacketSize);
            OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt.wfx, 0u, 2.0f, cbs[i].get(), sends) == OS_XAUDIO2_S_OK);
            buf.AudioBytes = static_cast<uint32_t>(xma_packets.size());
            buf.pAudioData = xma_packets.data();
        } else {
            OS_OPUSWAVEFORMAT fmt{};
            fmt.wfx.wFormatTag = OS_WAVE_FORMAT_OPUS;
            fmt.wfx.nChannels = 2u;
            fmt.wfx.nSamplesPerSec = 48000u;
            fmt.wfx.wBitsPerSample = 16u;
            fmt.wfx.cbSize = sizeof(OS_OPUSWAVEFORMAT) - sizeof(OS_WAVEFORMATEX);
            OS_REQUIRE(xa->CreateSourceVoice(&v, &fmt.wfx, 0u, 2.0f, cbs[i].get(), sends) == OS_XAUDIO2_S_OK);
            buf.AudioBytes = static_cast<uint32_t>(opus_stream.size());
            buf.pAudioData = opus_stream.data();
        }

        OS_REQUIRE(v != nullptr);
        OS_REQUIRE(v->SetVolume(0.05f) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(v->SubmitSourceBuffer(&buf) == OS_XAUDIO2_S_OK);
        OS_REQUIRE(v->Start() == OS_XAUDIO2_S_OK);
        voices[i] = v;
    }

    OS_XAUDIO2_PERFORMANCE_DATA perf{};
    xa->GetPerformanceData(&perf);
    OS_REQUIRE(perf.TotalSourceVoiceCount == kVoiceCount);
    OS_REQUIRE(perf.ActiveSourceVoiceCount == kVoiceCount);
    OS_REQUIRE(perf.ActiveSubmixVoiceCount == 2u);

    // Concurrent state polling thread verifies lock-free/deadlock-free monotonic SamplesPlayed!
    std::atomic<bool> stop_poller{false};
    std::atomic<bool> monotonic_ok{true};
    std::thread poller([&]() {
        std::vector<uint64_t> prev_samples(kVoiceCount, 0ull);
        while (!stop_poller.load(std::memory_order_acquire)) {
            for (uint32_t i = 0u; i < kVoiceCount; ++i) {
                OS_XAUDIO2_VOICE_STATE st{};
                voices[i]->GetState(&st);
                if (st.SamplesPlayed < prev_samples[i]) {
                    monotonic_ok.store(false, std::memory_order_release);
                }
                prev_samples[i] = st.SamplesPlayed;
            }
            std::this_thread::yield();
        }
    });

    // Render 10 quanta of 512 frames (5120 total frames) — enough to finish all buffers + loops!
    constexpr uint32_t kQuantum = 512u;
    std::vector<float> mix_chunk(kQuantum * 2u, 0.0f);
    double total_energy = 0.0;
    bool exit_loop_called = false;
    for (uint32_t q = 0u; q < 10u; ++q) {
        OS_REQUIRE(OpenScarlettXAudio2RenderMixForTest(xa, mix_chunk.data(), kQuantum) == OS_XAUDIO2_S_OK);
        for (float s : mix_chunk) {
            OS_REQUIRE(std::isfinite(s) && s >= -1.0f && s <= 1.0f);
            total_energy += static_cast<double>(s) * static_cast<double>(s);
        }
        if (q == 2u && !exit_loop_called) {
            // Voice 1 has been looping infinitely for > 1024 frames; now exit its loop!
            OS_REQUIRE(voices[1]->ExitLoop() == OS_XAUDIO2_S_OK);
            exit_loop_called = true;
        }
    }

    // Advance one more quantum via OpenScarlettXAudio2AdvanceClock to test reverb tail + clock export
#if defined(_WIN32)
    uint64_t total_mixed = 0ull;
    OS_REQUIRE(fn_adv_clock(xa, 512u, &total_mixed) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(total_mixed >= 5632ull);

    // Verify VolumeMeter APO recorded real non-zero peak & RMS levels on reverb_submix!
    float peaks[2] = {};
    float rms[2] = {};
    OS_XAUDIO2FX_VOLUMEMETER_LEVELS lvls{peaks, rms, 2u};
    OS_REQUIRE(fn_get_eff_p(reverb_submix, 1u, &lvls, sizeof(lvls)) == OS_XAUDIO2_S_OK);
    OS_REQUIRE(peaks[0] > 0.0f && rms[0] > 0.0f);
#endif

    stop_poller.store(true, std::memory_order_release);
    poller.join();
    OS_REQUIRE(monotonic_ok.load(std::memory_order_acquire));
    OS_REQUIRE(total_energy > 0.01);

    // Verify ALL 64 voices fired OnBufferStart, OnBufferEnd, OnStreamEnd, and advanced SamplesPlayed!
    for (uint32_t i = 0u; i < kVoiceCount; ++i) {
        OS_REQUIRE(cbs[i]->buffer_starts.load() >= 1u);
        OS_REQUIRE(cbs[i]->buffer_ends.load() >= 1u);
        OS_REQUIRE(cbs[i]->stream_ends.load() >= 1u);
        OS_REQUIRE(cbs[i]->pass_starts.load() >= 1u);
        OS_REQUIRE(cbs[i]->pass_ends.load() == cbs[i]->pass_starts.load());
        OS_REQUIRE(cbs[i]->errors.load() == 0u);
        OS_XAUDIO2_VOICE_STATE st{};
        voices[i]->GetState(&st);
        OS_REQUIRE(st.SamplesPlayed >= 1024ull);
        OS_REQUIRE(st.BuffersQueued == 0u);
    }
    // Verify exact loop counts on voice 0 (3 finite loops) and voice 1 (infinite loop -> ExitLoop)
    OS_REQUIRE(cbs[0]->loop_ends.load() == 3u);
    OS_REQUIRE(cbs[1]->loop_ends.load() >= 2u);

    for (auto* v : voices) v->DestroyVoice();
    reverb_submix->DestroyVoice();
    dry_submix->DestroyVoice();
    master->DestroyVoice();
    reverb_apo->Release();
    meter_apo->Release();
    xa->Release();
    std::printf("  64 voices mixed cleanly | energy=%.3f | v0 loops=%u | v1 inf-loops=%u | Reverb+Meter OK\n",
                total_energy, cbs[0]->loop_ends.load(), cbs[1]->loop_ends.load());
}

// ===========================================================================
// TEST 3: Multi-Frame Bink2 Streaming (36 Frames + Seek + Loop + Audio Packets)
// ===========================================================================
struct Bink2StreamFrameView {
    uint32_t frame_index;
    uint32_t width;
    uint32_t height;
    uint32_t is_keyframe;
    uint32_t end_of_stream;
    uint64_t pts_us;
    uint64_t duration_us;
    const uint8_t* pixels;
    size_t pixels_bytes;
    const uint8_t* plane_y;
    size_t plane_y_bytes;
    const uint8_t* plane_cb;
    size_t plane_cb_bytes;
    const uint8_t* plane_cr;
    size_t plane_cr_bytes;
    const float* audio_pcm;
    uint32_t audio_frames;
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
};

extern "C" {
int32_t OpenScarlettBink2StreamCreate(const uint8_t* data, size_t size, void** out_stream);
void OpenScarlettBink2StreamDestroy(void* stream);
int32_t OpenScarlettBink2StreamSetLoop(void* stream, int32_t loop_enabled);
int32_t OpenScarlettBink2StreamSeek(void* stream, uint32_t frame_index);
int32_t OpenScarlettBink2StreamDecodeNextFrame(void* stream, uint32_t pixel_format, void* out_frame_view);
}

void TestBink2MultiFrameStreaming() {
    std::printf("[Test 3] Bink2: 36-frame sequential stream + planar YCbCr + audio packets + seek + loop...\n");

    constexpr uint32_t kW = 64u;
    constexpr uint32_t kH = 64u;
    constexpr uint32_t kFrames = 36u;

    std::vector<std::vector<uint8_t>> src_frames(kFrames, std::vector<uint8_t>(kW * kH * 4u));
    for (uint32_t fi = 0u; fi < kFrames; ++fi) {
        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                const size_t off = (static_cast<size_t>(y) * kW + x) * 4u;
                src_frames[fi][off + 0u] = static_cast<uint8_t>((x * 3u + fi * 7u) & 0xFFu);
                src_frames[fi][off + 1u] = static_cast<uint8_t>((y * 3u + fi * 5u) & 0xFFu);
                src_frames[fi][off + 2u] = static_cast<uint8_t>(40u + (fi * 4u));
                src_frames[fi][off + 3u] = 255u;
            }
        }
    }

    openscarlett::Bink2EncodeParams ep{};
    ep.width = kW;
    ep.height = kH;
    ep.fps_num = 30u;
    ep.fps_den = 1u;
    ep.fourcc = "KB2i";
    ep.keyframe_interval = 10u;
    ep.audio_track_count = 1u;

    std::vector<uint8_t> container;
    OS_REQUIRE(openscarlett::Bink2EncodeTestStream(src_frames, ep, container).ok());
    OS_REQUIRE(!container.empty());

    void* stream = nullptr;
    OS_REQUIRE(OpenScarlettBink2StreamCreate(container.data(), container.size(), &stream) == 0);
    OS_REQUIRE(stream != nullptr);

    uint64_t prev_pts = 0ull;
    for (uint32_t fi = 0u; fi < kFrames; ++fi) {
        Bink2StreamFrameView fv{};
        OS_REQUIRE(OpenScarlettBink2StreamDecodeNextFrame(stream, 0u, &fv) == 0);
        OS_REQUIRE(fv.frame_index == fi);
        OS_REQUIRE(fv.width == kW && fv.height == kH);
        OS_REQUIRE(fv.pixels != nullptr && fv.pixels_bytes == kW * kH * 4u);
        OS_REQUIRE(fv.plane_y != nullptr && fv.plane_y_bytes == kW * kH);
        OS_REQUIRE(fv.plane_cb != nullptr && fv.plane_cr != nullptr);
        OS_REQUIRE(fv.audio_pcm != nullptr && fv.audio_frames == 1600u && fv.audio_channels == 2u);
        OS_REQUIRE(fv.duration_us == 33333ull);
        if (fi > 0u) {
            OS_REQUIRE(fv.pts_us > prev_pts);
        }
        prev_pts = fv.pts_us;

        uint64_t px_sum = 0ull;
        for (size_t i = 0u; i < fv.pixels_bytes; ++i) px_sum += fv.pixels[i];
        OS_REQUIRE(px_sum > 100000ull);
        OS_REQUIRE(fv.end_of_stream == ((fi + 1u == kFrames) ? 1u : 0u));
    }

    // Non-looping stream must report EOS (1) on next call
    {
        Bink2StreamFrameView eos_v{};
        OS_REQUIRE(OpenScarlettBink2StreamDecodeNextFrame(stream, 0u, &eos_v) == 1);
        OS_REQUIRE(eos_v.end_of_stream == 1u);
    }

    // Enable loop, seek to non-keyframe frame 34, decode 34 -> 35 -> wrap to 0 (keyframe reset)!
    OS_REQUIRE(OpenScarlettBink2StreamSetLoop(stream, 1) == 0);
    OS_REQUIRE(OpenScarlettBink2StreamSeek(stream, 34u) == 0);
    const uint32_t expected_loop_seq[3] = {34u, 35u, 0u};
    for (uint32_t step = 0u; step < 3u; ++step) {
        Bink2StreamFrameView fv{};
        OS_REQUIRE(OpenScarlettBink2StreamDecodeNextFrame(stream, 1u, &fv) == 0);  // BGRA8
        OS_REQUIRE(fv.frame_index == expected_loop_seq[step]);
        OS_REQUIRE(fv.pts_us > prev_pts);
        prev_pts = fv.pts_us;
        if (step == 2u) {
            OS_REQUIRE(fv.is_keyframe == 1u);
        }
    }

    OpenScarlettBink2StreamDestroy(stream);
    std::printf("  36 sequential frames + seek(34)->wrap(0) + YCbCr + 48kHz audio packets OK\n");
}

// ===========================================================================
// TEST 4: GameInput Simultaneous Gamepad+Keyboard+Mouse+UiNav, Hot-Plug & LT/RT Rumble
// ===========================================================================
struct TestUiNavState {
    uint32_t buttons;
};

struct TestRumbleTelemetry {
    GameInputRumbleParams lastParams;
    uint16_t xinputLeftMotor;
    uint16_t xinputRightMotor;
    uint32_t activeSlot;
    uint64_t updateCount;
    uint64_t impulseTriggerUpdateCount;
    uint64_t lastTimestampUs;
};

using PfnTestDeviceCallback = void(OPENSCARLETT_GAMEINPUT_CALL*)(
    uint64_t callbackToken,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus);

struct TestIGameInputEx {
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() = 0;
    virtual uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() = 0;
    virtual int32_t OPENSCARLETT_GAMEINPUT_CALL RegisterDeviceCallback(
        IGameInputDevice* device,
        GameInputKind inputKind,
        uint32_t statusFilter,
        uint32_t enumerationKind,
        void* context,
        PfnTestDeviceCallback callbackFunc,
        uint64_t* callbackToken) = 0;
    virtual void OPENSCARLETT_GAMEINPUT_CALL StopCallback(uint64_t callbackToken) = 0;
    virtual bool OPENSCARLETT_GAMEINPUT_CALL UnregisterCallback(
        uint64_t callbackToken, uint64_t timeoutInMicroseconds) = 0;
};

constexpr GameInputGuid kIidGameInputEx = {
    0x5CA01001u, 0x4254u, 0x445Au, {0x9C, 0x09, 0xFF, 0xC4, 0x0F, 0x00, 0x69, 0x19}};

struct HotPlugEventRecord {
    IGameInputDevice* device = nullptr;
    uint32_t current_status = 0u;
    uint32_t previous_status = 0u;
    uint64_t timestamp = 0ull;
};

struct HotPlugLog {
    std::mutex mutex;
    std::vector<HotPlugEventRecord> records;
};

void OPENSCARLETT_GAMEINPUT_CALL OnHotPlugDeviceEvent(
    uint64_t /*token*/,
    void* context,
    IGameInputDevice* device,
    uint64_t timestamp,
    uint32_t currentStatus,
    uint32_t previousStatus) {
    auto* log = static_cast<HotPlugLog*>(context);
    std::lock_guard<std::mutex> lk(log->mutex);
    log->records.push_back(HotPlugEventRecord{device, currentStatus, previousStatus, timestamp});
}

void TestGameInputLifecycle() {
    std::printf("[Test 4] GameInput: Simultaneous GP+KB+Mouse+UiNav, Hot-Plug Callbacks, LT/RT Impulse Rumble...\n");

#if defined(_WIN32)
    HMODULE gi_mod = GetModuleHandleW(L"GameInput.dll");
    if (!gi_mod) {
        const fs::path p = GetExeDir() / "GameInput.dll";
        gi_mod = LoadLibraryW(p.c_str());
    }
    OS_REQUIRE(gi_mod != nullptr);

    using PfnSimHotPlug = int32_t(__stdcall*)(IGameInput*, uint32_t, GameInputKind, int32_t, IGameInputDevice**);
    using PfnGetRumbleTel = int32_t(__stdcall*)(IGameInputDevice*, void*);
    using PfnGetDevForKind = int32_t(__stdcall*)(IGameInput*, GameInputKind, IGameInputDevice**);
    using PfnGetUiNav = int32_t(__stdcall*)(IGameInputReading*, void*);

    auto fn_hotplug = reinterpret_cast<PfnSimHotPlug>(reinterpret_cast<void*>(GetProcAddress(gi_mod, "OpenScarlettGameInputSimulateDeviceHotPlug")));
    auto fn_rumble_tel = reinterpret_cast<PfnGetRumbleTel>(reinterpret_cast<void*>(GetProcAddress(gi_mod, "OpenScarlettGameInputGetDeviceRumbleTelemetry")));
    auto fn_dev_kind = reinterpret_cast<PfnGetDevForKind>(reinterpret_cast<void*>(GetProcAddress(gi_mod, "OpenScarlettGameInputGetDeviceForKind")));
    auto fn_ui_nav = reinterpret_cast<PfnGetUiNav>(reinterpret_cast<void*>(GetProcAddress(gi_mod, "OpenScarlettGameInputGetUiNavigationState")));
    OS_REQUIRE(fn_hotplug && fn_rumble_tel && fn_dev_kind && fn_ui_nav);
#endif

    IGameInput* gi = nullptr;
    OS_REQUIRE(GameInputCreate(&gi) == GAMEINPUT_S_OK && gi != nullptr);
    gi->SetFocusPolicy(GameInputEnableBackgroundInput);

    // 4A. Simultaneous Gamepad + Keyboard + Mouse + UiNavigation reading ring buffers
    const GameInputKind all_kinds = static_cast<GameInputKind>(
        GameInputKindGamepad | GameInputKindKeyboard | GameInputKindMouse | GameInputKindUiNavigation);

    GameInputKeyState k_enter{};
    k_enter.virtualKey = 0x0Du;  // VK_RETURN
    k_enter.scanCode = 0x1Cu;
    GameInputKeyState k_w{};
    k_w.virtualKey = 'W';
    k_w.scanCode = 0x11u;
    k_w.codePoint = 'W';

    GameInputGamepadState gp1{GameInputGamepadA | GameInputGamepadDPadUp, 0.2f, 0.8f, 0.5f, 0.9f, 0.0f, 0.0f};
    GameInputGamepadState gp2{GameInputGamepadB | GameInputGamepadMenu, 0.6f, 0.1f, -0.4f, 0.0f, 0.3f, -0.3f};

    OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, &gp1, &k_enter, 1u, 1) == GAMEINPUT_S_OK);
    OS_REQUIRE(OpenScarlettGameInputInjectRawMouseDelta(gi, 42, -18, 0, 120, GameInputMouseLeftButton) == GAMEINPUT_S_OK);

    IGameInputReading* r1 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(all_kinds, nullptr, &r1) == GAMEINPUT_S_OK && r1 != nullptr);

    GameInputGamepadState got_gp1{};
    GameInputMouseState got_m1{};
    GameInputKeyState got_k1[4]{};
    OS_REQUIRE(r1->GetGamepadState(&got_gp1) && got_gp1.buttons == gp1.buttons);
    OS_REQUIRE(r1->GetKeyCount() == 1u && r1->GetKeyState(4u, got_k1) == 1u && got_k1[0].virtualKey == 0x0Du);
    OS_REQUIRE(r1->GetMouseState(&got_m1) && got_m1.positionX == 42 && got_m1.positionY == -18 &&
               got_m1.wheelY == 120 && (got_m1.buttons & GameInputMouseLeftButton) != 0u);
#if defined(_WIN32)
    TestUiNavState nav1{};
    OS_REQUIRE(fn_ui_nav(r1, &nav1) == GAMEINPUT_S_OK);
    OS_REQUIRE((nav1.buttons & 0x00000004u) != 0u);  // Accept
    OS_REQUIRE((nav1.buttons & 0x00000010u) != 0u);  // Up
#endif

    // Capture second reading on same device, plus dedicated keyboard & mouse device readings
    OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, &gp2, &k_w, 1u, 1) == GAMEINPUT_S_OK);
    OS_REQUIRE(OpenScarlettGameInputInjectRawMouseDelta(gi, 10, 5, 0, 0, GameInputMouseRightButton) == GAMEINPUT_S_OK);

    IGameInputDevice* gp_dev = nullptr;
    r1->GetDevice(&gp_dev);
    OS_REQUIRE(gp_dev != nullptr);

    IGameInputReading* r2 = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(all_kinds, gp_dev, &r2) == GAMEINPUT_S_OK && r2 != nullptr);
    OS_REQUIRE(r2->GetTimestamp() >= r1->GetTimestamp());
    OS_REQUIRE(r2->GetSequenceNumber(all_kinds) > r1->GetSequenceNumber(all_kinds));

    // Per-device ring buffer traversal: GetPreviousReading & GetNextReading
    IGameInputReading* prev = nullptr;
    OS_REQUIRE(gi->GetPreviousReading(r2, all_kinds, gp_dev, &prev) == GAMEINPUT_S_OK && prev != nullptr);
    OS_REQUIRE(prev->GetSequenceNumber(all_kinds) == r1->GetSequenceNumber(all_kinds));
    prev->Release();

    IGameInputReading* next = nullptr;
    OS_REQUIRE(gi->GetNextReading(r1, all_kinds, gp_dev, &next) == GAMEINPUT_S_OK && next != nullptr);
    OS_REQUIRE(next->GetSequenceNumber(all_kinds) == r2->GetSequenceNumber(all_kinds));
    next->Release();

    // Dedicated Keyboard & Mouse readings
    IGameInputReading* kb_r = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindKeyboard, nullptr, &kb_r) == GAMEINPUT_S_OK && kb_r != nullptr);
    OS_REQUIRE(kb_r->GetKeyCount() == 1u);
    IGameInputReading* ms_r = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindMouse, nullptr, &ms_r) == GAMEINPUT_S_OK && ms_r != nullptr);
    GameInputMouseState got_m2{};
    OS_REQUIRE(ms_r->GetMouseState(&got_m2) && got_m2.positionX == 52 && got_m2.positionY == -13);
    kb_r->Release();
    ms_r->Release();

    // 4B. Hot-plug RegisterDeviceCallback / StopCallback / UnregisterCallback via COM QI
    TestIGameInputEx* gi_ex = nullptr;
    OS_REQUIRE(gi->QueryInterface(&kIidGameInputEx, reinterpret_cast<void**>(&gi_ex)) == GAMEINPUT_S_OK);
    OS_REQUIRE(gi_ex != nullptr);

    HotPlugLog hp_log{};
    uint64_t cb_token = 0ull;
    // enumerationKind == 2 (GameInputBlockingEnumeration) delivers initial connected devices synchronously!
    OS_REQUIRE(gi_ex->RegisterDeviceCallback(
                   nullptr,
                   GameInputKindUnknown,
                   GameInputDeviceConnected,
                   2u,
                   &hp_log,
                   &OnHotPlugDeviceEvent,
                   &cb_token) == GAMEINPUT_S_OK);
    OS_REQUIRE(cb_token != 0ull);
    {
        std::lock_guard<std::mutex> lk(hp_log.mutex);
        OS_REQUIRE(hp_log.records.size() >= 3u);  // Gamepad + Keyboard + Mouse initially connected
    }

#if defined(_WIN32)
    // Simulate hot-plug arrival & removal of a 2nd Xbox controller (slot 3)
    IGameInputDevice* hp_dev = nullptr;
    OS_REQUIRE(fn_hotplug(gi, 3u, GameInputKindGamepad, 1, &hp_dev) == GAMEINPUT_S_OK && hp_dev != nullptr);
    OS_REQUIRE((hp_dev->GetDeviceStatus() & GameInputDeviceConnected) != 0u);
    {
        std::lock_guard<std::mutex> lk(hp_log.mutex);
        const auto& last = hp_log.records.back();
        OS_REQUIRE(last.device == hp_dev);
        OS_REQUIRE((last.current_status & GameInputDeviceConnected) != 0u);
        OS_REQUIRE(last.previous_status == GameInputDeviceNoStatus);
    }

    // Disconnect slot 3 -> callback fires removal & GetCurrentReading rejects disconnected device!
    OS_REQUIRE(fn_hotplug(gi, 3u, GameInputKindGamepad, 0, nullptr) == GAMEINPUT_S_OK);
    OS_REQUIRE((hp_dev->GetDeviceStatus() & GameInputDeviceConnected) == 0u);
    {
        std::lock_guard<std::mutex> lk(hp_log.mutex);
        const auto& last = hp_log.records.back();
        OS_REQUIRE(last.device == hp_dev);
        OS_REQUIRE(last.current_status == GameInputDeviceNoStatus);
        OS_REQUIRE((last.previous_status & GameInputDeviceConnected) != 0u);
    }
    IGameInputReading* disc_r = nullptr;
    OS_REQUIRE(gi->GetCurrentReading(GameInputKindGamepad, hp_dev, &disc_r) == GAMEINPUT_E_DEVICE_DISCONNECTED);

    // StopCallback suppresses further notifications, then UnregisterCallback removes token
    gi_ex->StopCallback(cb_token);
    const size_t count_before_stop = hp_log.records.size();
    OS_REQUIRE(fn_hotplug(gi, 3u, GameInputKindGamepad, 1, nullptr) == GAMEINPUT_S_OK);
    OS_REQUIRE(hp_log.records.size() == count_before_stop);
    OS_REQUIRE(gi_ex->UnregisterCallback(cb_token, 1000ull));
    hp_dev->Release();

    // 4C. Impulse-trigger (LT / RT) SetRumbleState forwarding + telemetry verification
    GameInputRumbleParams lt_only{0.0f, 0.0f, 0.85f, 0.0f};
    gp_dev->SetRumbleState(&lt_only);
    TestRumbleTelemetry rtel{};
    OS_REQUIRE(fn_rumble_tel(gp_dev, &rtel) == GAMEINPUT_S_OK);
    OS_REQUIRE(std::fabs(rtel.lastParams.leftTrigger - 0.85f) < 1e-4f);
    OS_REQUIRE(rtel.xinputLeftMotor > 20000u && rtel.xinputRightMotor == 0u);
    OS_REQUIRE(rtel.impulseTriggerUpdateCount == 1ull);

    GameInputRumbleParams rt_only{0.0f, 0.0f, 0.0f, 0.90f};
    gp_dev->SetRumbleState(&rt_only);
    OS_REQUIRE(fn_rumble_tel(gp_dev, &rtel) == GAMEINPUT_S_OK);
    OS_REQUIRE(std::fabs(rtel.lastParams.rightTrigger - 0.90f) < 1e-4f);
    OS_REQUIRE(rtel.xinputLeftMotor == 0u && rtel.xinputRightMotor > 20000u);
    OS_REQUIRE(rtel.impulseTriggerUpdateCount == 2ull);

    GameInputRumbleParams full_rumble{0.50f, 0.40f, 0.70f, 0.60f};
    gp_dev->SetRumbleState(&full_rumble);
    OS_REQUIRE(fn_rumble_tel(gp_dev, &rtel) == GAMEINPUT_S_OK);
    OS_REQUIRE(rtel.xinputLeftMotor > 32768u && rtel.xinputRightMotor > 26000u);
    OS_REQUIRE(rtel.impulseTriggerUpdateCount == 3ull);

    GameInputRumbleParams zero_rumble{0.0f, 0.0f, 0.0f, 0.0f};
    gp_dev->SetRumbleState(&zero_rumble);
    OS_REQUIRE(fn_rumble_tel(gp_dev, &rtel) == GAMEINPUT_S_OK);
    OS_REQUIRE(rtel.xinputLeftMotor == 0u && rtel.xinputRightMotor == 0u);
    OS_REQUIRE(rtel.updateCount == 4ull);
#endif

    gi_ex->Release();
    r2->Release();
    gp_dev->Release();
    r1->Release();
    gi->Release();
    std::printf("  Simultaneous GP+KB+Mouse+UiNav + Hot-Plug + LT/RT Impulse Rumble OK\n");
}

}  // namespace

int main() {
#if defined(_WIN32)
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestMfPlatVideoAndMediaEngine();
#endif
    TestXAudio2Concurrent64VoicesAndEffects();
    TestBink2MultiFrameStreaming();
    TestGameInputLifecycle();
#if defined(_WIN32)
    CoUninitialize();
#endif
    std::printf("\n[PASS] test_media_video_gameinput (mfplat_xs + 64-voice XAudio2_9 + Bink2 36f + GameInput)\n");
    return 0;
}
