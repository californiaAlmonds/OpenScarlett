// OpenScarlett MIT Playable Xbox Series X|S (Scarlett) Platform Implementation.
// Pure standalone Xbox-ABI host (does NOT link openscarlett_core):
//   - d3d12_xs.dll    : D3D12CreateDeviceX + D3D12 textured fullscreen quad shader pass + PresentX
//   - kernelx.dll     : XMemAlloc / XMemFree + GetConsoleType + TitleMemoryStatus
//   - xgameruntime.dll: XGameRuntimeInitialize + XTaskQueue + XUser + XDisplay + XGameSave (S:\)
//   - GameInput.dll   : Live keyboard & Xbox gamepad input + automated verification injection
//   - XAudio2_9.dll   : Live WASAPI streaming SFX output via IXAudio2SourceVoice

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "openscarlett/audio/xaudio2_abi.h"
#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "scarlett_platform.h"

namespace {

constexpr uint32_t kMaxKeyQueue = 256u;
constexpr uint32_t kAudioSampleRate = 44100u;
constexpr uint32_t kAudioRingSlots = 8u;
constexpr float kPi = 3.14159265358979323846f;

// Minimal HLSL SM 5.0 fullscreen triangle shader that samples the uploaded game framebuffer texture.
// Works identically on both Hardware D3D12 GPUs (GTX 1650 Ti) and D3D12 WARP with zero external files.
constexpr const char* kBlitShaderHlsl = R"HLSL(
Texture2D<float4> g_fb : register(t0);
SamplerState g_samp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut VsMain(uint vid : SV_VertexID) {
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float4 PsMain(VsOut i) : SV_Target0 {
    float4 c = g_fb.Sample(g_samp, i.uv);
    return float4(c.rgb, 1.0f);
}
)HLSL";

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

// Dynamic kernelx.dll ABI bindings (avoids any Win32 symbol collision with kernel32/xgameruntime).
struct KernelXTitleMemoryStatus {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullLegacyUsed;
    uint64_t ullLegacyPeak;
    uint64_t ullLegacyAvail;
    uint64_t ullTitleUsed;
    uint64_t ullTitleAvail;
};

using PfnGetConsoleType = uint32_t(__stdcall*)();
using PfnXGetProcessorCount = uint32_t(__stdcall*)();
using PfnTitleMemoryStatus = int32_t(__stdcall*)(KernelXTitleMemoryStatus*);
using PfnXMemAlloc = void*(__stdcall*)(size_t, uint64_t);
using PfnXMemFree = void(__stdcall*)(void*, uint64_t);

struct KeyEventItem {
    uint8_t pressed;
    uint8_t key;
};

struct ScarlettPlatformState {
    bool initialized = false;
    ScarlettPlatformConfig cfg{};

    // Timing
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    uint32_t virtual_ticks_ms = 0;

    // Win32 window
    HWND hwnd = nullptr;
    bool should_quit = false;

    // kernelx.dll
    HMODULE h_kernelx = nullptr;
    PfnXMemAlloc fn_xmem_alloc = nullptr;
    PfnXMemFree fn_xmem_free = nullptr;
    uint8_t* staging_rgba = nullptr;  // Allocated via kernelx.dll!XMemAlloc
    size_t staging_rgba_bytes = 0;

    // xgameruntime.dll
    bool xgr_inited = false;
    XTaskQueueHandle task_queue = nullptr;
    XUserHandle user = nullptr;
    XDisplayTimeoutDeferralHandle display_deferral = nullptr;
    XGameSaveProviderHandle save_provider = nullptr;
    std::string save_container = "scarlett_game";

    // GameInput.dll
    IGameInput* game_input = nullptr;
    std::array<KeyEventItem, kMaxKeyQueue> key_queue{};
    uint32_t key_read = 0;
    uint32_t key_write = 0;
    std::array<uint8_t, 256> win32_key_state{};
    std::array<uint8_t, 256> gi_key_state{};
    std::array<uint8_t, 256> combined_key_state{};

    // XAudio2_9.dll
    IXAudio2* xaudio2 = nullptr;
    IXAudio2MasteringVoice* master_voice = nullptr;
    IXAudio2SourceVoice* sfx_voice = nullptr;
    std::array<std::vector<int16_t>, kAudioRingSlots> audio_ring{};
    uint32_t audio_ring_next = 0;

    // d3d12_xs.dll
    ComPtr<ID3D12DeviceX> device;
    ComPtr<ID3D12CommandQueueX> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandListX> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    ComPtr<ID3D12RootSignature> root_sig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12Resource> rt_tex;
    ComPtr<ID3D12Resource> fb_tex;
    ComPtr<ID3D12Resource> upload_buf;
    uint32_t upload_row_pitch = 0;
    uint64_t upload_total_bytes = 0;
    D3D12_RESOURCE_STATES fb_tex_state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES rt_tex_state = D3D12_RESOURCE_STATE_COMMON;

    // Stats & readback cache
    ScarlettPlatformStats stats{};
    std::set<uint64_t> unique_hashes;
    std::vector<uint8_t> last_readback_rgba;
    uint32_t last_readback_w = 0;
    uint32_t last_readback_h = 0;
};

ScarlettPlatformState g_plat;

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

uint8_t TranslateVkToScarlettKey(uint32_t vk) {
    switch (vk) {
        case VK_RETURN: return static_cast<uint8_t>(SCARLETT_KEY_ENTER);
        case VK_ESCAPE: return static_cast<uint8_t>(SCARLETT_KEY_ESCAPE);
        case VK_TAB:    return static_cast<uint8_t>(SCARLETT_KEY_TAB);
        case VK_UP:
        case 'W':       return static_cast<uint8_t>(SCARLETT_KEY_UP);
        case VK_DOWN:
        case 'S':       return static_cast<uint8_t>(SCARLETT_KEY_DOWN);
        case 'A':       return static_cast<uint8_t>(SCARLETT_KEY_STRAFE_L);
        case 'D':       return static_cast<uint8_t>(SCARLETT_KEY_STRAFE_R);
        case VK_LEFT:   return static_cast<uint8_t>(SCARLETT_KEY_LEFT);
        case VK_RIGHT:  return static_cast<uint8_t>(SCARLETT_KEY_RIGHT);
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case 'F':       return static_cast<uint8_t>(SCARLETT_KEY_FIRE);
        case VK_SPACE:
        case 'E':       return static_cast<uint8_t>(SCARLETT_KEY_USE);
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: return static_cast<uint8_t>(SCARLETT_KEY_RSHIFT);
        case VK_MENU:   return static_cast<uint8_t>(SCARLETT_KEY_RALT);
        case VK_F1:     return static_cast<uint8_t>(SCARLETT_KEY_F1);
        case VK_F2:     return static_cast<uint8_t>(SCARLETT_KEY_F2);
        case VK_F3:     return static_cast<uint8_t>(SCARLETT_KEY_F3);
        case VK_F4:     return static_cast<uint8_t>(SCARLETT_KEY_F4);
        case VK_F5:     return static_cast<uint8_t>(SCARLETT_KEY_F5);
        case VK_F6:     return static_cast<uint8_t>(SCARLETT_KEY_F6);
        default:
            if (vk >= '0' && vk <= '9') return static_cast<uint8_t>(vk);
            if (vk >= 'A' && vk <= 'Z') return static_cast<uint8_t>(vk - 'A' + 'a');
            return 0u;
    }
}

void PushKeyEvent(uint8_t pressed, uint8_t key) {
    if (key == 0u) return;
    const uint32_t next_write = (g_plat.key_write + 1u) % kMaxKeyQueue;
    if (next_write == g_plat.key_read) {
        return;  // Queue full
    }
    g_plat.key_queue[g_plat.key_write] = KeyEventItem{pressed, key};
    g_plat.key_write = next_write;
    g_plat.stats.input_events_queued += 1ull;
}

void ReconcileInputState() {
    for (size_t k = 1; k < 256u; ++k) {
        const uint8_t desired = (g_plat.win32_key_state[k] | g_plat.gi_key_state[k]) ? 1u : 0u;
        if (desired != g_plat.combined_key_state[k]) {
            g_plat.combined_key_state[k] = desired;
            PushKeyEvent(desired, static_cast<uint8_t>(k));
            if (desired && k == static_cast<size_t>(SCARLETT_KEY_FIRE)) {
                ScarlettPlatformPlaySfx(165.0f, 55u, 0.18f);
            } else if (desired && (k == static_cast<size_t>(SCARLETT_KEY_ENTER) ||
                                   k == static_cast<size_t>(SCARLETT_KEY_USE))) {
                ScarlettPlatformPlaySfx(520.0f, 35u, 0.12f);
            }
        }
    }
}

LRESULT CALLBACK ScarlettWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            g_plat.should_quit = true;
            return 0;
        case WM_DESTROY:
            g_plat.should_quit = true;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            // Allow Alt+Enter to pass through to PresentX's borderless fullscreen handler!
            if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN) {
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
            }
            const uint8_t key = TranslateVkToScarlettKey(static_cast<uint32_t>(wParam));
            if (key != 0u) {
                g_plat.win32_key_state[key] = 1u;
                ReconcileInputState();
            }
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const uint8_t key = TranslateVkToScarlettKey(static_cast<uint32_t>(wParam));
            if (key != 0u) {
                g_plat.win32_key_state[key] = 0u;
                ReconcileInputState();
            }
            return 0;
        }
        case WM_KILLFOCUS:
            g_plat.win32_key_state.fill(0u);
            ReconcileInputState();
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool CreateGameWindow() {
    static const wchar_t* kClassName = L"OpenScarlettPlayableGameWndClass";
    static bool s_class_registered = false;
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    if (!s_class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ScarlettWindowProc;
        wc.hInstance = hinst;
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0) {
            return false;
        }
        s_class_registered = true;
    }

    const uint32_t w = g_plat.cfg.window_width ? g_plat.cfg.window_width : g_plat.cfg.fb_width;
    const uint32_t h = g_plat.cfg.window_height ? g_plat.cfg.window_height : g_plat.cfg.fb_height;
    RECT wr{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    ::AdjustWindowRectEx(&wr, style, FALSE, 0);

    std::wstring title = Utf8ToWide(g_plat.cfg.window_title ? g_plat.cfg.window_title
                                                            : "OpenScarlett — Xbox Series X|S");
    g_plat.hwnd = ::CreateWindowExW(
        0,
        kClassName,
        title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);
    if (!g_plat.hwnd) {
        return false;
    }

    if (!g_plat.cfg.headless && g_plat.cfg.verify_ticks <= 0) {
        ::ShowWindow(g_plat.hwnd, SW_SHOW);
        ::UpdateWindow(g_plat.hwnd);
    }
    return true;
}

bool InitKernelX() {
    g_plat.h_kernelx = ::LoadLibraryA("kernelx.dll");
    if (!g_plat.h_kernelx) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: LoadLibraryA(kernelx.dll) err=%lu\n",
                     static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    auto p_console = reinterpret_cast<PfnGetConsoleType>(
        ::GetProcAddress(g_plat.h_kernelx, "GetConsoleType"));
    auto p_procs = reinterpret_cast<PfnXGetProcessorCount>(
        ::GetProcAddress(g_plat.h_kernelx, "XGetProcessorCount"));
    auto p_tms = reinterpret_cast<PfnTitleMemoryStatus>(
        ::GetProcAddress(g_plat.h_kernelx, "TitleMemoryStatus"));
    g_plat.fn_xmem_alloc = reinterpret_cast<PfnXMemAlloc>(
        ::GetProcAddress(g_plat.h_kernelx, "XMemAlloc"));
    g_plat.fn_xmem_free = reinterpret_cast<PfnXMemFree>(
        ::GetProcAddress(g_plat.h_kernelx, "XMemFree"));

    if (!p_console || !p_procs || !p_tms || !g_plat.fn_xmem_alloc || !g_plat.fn_xmem_free) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: kernelx.dll missing expected exports\n");
        return false;
    }

    g_plat.stats.console_type = p_console();
    (void)p_procs();
    KernelXTitleMemoryStatus tms{};
    tms.dwLength = static_cast<uint32_t>(sizeof(KernelXTitleMemoryStatus));
    if (p_tms(&tms) == 0) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: kernelx TitleMemoryStatus failed\n");
        return false;
    }

    // Allocate the game's RGBA8 staging framebuffer through Xbox unified XMemAlloc!
    g_plat.staging_rgba_bytes = static_cast<size_t>(g_plat.cfg.fb_width) * g_plat.cfg.fb_height * 4u;
    g_plat.staging_rgba = static_cast<uint8_t*>(
        g_plat.fn_xmem_alloc(g_plat.staging_rgba_bytes, 0ull));
    if (!g_plat.staging_rgba) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: kernelx XMemAlloc failed\n");
        return false;
    }
    std::memset(g_plat.staging_rgba, 0, g_plat.staging_rgba_bytes);
    return true;
}

bool InitXGameRuntime() {
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: XGameRuntimeInitialize\n");
        return false;
    }
    g_plat.xgr_inited = true;

    (void)XSystemGetDeviceType();
    uint32_t tid = 0;
    if (XGameGetXboxTitleId(&tid) == XGR_S_OK) {
        g_plat.stats.title_id = tid;
    }

    if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                         XTaskQueueDispatchMode_Manual,
                         &g_plat.task_queue) != XGR_S_OK || !g_plat.task_queue) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: XTaskQueueCreate\n");
        return false;
    }

    // Sign in default Scarlett profile silently via XUserAddAsync
    XAsyncBlock user_async{};
    user_async.queue = g_plat.task_queue;
    if (XUserAddAsync(XUserAddOptions_AddDefaultUserSilently, &user_async) != XGR_S_OK) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: XUserAddAsync\n");
        return false;
    }
    while (XAsyncGetStatus(&user_async, false) == XGR_E_PENDING) {
        XTaskQueueDispatch(g_plat.task_queue, XTaskQueuePort_Completion, 10u);
    }
    if (XUserAddResult(&user_async, &g_plat.user) != XGR_S_OK || !g_plat.user) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: XUserAddResult\n");
        return false;
    }
    size_t gt_used = 0;
    (void)XUserGetGamertag(g_plat.user, XUserGamertagComponent_Classic,
                           sizeof(g_plat.stats.gamertag), g_plat.stats.gamertag, &gt_used);

    // Keep console display awake during active gameplay
    (void)XDisplayTryAcquireTimeoutDeferral(&g_plat.display_deferral);

    // Initialize XGameSave provider asynchronously (persists saves to S:\ / OPENSCARLETT_SAVE_ROOT)
    XAsyncBlock save_async{};
    save_async.queue = g_plat.task_queue;
    if (XGameSaveInitializeProviderAsync(
            g_plat.user, "00000000-0000-0000-0000-00005ca0d001", false, &save_async) == XGR_S_OK) {
        while (XAsyncGetStatus(&save_async, false) == XGR_E_PENDING) {
            XTaskQueueDispatch(g_plat.task_queue, XTaskQueuePort_Completion, 10u);
        }
        (void)XGameSaveInitializeProviderResult(&save_async, &g_plat.save_provider);
    }
    return true;
}

bool InitGameInput() {
    if (GameInputCreate(&g_plat.game_input) != GAMEINPUT_S_OK || !g_plat.game_input) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: GameInputCreate\n");
        return false;
    }
    (void)OpenScarlettGameInputSetTargetWindow(g_plat.game_input, g_plat.hwnd);
    g_plat.game_input->SetFocusPolicy(
        (g_plat.cfg.headless || g_plat.cfg.verify_ticks > 0)
            ? GameInputEnableBackgroundInput
            : GameInputDefaultFocusPolicy);
    return true;
}

bool InitXAudio2() {
    if (XAudio2Create(&g_plat.xaudio2, 0u, OS_XAUDIO2_DEFAULT_PROCESSOR) != OS_XAUDIO2_S_OK ||
        !g_plat.xaudio2) {
        std::fprintf(stderr, "[scarlett_platform] WARN: XAudio2Create failed; continuing silent\n");
        return true;
    }
    if (g_plat.xaudio2->CreateMasteringVoice(&g_plat.master_voice) != OS_XAUDIO2_S_OK ||
        !g_plat.master_voice) {
        return true;
    }
    OS_WAVEFORMATEX wfx{};
    wfx.wFormatTag = OS_WAVE_FORMAT_PCM;
    wfx.nChannels = 1u;
    wfx.nSamplesPerSec = kAudioSampleRate;
    wfx.wBitsPerSample = 16u;
    wfx.nBlockAlign = static_cast<uint16_t>((wfx.nChannels * wfx.wBitsPerSample) / 8u);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0u;
    if (g_plat.xaudio2->CreateSourceVoice(&g_plat.sfx_voice, &wfx) == OS_XAUDIO2_S_OK &&
        g_plat.sfx_voice) {
        (void)g_plat.sfx_voice->Start(0u, OS_XAUDIO2_COMMIT_NOW);
    }
    return true;
}

void WaitForGpuIdle() {
    if (!g_plat.queue || !g_plat.fence || !g_plat.fence_event) return;
    g_plat.fence_value += 1ull;
    if (SUCCEEDED(g_plat.queue->Signal(g_plat.fence.Get(), g_plat.fence_value))) {
        if (g_plat.fence->GetCompletedValue() < g_plat.fence_value) {
            (void)g_plat.fence->SetEventOnCompletion(g_plat.fence_value, g_plat.fence_event);
            ::WaitForSingleObject(g_plat.fence_event, 10000u);
        }
    }
}

bool InitD3D12X() {
    OpenScarlettD3D12SetPreferWarp(g_plat.cfg.prefer_warp ? 1 : 0);

    // Use standard Xbox Series X|S D3D12CreateDeviceX entry point.
    HRESULT hr = D3D12CreateDeviceX(
        nullptr,
        D3D_FEATURE_LEVEL_12_0,
        IID_ID3D12DeviceX,
        reinterpret_cast<void**>(g_plat.device.Put()));
    if (FAILED(hr) || !g_plat.device) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: D3D12CreateDeviceX hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    if (SUCCEEDED(g_plat.device->GetGpuHardwareInfoX(&gpu_info))) {
        std::snprintf(g_plat.stats.adapter_name, sizeof(g_plat.stats.adapter_name),
                      "%s", gpu_info.AdapterName);
    }

    // 60 FPS pacing in interactive mode; uncapped in automated verify/headless mode.
    const uint32_t interval_us = (g_plat.cfg.headless || g_plat.cfg.verify_ticks > 0) ? 0u : 16667u;
    (void)g_plat.device->SetFrameIntervalX(nullptr, interval_us, 2u, 0u);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_plat.device->CreateCommandQueue(
        &qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(g_plat.queue.Put()));
    if (FAILED(hr) || !g_plat.queue) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: CreateCommandQueue hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    // Attach our Win32 window to PresentX swapchain.
    D3D12XBOX_PRESENTX_CONFIG pcfg{};
    pcfg.OutputWindow = g_plat.hwnd;
    pcfg.PreferredFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    pcfg.ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    (void)OpenScarlettD3D12SetPresentConfig(g_plat.queue.Get(), &pcfg);

    // Enable readback capture only when verifying frames; disable in interactive play for max 60 FPS throughput.
    {
        ComPtr<IOpenScarlettPresentCapture> cap;
        if (SUCCEEDED(g_plat.queue->QueryInterface(
                IID_IOpenScarlettPresentCapture, reinterpret_cast<void**>(cap.Put()))) && cap) {
            (void)cap->SetReadbackCaptureEnabled(
                (g_plat.cfg.verify_ticks > 0 || g_plat.cfg.headless) ? TRUE : FALSE);
        }
    }

    hr = g_plat.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(g_plat.alloc.Put()));
    if (FAILED(hr)) return false;

    hr = g_plat.device->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, g_plat.alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(g_plat.cmd.Put()));
    if (FAILED(hr)) return false;
    (void)g_plat.cmd->Close();

    hr = g_plat.device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_plat.fence.Put()));
    if (FAILED(hr)) return false;
    g_plat.fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_plat.fence_event) return false;

    // Compile fullscreen blit VS + PS via D3DCompile (SM 5.0 DXBC).
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> err_blob;
    const size_t hlsl_len = std::strlen(kBlitShaderHlsl);
    hr = ::D3DCompile(kBlitShaderHlsl, hlsl_len, "scarlett_blit.hlsl", nullptr, nullptr,
                      "VsMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u,
                      vs_blob.Put(), err_blob.Put());
    if (FAILED(hr) || !vs_blob) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: D3DCompile VsMain hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    err_blob.Reset();
    hr = ::D3DCompile(kBlitShaderHlsl, hlsl_len, "scarlett_blit.hlsl", nullptr, nullptr,
                      "PsMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u,
                      ps_blob.Put(), err_blob.Put());
    if (FAILED(hr) || !ps_blob) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: D3DCompile PsMain hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    // Root Signature: 1 descriptor table (SRV t0) + 1 static point/linear sampler (s0)
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1u;
    srv_range.BaseShaderRegister = 0u;
    srv_range.RegisterSpace = 0u;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.DescriptorTable.NumDescriptorRanges = 1u;
    root_param.DescriptorTable.pDescriptorRanges = &srv_range;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0u;
    sampler.RegisterSpace = 0u;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1u;
    rs_desc.pParameters = &root_param;
    rs_desc.NumStaticSamplers = 1u;
    rs_desc.pStaticSamplers = &sampler;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rs_blob;
    err_blob.Reset();
    hr = ::D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                       rs_blob.Put(), err_blob.Put());
    if (FAILED(hr) || !rs_blob) return false;

    hr = g_plat.device->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        IID_PPV_ARGS(g_plat.root_sig.Put()));
    if (FAILED(hr) || !g_plat.root_sig) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = g_plat.root_sig.Get();
    pso_desc.VS.pShaderBytecode = vs_blob->GetBufferPointer();
    pso_desc.VS.BytecodeLength = vs_blob->GetBufferSize();
    pso_desc.PS.pShaderBytecode = ps_blob->GetBufferPointer();
    pso_desc.PS.BytecodeLength = ps_blob->GetBufferSize();
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1u;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1u;

    hr = g_plat.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(g_plat.pso.Put()));
    if (FAILED(hr) || !g_plat.pso) {
        std::fprintf(stderr, "[scarlett_platform] FAIL: CreateGraphicsPipelineState hr=0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    // Create RTV & shader-visible SRV descriptor heaps.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.NumDescriptors = 1u;
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = g_plat.device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(g_plat.rtv_heap.Put()));
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_hd{};
    srv_hd.NumDescriptors = 1u;
    srv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_plat.device->CreateDescriptorHeap(&srv_hd, IID_PPV_ARGS(g_plat.srv_heap.Put()));
    if (FAILED(hr)) return false;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // 1. Render Target Texture (presented via PresentX)
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = g_plat.cfg.fb_width;
    rt_desc.Height = g_plat.cfg.fb_height;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    hr = g_plat.device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(g_plat.rt_tex.Put()));
    if (FAILED(hr)) return false;
    g_plat.rt_tex_state = D3D12_RESOURCE_STATE_COMMON;
    g_plat.device->CreateRenderTargetView(
        g_plat.rt_tex.Get(), nullptr, g_plat.rtv_heap->GetCPUDescriptorHandleForHeapStart());

    // 2. Game Framebuffer SRV Texture
    D3D12_RESOURCE_DESC fb_desc{};
    fb_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fb_desc.Width = g_plat.cfg.fb_width;
    fb_desc.Height = g_plat.cfg.fb_height;
    fb_desc.DepthOrArraySize = 1u;
    fb_desc.MipLevels = 1u;
    fb_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fb_desc.SampleDesc.Count = 1u;
    fb_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    fb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = g_plat.device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &fb_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(g_plat.fb_tex.Put()));
    if (FAILED(hr)) return false;
    g_plat.fb_tex_state = D3D12_RESOURCE_STATE_COMMON;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1u;
    g_plat.device->CreateShaderResourceView(
        g_plat.fb_tex.Get(), &srv_desc, g_plat.srv_heap->GetCPUDescriptorHandleForHeapStart());

    // 3. Upload Heap Staging Buffer (256-byte row-pitch aligned)
    const uint32_t raw_row = g_plat.cfg.fb_width * 4u;
    g_plat.upload_row_pitch = (raw_row + 255u) & ~255u;
    g_plat.upload_total_bytes = static_cast<uint64_t>(g_plat.upload_row_pitch) * g_plat.cfg.fb_height;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = g_plat.upload_total_bytes;
    up_desc.Height = 1u;
    up_desc.DepthOrArraySize = 1u;
    up_desc.MipLevels = 1u;
    up_desc.Format = DXGI_FORMAT_UNKNOWN;
    up_desc.SampleDesc.Count = 1u;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = g_plat.device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(g_plat.upload_buf.Put()));
    if (FAILED(hr)) return false;

    return true;
}

bool WriteWholeFileUtf8(const char* path_utf8, const void* data, size_t size) {
    const std::wstring wpath = Utf8ToWide(path_utf8);
    if (wpath.empty()) return false;
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
        DWORD chunk = 0;
        const DWORD req = static_cast<DWORD>(std::min<size_t>(size - done, 16u * 1024u * 1024u));
        if (!::WriteFile(h, p + done, req, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        done += chunk;
    }
    ::CloseHandle(h);
    return true;
}

}  // namespace

extern "C" {

int ScarlettPlatformInit(const ScarlettPlatformConfig* config) {
    if (g_plat.initialized) return 1;
    if (!config || config->fb_width == 0u || config->fb_height == 0u) return 0;

    g_plat.cfg = *config;
    if (config->save_container && config->save_container[0] != '\0') {
        g_plat.save_container = config->save_container;
    } else {
        g_plat.save_container = "scarlett_game";
    }
    ::QueryPerformanceFrequency(&g_plat.qpc_freq);
    ::QueryPerformanceCounter(&g_plat.qpc_start);
    g_plat.virtual_ticks_ms = 0u;
    g_plat.should_quit = false;

    if (!CreateGameWindow()) return 0;
    if (!InitKernelX()) return 0;
    if (!InitXGameRuntime()) return 0;
    if (!InitGameInput()) return 0;
    if (!InitXAudio2()) return 0;
    if (!InitD3D12X()) return 0;

    // Play brief welcoming startup chime through XAudio2_9.dll (safe on null audio too).
    ScarlettPlatformPlaySfx(440.0f, 40u, 0.10f);

    g_plat.initialized = true;
    return 1;
}

void ScarlettPlatformShutdown(void) {
    if (!g_plat.initialized) return;

    WaitForGpuIdle();

    g_plat.upload_buf.Reset();
    g_plat.fb_tex.Reset();
    g_plat.rt_tex.Reset();
    g_plat.srv_heap.Reset();
    g_plat.rtv_heap.Reset();
    g_plat.pso.Reset();
    g_plat.root_sig.Reset();
    if (g_plat.fence_event) {
        ::CloseHandle(g_plat.fence_event);
        g_plat.fence_event = nullptr;
    }
    g_plat.fence.Reset();
    g_plat.cmd.Reset();
    g_plat.alloc.Reset();
    g_plat.queue.Reset();
    g_plat.device.Reset();
    OpenScarlettD3D12SetPreferWarp(0);

    if (g_plat.sfx_voice) {
        (void)g_plat.sfx_voice->Stop(0u, OS_XAUDIO2_COMMIT_NOW);
        g_plat.sfx_voice->DestroyVoice();
        g_plat.sfx_voice = nullptr;
    }
    if (g_plat.master_voice) {
        g_plat.master_voice->DestroyVoice();
        g_plat.master_voice = nullptr;
    }
    if (g_plat.xaudio2) {
        g_plat.xaudio2->Release();
        g_plat.xaudio2 = nullptr;
    }

    if (g_plat.game_input) {
        (void)OpenScarlettGameInputInjectTestState(g_plat.game_input, nullptr, nullptr, 0u, 0);
        g_plat.game_input->Release();
        g_plat.game_input = nullptr;
    }

    if (g_plat.save_provider) {
        XGameSaveCloseProvider(g_plat.save_provider);
        g_plat.save_provider = nullptr;
    }
    if (g_plat.display_deferral) {
        XDisplayCloseTimeoutDeferralHandle(g_plat.display_deferral);
        g_plat.display_deferral = nullptr;
    }
    if (g_plat.user) {
        XUserCloseHandle(g_plat.user);
        g_plat.user = nullptr;
    }
    if (g_plat.task_queue) {
        XTaskQueueCloseHandle(g_plat.task_queue);
        g_plat.task_queue = nullptr;
    }
    if (g_plat.xgr_inited) {
        XGameRuntimeUninitialize();
        g_plat.xgr_inited = false;
    }

    if (g_plat.staging_rgba && g_plat.fn_xmem_free) {
        g_plat.fn_xmem_free(g_plat.staging_rgba, 0ull);
        g_plat.staging_rgba = nullptr;
    }
    if (g_plat.h_kernelx) {
        ::FreeLibrary(g_plat.h_kernelx);
        g_plat.h_kernelx = nullptr;
    }

    if (g_plat.hwnd) {
        ::DestroyWindow(g_plat.hwnd);
        g_plat.hwnd = nullptr;
    }

    g_plat.initialized = false;
}

int ScarlettPlatformShouldQuit(void) {
    return g_plat.should_quit ? 1 : 0;
}

void ScarlettPlatformSetWindowTitle(const char* title_utf8) {
    if (!g_plat.hwnd || !title_utf8) return;
    std::wstring wtitle = Utf8ToWide(title_utf8);
    if (!wtitle.empty()) {
        ::SetWindowTextW(g_plat.hwnd, wtitle.c_str());
    }
}

uint32_t ScarlettPlatformGetTicksMs(void) {
    if (g_plat.cfg.verify_ticks > 0) {
        // Advance virtual game clock smoothly (35 Hz DOOM ticrate = ~29 ms per call) so
        // automated verification runs deterministically at full speed without wall-clock stalling.
        g_plat.virtual_ticks_ms += 15u;
        return g_plat.virtual_ticks_ms;
    }
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    if (g_plat.qpc_freq.QuadPart == 0) return 0u;
    const uint64_t ms = static_cast<uint64_t>((now.QuadPart - g_plat.qpc_start.QuadPart) * 1000ll /
                                              g_plat.qpc_freq.QuadPart);
    return static_cast<uint32_t>(ms);
}

void ScarlettPlatformSleepMs(uint32_t ms) {
    if (g_plat.cfg.verify_ticks > 0) {
        g_plat.virtual_ticks_ms += (ms > 0u ? ms : 1u);
        return;
    }
    if (ms > 0u) {
        ::Sleep(ms);
    }
}

void ScarlettPlatformPumpInput(void) {
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0u, 0u, PM_REMOVE) != FALSE) {
        if (msg.message == WM_QUIT) {
            g_plat.should_quit = true;
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    // Dispatch any pending GDK async callbacks on our manual completion port.
    if (g_plat.task_queue) {
        while (XTaskQueueDispatch(g_plat.task_queue, XTaskQueuePort_Completion, 0u)) {
        }
    }

    // Poll GameInput.dll for both Xbox gamepad and keyboard state.
    if (g_plat.game_input) {
        g_plat.gi_key_state.fill(0u);
        IGameInputReading* reading = nullptr;
        const GameInputKind kind =
            static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard);
        if (g_plat.game_input->GetCurrentReading(kind, nullptr, &reading) == GAMEINPUT_S_OK &&
            reading != nullptr) {
            GameInputGamepadState gp{};
            if (reading->GetGamepadState(&gp)) {
                if ((gp.buttons & GameInputGamepadDPadUp) || gp.leftThumbstickY > 0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_UP] = 1u;
                }
                if ((gp.buttons & GameInputGamepadDPadDown) || gp.leftThumbstickY < -0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_DOWN] = 1u;
                }
                if (gp.leftThumbstickX < -0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_STRAFE_L] = 1u;
                }
                if (gp.leftThumbstickX > 0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_STRAFE_R] = 1u;
                }
                if ((gp.buttons & GameInputGamepadDPadLeft) || gp.rightThumbstickX < -0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_LEFT] = 1u;
                }
                if ((gp.buttons & GameInputGamepadDPadRight) || gp.rightThumbstickX > 0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_RIGHT] = 1u;
                }
                if ((gp.buttons & GameInputGamepadX) || gp.rightTrigger > 0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_FIRE] = 1u;
                }
                if (gp.buttons & GameInputGamepadA) {
                    g_plat.gi_key_state[SCARLETT_KEY_USE] = 1u;
                }
                if (gp.buttons & (GameInputGamepadMenu | GameInputGamepadB)) {
                    g_plat.gi_key_state[SCARLETT_KEY_ESCAPE] = 1u;
                }
                if (gp.buttons & GameInputGamepadView) {
                    g_plat.gi_key_state[SCARLETT_KEY_TAB] = 1u;
                }
                if (gp.leftTrigger > 0.35f) {
                    g_plat.gi_key_state[SCARLETT_KEY_RSHIFT] = 1u;
                }
            }

            const uint32_t kcount = reading->GetKeyCount();
            if (kcount > 0u) {
                std::vector<GameInputKeyState> keys(kcount);
                const uint32_t got = reading->GetKeyState(kcount, keys.data());
                for (uint32_t i = 0; i < got; ++i) {
                    const uint8_t sk = TranslateVkToScarlettKey(keys[i].virtualKey);
                    if (sk != 0u) {
                        g_plat.gi_key_state[sk] = 1u;
                    }
                }
            }
            reading->Release();
        }
        ReconcileInputState();
    }
}

int ScarlettPlatformPopKeyEvent(int* out_pressed, unsigned char* out_key) {
    if (!out_pressed || !out_key) return 0;
    if (g_plat.key_read == g_plat.key_write) return 0;
    const KeyEventItem item = g_plat.key_queue[g_plat.key_read];
    g_plat.key_read = (g_plat.key_read + 1u) % kMaxKeyQueue;
    *out_pressed = static_cast<int>(item.pressed);
    *out_key = item.key;
    return 1;
}

int ScarlettPlatformIsKeyHeld(unsigned char key) {
    return g_plat.combined_key_state[key] ? 1 : 0;
}

void ScarlettPlatformInjectVerifyInput(uint32_t tick_index) {
    if (!g_plat.game_input) return;

    GameInputGamepadState gp{};
    std::vector<GameInputKeyState> keys;
    auto add_vk = [&](uint8_t vk) {
        GameInputKeyState ks{};
        ks.virtualKey = vk;
        ks.scanCode = ::MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        ks.codePoint = (vk >= 0x20u && vk <= 0x7Eu) ? vk : 0u;
        ks.isDeadKey = false;
        keys.push_back(ks);
    };

    // Scripted gameplay sequence:
    //   - Early ticks: walk forward (leftStickY + W)
    //   - Mid ticks: turn right + fire weapon (rightTrigger + Ctrl)
    //   - Late ticks: strafe + turn left + open/use (A button + Space)
    if (tick_index >= 5u && tick_index < 25u) {
        gp.leftThumbstickY = 0.9f;
        add_vk('W');
    } else if (tick_index >= 25u && tick_index < 45u) {
        gp.rightThumbstickX = 0.8f;
        gp.rightTrigger = (tick_index % 6u < 3u) ? 0.95f : 0.0f;
        add_vk(VK_RIGHT);
        if (tick_index % 6u < 3u) add_vk(VK_CONTROL);
    } else if (tick_index >= 45u && tick_index < 65u) {
        gp.leftThumbstickY = 0.85f;
        gp.leftThumbstickX = 0.6f;
        gp.buttons = (tick_index % 8u < 2u) ? GameInputGamepadA : GameInputGamepadNone;
        add_vk('W');
        add_vk('D');
    } else if (tick_index >= 65u) {
        gp.rightThumbstickX = -0.8f;
        gp.rightTrigger = (tick_index % 5u < 2u) ? 0.9f : 0.0f;
        add_vk(VK_LEFT);
    }

    (void)OpenScarlettGameInputInjectTestState(
        g_plat.game_input,
        &gp,
        keys.empty() ? nullptr : keys.data(),
        static_cast<uint32_t>(keys.size()),
        1);
}

int ScarlettPlatformPresentFrame(const uint32_t* pixels,
                                 uint32_t width,
                                 uint32_t height,
                                 int is_xrgb_00rrggbb) {
    if (!g_plat.initialized || !pixels) return 0;
    if (width != g_plat.cfg.fb_width || height != g_plat.cfg.fb_height) return 0;

    // 1. Convert input pixels into our kernelx.dll XMemAlloc RGBA8 staging framebuffer.
    const size_t count = static_cast<size_t>(width) * height;
    if (is_xrgb_00rrggbb) {
        uint8_t* dst = g_plat.staging_rgba;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t px = pixels[i];
            dst[i * 4u + 0u] = static_cast<uint8_t>((px >> 16) & 0xFFu);
            dst[i * 4u + 1u] = static_cast<uint8_t>((px >> 8) & 0xFFu);
            dst[i * 4u + 2u] = static_cast<uint8_t>(px & 0xFFu);
            dst[i * 4u + 3u] = 0xFFu;
        }
    } else {
        std::memcpy(g_plat.staging_rgba, pixels, count * 4u);
    }

    // 2. Copy RGBA rows into 256-byte-aligned D3D12 upload buffer.
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    if (FAILED(g_plat.upload_buf->Map(0u, &read_range, &mapped)) || !mapped) {
        return 0;
    }
    const uint32_t src_row_bytes = width * 4u;
    auto* dst_base = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst_base + static_cast<size_t>(y) * g_plat.upload_row_pitch,
                    g_plat.staging_rgba + static_cast<size_t>(y) * src_row_bytes,
                    src_row_bytes);
    }
    g_plat.upload_buf->Unmap(0u, nullptr);

    // 3. Acquire Xbox frame pipeline token via WaitFrameEventX.
    D3D12XBOX_FRAME_PIPELINE_TOKEN token = 0;
    (void)g_plat.device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &token);

    // 4. Record D3D12 upload + fullscreen textured quad shader pass.
    if (FAILED(g_plat.alloc->Reset())) return 0;
    if (FAILED(g_plat.cmd->Reset(g_plat.alloc.Get(), g_plat.pso.Get()))) return 0;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES dst) {
        if (cur == dst) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur;
        b.Transition.StateAfter = dst;
        g_plat.cmd->ResourceBarrier(1u, &b);
        cur = dst;
    };

    transition(g_plat.fb_tex.Get(), g_plat.fb_tex_state, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION copy_dst{};
    copy_dst.pResource = g_plat.fb_tex.Get();
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dst.SubresourceIndex = 0u;

    D3D12_TEXTURE_COPY_LOCATION copy_src{};
    copy_src.pResource = g_plat.upload_buf.Get();
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_src.PlacedFootprint.Offset = 0ull;
    copy_src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    copy_src.PlacedFootprint.Footprint.Width = width;
    copy_src.PlacedFootprint.Footprint.Height = height;
    copy_src.PlacedFootprint.Footprint.Depth = 1u;
    copy_src.PlacedFootprint.Footprint.RowPitch = g_plat.upload_row_pitch;

    g_plat.cmd->CopyTextureRegion(&copy_dst, 0u, 0u, 0u, &copy_src, nullptr);

    transition(g_plat.fb_tex.Get(), g_plat.fb_tex_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(g_plat.rt_tex.Get(), g_plat.rt_tex_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_plat.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_plat.cmd->OMSetRenderTargets(1u, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    g_plat.cmd->RSSetViewports(1u, &vp);
    g_plat.cmd->RSSetScissorRects(1u, &sc);

    g_plat.cmd->SetGraphicsRootSignature(g_plat.root_sig.Get());
    ID3D12DescriptorHeap* heaps[] = {g_plat.srv_heap.Get()};
    g_plat.cmd->SetDescriptorHeaps(1u, heaps);
    g_plat.cmd->SetGraphicsRootDescriptorTable(0u, g_plat.srv_heap->GetGPUDescriptorHandleForHeapStart());
    g_plat.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_plat.cmd->DrawInstanced(3u, 1u, 0u, 0u);

    transition(g_plat.rt_tex.Get(), g_plat.rt_tex_state, D3D12_RESOURCE_STATE_PRESENT);

    if (FAILED(g_plat.cmd->Close())) return 0;

    ID3D12CommandList* lists[] = {g_plat.cmd.Get()};
    g_plat.queue->ExecuteCommandLists(1u, lists);

    // 5. Present via Xbox Series X|S ID3D12CommandQueueX::PresentX!
    ID3D12Resource* plane_res[] = {g_plat.rt_tex.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = token;
    plane.ResourceCount = 1u;
    plane.ppResources = plane_res;
    plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres{};
    pres.Flags = (g_plat.cfg.headless || g_plat.cfg.verify_ticks > 0)
                     ? D3D12XBOX_PRESENT_FLAG_IMMEDIATE
                     : D3D12XBOX_PRESENT_FLAG_NONE;
    pres.SyncInterval = (g_plat.cfg.headless || g_plat.cfg.verify_ticks > 0) ? 0u : 1u;
    pres.OutputWindow = g_plat.hwnd;

    if (FAILED(g_plat.queue->PresentX(1u, &plane, &pres))) {
        return 0;
    }

    WaitForGpuIdle();

    g_plat.stats.frames_presented += 1ull;

    // 6. In verification mode, collect GPU readback hash & pixel stats from PresentX capture.
    if (g_plat.cfg.verify_ticks > 0 || g_plat.cfg.headless) {
        uint64_t hash = 0;
        uint32_t out_w = 0;
        uint32_t out_h = 0;
        if (SUCCEEDED(OpenScarlettD3D12GetLastPresentHash(g_plat.queue.Get(), &hash, &out_w, &out_h)) &&
            hash != 0ull) {
            g_plat.stats.last_frame_hash = hash;
            g_plat.unique_hashes.insert(hash);
            g_plat.stats.unique_frame_hashes = g_plat.unique_hashes.size();

            const uint32_t need = out_w * out_h * 4u;
            g_plat.last_readback_rgba.resize(need);
            uint32_t copied = 0;
            if (SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
                    g_plat.queue.Get(), g_plat.last_readback_rgba.data(), need, &copied)) &&
                copied == need) {
                g_plat.last_readback_w = out_w;
                g_plat.last_readback_h = out_h;
                uint32_t non_black = 0;
                std::set<uint32_t> uniq_rgb;
                const uint32_t n_px = out_w * out_h;
                for (uint32_t i = 0; i < n_px; ++i) {
                    const uint8_t r = g_plat.last_readback_rgba[i * 4u + 0u];
                    const uint8_t g = g_plat.last_readback_rgba[i * 4u + 1u];
                    const uint8_t b = g_plat.last_readback_rgba[i * 4u + 2u];
                    if (r > 4u || g > 4u || b > 4u) ++non_black;
                    if (uniq_rgb.size() < 4096u) {
                        uniq_rgb.insert((static_cast<uint32_t>(r) << 16) |
                                        (static_cast<uint32_t>(g) << 8) | b);
                    }
                }
                g_plat.stats.last_non_black_pixels = non_black;
                g_plat.stats.last_unique_colors = static_cast<uint32_t>(uniq_rgb.size());
            }
        }
    }

    return 1;
}

void ScarlettPlatformPlaySfx(float freq_hz, uint32_t duration_ms, float gain) {
    if (!g_plat.sfx_voice || duration_ms == 0u) return;
    OS_XAUDIO2_VOICE_STATE st{};
    g_plat.sfx_voice->GetState(&st, 0u);
    if (st.BuffersQueued >= kAudioRingSlots - 1u) {
        return;  // Avoid overflowing queued ring slots
    }

    const uint32_t clamped_ms = std::min<uint32_t>(duration_ms, 250u);
    const size_t sample_count = (static_cast<size_t>(kAudioSampleRate) * clamped_ms) / 1000u;
    auto& buf = g_plat.audio_ring[g_plat.audio_ring_next];
    g_plat.audio_ring_next = (g_plat.audio_ring_next + 1u) % kAudioRingSlots;
    buf.resize(sample_count);

    const float clamped_gain = std::clamp(gain, 0.0f, 0.5f);
    for (size_t i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kAudioSampleRate);
        const float env = 1.0f - (static_cast<float>(i) / static_cast<float>(sample_count));
        const float sample = std::sin(2.0f * kPi * freq_hz * t) * env * clamped_gain;
        buf[i] = static_cast<int16_t>(std::clamp(sample * 32767.0f, -32767.0f, 32767.0f));
    }

    OS_XAUDIO2_BUFFER xbuf{};
    xbuf.AudioBytes = static_cast<uint32_t>(buf.size() * sizeof(int16_t));
    xbuf.pAudioData = reinterpret_cast<const uint8_t*>(buf.data());
    if (g_plat.sfx_voice->SubmitSourceBuffer(&xbuf, nullptr) == OS_XAUDIO2_S_OK) {
        g_plat.stats.audio_buffers_submitted += 1ull;
    }
}

int ScarlettPlatformSaveAndVerify(const char* blob_name, const void* data, size_t byte_count) {
    if (!g_plat.save_provider || !blob_name || !data || byte_count == 0u) return 0;

    XGameSaveContainerHandle cont = nullptr;
    if (XGameSaveCreateContainer(g_plat.save_provider, g_plat.save_container.c_str(), &cont) != XGR_S_OK ||
        !cont) {
        return 0;
    }

    XGameSaveUpdateHandle upd = nullptr;
    if (XGameSaveCreateUpdate(cont, g_plat.save_container.c_str(), &upd) != XGR_S_OK || !upd) {
        XGameSaveCloseContainer(cont);
        return 0;
    }

    const bool write_ok =
        (XGameSaveSubmitBlobWrite(upd, blob_name, static_cast<const uint8_t*>(data), byte_count) == XGR_S_OK) &&
        (XGameSaveSubmitUpdate(upd) == XGR_S_OK);
    XGameSaveCloseUpdate(upd);
    if (!write_ok) {
        XGameSaveCloseContainer(cont);
        return 0;
    }

    // Verify immediate readback from persistent storage (S:\).
    const char* names[1] = {blob_name};
    uint32_t count = 1u;
    std::vector<uint8_t> read_buf(sizeof(XGameSaveBlob) + byte_count + 256u, 0u);
    auto* blobs = reinterpret_cast<XGameSaveBlob*>(read_buf.data());
    const bool read_ok =
        (XGameSaveReadBlobData(cont, names, &count, read_buf.size(), blobs) == XGR_S_OK) &&
        (count == 1u) &&
        (blobs[0].info.size == byte_count) &&
        (std::memcmp(blobs[0].data, data, byte_count) == 0);
    XGameSaveCloseContainer(cont);

    g_plat.stats.save_roundtrip_ok = read_ok ? 1 : 0;
    return read_ok ? 1 : 0;
}

int ScarlettPlatformWriteLastFramePpm(const char* path_utf8) {
    if (!path_utf8 || g_plat.last_readback_rgba.empty() ||
        g_plat.last_readback_w == 0u || g_plat.last_readback_h == 0u) {
        return 0;
    }
    char hdr[64]{};
    const int hdr_len = std::snprintf(
        hdr, sizeof(hdr), "P6\n%u %u\n255\n", g_plat.last_readback_w, g_plat.last_readback_h);
    if (hdr_len <= 0) return 0;

    const size_t n_px = static_cast<size_t>(g_plat.last_readback_w) * g_plat.last_readback_h;
    std::vector<uint8_t> ppm;
    ppm.reserve(static_cast<size_t>(hdr_len) + n_px * 3u);
    ppm.insert(ppm.end(), hdr, hdr + hdr_len);
    for (size_t i = 0; i < n_px; ++i) {
        ppm.push_back(g_plat.last_readback_rgba[i * 4u + 0u]);
        ppm.push_back(g_plat.last_readback_rgba[i * 4u + 1u]);
        ppm.push_back(g_plat.last_readback_rgba[i * 4u + 2u]);
    }
    return WriteWholeFileUtf8(path_utf8, ppm.data(), ppm.size()) ? 1 : 0;
}

int ScarlettPlatformWriteMetricsFile(const char* path_utf8, const char* extra_kv_lines) {
    if (!path_utf8) return 0;
    char buf[2048]{};
    const int n = std::snprintf(
        buf, sizeof(buf),
        "adapter=%s\n"
        "console_type=%u\n"
        "title_id=0x%08X\n"
        "gamertag=%s\n"
        "width=%u\n"
        "height=%u\n"
        "frames_presented=%llu\n"
        "unique_frame_hashes=%llu\n"
        "last_frame_hash=0x%016llX\n"
        "non_black_pixels=%u\n"
        "unique_colors=%u\n"
        "audio_buffers_submitted=%llu\n"
        "input_events_queued=%llu\n"
        "save_roundtrip_ok=%d\n"
        "%s",
        g_plat.stats.adapter_name,
        g_plat.stats.console_type,
        g_plat.stats.title_id,
        g_plat.stats.gamertag,
        g_plat.cfg.fb_width,
        g_plat.cfg.fb_height,
        static_cast<unsigned long long>(g_plat.stats.frames_presented),
        static_cast<unsigned long long>(g_plat.stats.unique_frame_hashes),
        static_cast<unsigned long long>(g_plat.stats.last_frame_hash),
        g_plat.stats.last_non_black_pixels,
        g_plat.stats.last_unique_colors,
        static_cast<unsigned long long>(g_plat.stats.audio_buffers_submitted),
        static_cast<unsigned long long>(g_plat.stats.input_events_queued),
        g_plat.stats.save_roundtrip_ok,
        extra_kv_lines ? extra_kv_lines : "");
    if (n <= 0) return 0;
    return WriteWholeFileUtf8(path_utf8, buf, static_cast<size_t>(n)) ? 1 : 0;
}

void ScarlettPlatformGetStats(ScarlettPlatformStats* out_stats) {
    if (out_stats) {
        *out_stats = g_plat.stats;
    }
}

int ScarlettPlatformResolveGameAsset(const char* rel_name, char* out_path, size_t out_size) {
    if (!rel_name || !out_path || out_size == 0u) return 0;
    out_path[0] = '\0';

    // Probe virtual G:\<rel_name> through hooked CreateFileW so VFS read-only routing & telemetry run!
    const std::string g_virt = std::string("G:\\") + rel_name;
    const std::wstring g_virt_w = Utf8ToWide(g_virt.c_str());
    HANDLE h = ::CreateFileW(g_virt_w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        // Resolve underlying read-only backing path for UCRT fopen() (ucrtbase.dll bypasses EXE IAT).
        char vfs_g[MAX_PATH * 2]{};
        const DWORD len = ::GetEnvironmentVariableA("OPENSCARLETT_VFS_G", vfs_g,
                                                    static_cast<DWORD>(sizeof(vfs_g)));
        if (len > 0u && len < sizeof(vfs_g)) {
            std::string full = std::string(vfs_g);
            if (!full.empty() && full.back() != '\\' && full.back() != '/') full.push_back('\\');
            full += rel_name;
            std::snprintf(out_path, out_size, "%s", full.c_str());
            return 1;
        }
        std::snprintf(out_path, out_size, "%s", g_virt.c_str());
        return 1;
    }

    // Fallback: relative to executable directory (direct standalone launch without openscarlett run).
    char exe_buf[MAX_PATH * 2]{};
    const DWORD elen = ::GetModuleFileNameA(nullptr, exe_buf, static_cast<DWORD>(sizeof(exe_buf)));
    if (elen > 0u && elen < sizeof(exe_buf)) {
        std::string dir(exe_buf);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash + 1u);
        std::string cand = dir + rel_name;
        const std::wstring cand_w = Utf8ToWide(cand.c_str());
        HANDLE hc = ::CreateFileW(cand_w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hc != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hc);
            std::snprintf(out_path, out_size, "%s", cand.c_str());
            return 1;
        }
    }
    return 0;
}

int ScarlettPlatformResolveScratchPath(const char* rel_name, char* out_path, size_t out_size) {
    if (!rel_name || !out_path || out_size == 0u) return 0;
    char vfs_t[MAX_PATH * 2]{};
    const DWORD len = ::GetEnvironmentVariableA("OPENSCARLETT_VFS_T", vfs_t,
                                                static_cast<DWORD>(sizeof(vfs_t)));
    if (len > 0u && len < sizeof(vfs_t)) {
        std::string full(vfs_t);
        if (!full.empty() && full.back() != '\\' && full.back() != '/') full.push_back('\\');
        full += rel_name;
        std::snprintf(out_path, out_size, "%s", full.c_str());
        return 1;
    }
    std::snprintf(out_path, out_size, "T:\\%s", rel_name);
    return 1;
}

int ScarlettPlatformChdirToSaveDir(void) {
    wchar_t dir_buf[MAX_PATH * 2]{};
    DWORD len = ::GetEnvironmentVariableW(L"OPENSCARLETT_VFS_S", dir_buf,
                                          static_cast<DWORD>(sizeof(dir_buf) / sizeof(dir_buf[0])));
    if (len == 0u || len >= sizeof(dir_buf) / sizeof(dir_buf[0])) {
        len = ::GetEnvironmentVariableW(L"OPENSCARLETT_VFS_T", dir_buf,
                                        static_cast<DWORD>(sizeof(dir_buf) / sizeof(dir_buf[0])));
    }
    if (len > 0u && len < sizeof(dir_buf) / sizeof(dir_buf[0])) {
        ::CreateDirectoryW(dir_buf, nullptr);
        return ::SetCurrentDirectoryW(dir_buf) != FALSE ? 1 : 0;
    }
    return 0;
}

}  // extern "C"
