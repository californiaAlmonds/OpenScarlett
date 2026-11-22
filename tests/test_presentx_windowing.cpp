#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/input/gameinput_abi.h"
#include "os_check.h"

namespace {

template <typename T>
class TestComPtr {
public:
    TestComPtr() = default;
    ~TestComPtr() { Reset(); }
    TestComPtr(const TestComPtr&) = delete;
    TestComPtr& operator=(const TestComPtr&) = delete;

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

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        // Defer DestroyWindow to the test teardown so we can also verify WndProc restoration.
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateTestWindow(uint32_t client_w, uint32_t client_h) {
    static const wchar_t* kClassName = L"OpenScarlettPresentXWindowTestClass";
    static bool s_registered = false;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &TestWindowProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kClassName;
        ATOM atom = RegisterClassExW(&wc);
        OS_REQUIRE(atom != 0);
        s_registered = true;
    }

    RECT wr{0, 0, static_cast<LONG>(client_w), static_cast<LONG>(client_h)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD ex_style = 0;
    OS_REQUIRE(AdjustWindowRectEx(&wr, style, FALSE, ex_style) != FALSE);

    // Keep hidden (no WS_VISIBLE) so tests are non-intrusive on desktop and work cleanly on CI.
    HWND hwnd = CreateWindowExW(
        ex_style,
        kClassName,
        L"OpenScarlett PresentX Windowing Test",
        style,
        100,
        100,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);
    OS_REQUIRE(hwnd != nullptr);
    return hwnd;
}

void ResizeTestWindowClient(HWND hwnd, uint32_t client_w, uint32_t client_h) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT wr{0, 0, static_cast<LONG>(client_w), static_cast<LONG>(client_h)};
    OS_REQUIRE(AdjustWindowRectEx(&wr, style, FALSE, ex_style) != FALSE);
    OS_REQUIRE(SetWindowPos(
                   hwnd,
                   nullptr,
                   0,
                   0,
                   wr.right - wr.left,
                   wr.bottom - wr.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
}

bool RectsEqual(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool HasHardwareD3D12Gpu(openscarlett::D3D12AdapterInfo& out_info) {
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    TestComPtr<ID3D12Device> dev;
    TestComPtr<IDXGIAdapter1> adapter;
    openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
        opts, dev.Put(), adapter.Put(), &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

void RenderSolidFrameAndPresent(
    ID3D12CommandQueueX* queue,
    ID3D12CommandAllocator* alloc,
    ID3D12GraphicsCommandListX* cmd,
    ID3D12Resource* rt,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    uint8_t a,
    uint32_t present_flags,
    D3D12XBOX_FRAME_PIPELINE_TOKEN token) {
    OS_REQUIRE(SUCCEEDED(alloc->Reset()));
    OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc, nullptr)));

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = rt;
    to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &to_rt);

    const float clear_color[4] = {
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        static_cast<float>(a) / 255.0f,
    };
    cmd->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);

    D3D12_RESOURCE_BARRIER to_present{};
    to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_present.Transition.pResource = rt;
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &to_present);

    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Resource* plane_res[] = {rt};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = token;
    plane.ResourceCount = 1u;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres{};
    pres.Flags = present_flags;
    pres.SyncInterval = 1u;

    OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));
}

void RunWindowingAndInputSuite(bool use_warp) {
    const char* label = use_warp ? "D3D12 WARP" : "Hardware GPU";
    std::printf("\n=== PresentX Windowing + Interactive GameInput Suite (%s) ===\n", label);

    constexpr uint32_t kInitialW = 256u;
    constexpr uint32_t kInitialH = 256u;
    HWND hwnd = CreateTestWindow(kInitialW, kInitialH);
    const LONG_PTR orig_wndproc = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    OS_REQUIRE(orig_wndproc == reinterpret_cast<LONG_PTR>(&TestWindowProc));

    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);
    OpenScarlettD3D12ResetTelemetry();

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1u;

    TestComPtr<ID3D12DeviceX> dev;
    OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(dev.Put()))));
    OS_REQUIRE(dev.Get() != nullptr);

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    OS_REQUIRE(SUCCEEDED(dev->GetGpuHardwareInfoX(&gpu_info)));
    OS_REQUIRE((gpu_info.IsWarp != 0u) == use_warp);
    std::printf("  Adapter : %s (Warp=%u)\n", gpu_info.AdapterName, gpu_info.IsWarp);

    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    TestComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()))));
    OS_REQUIRE(queue.Get() != nullptr);

    // Verify both additive COM interfaces are reachable without disturbing primary vtable.
    {
        TestComPtr<IOpenScarlettPresentCapture> cap;
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentCapture, reinterpret_cast<void**>(cap.Put()))));
        OS_REQUIRE(SUCCEEDED(cap->SetOutputWindow(hwnd)));
        TestComPtr<IOpenScarlettPresentWindow> pwin;
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentWindow, reinterpret_cast<void**>(pwin.Put()))));
    }

    TestComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));

    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kInitialW;
    rt_desc.Height = kInitialH;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    TestComPtr<ID3D12Resource> rt;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(rt.Put()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1u;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    TestComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()))));

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_handle);

    // -------------------------------------------------------------------------
    // 1. Multi-frame animated Win32 swapchain rendering (6 distinct frames)
    // -------------------------------------------------------------------------
    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 0u, 2u, 0u)));

    std::set<uint64_t> unique_hashes;
    std::vector<uint8_t> pixels(static_cast<size_t>(kInitialW) * kInitialH * 4u, 0u);
    for (uint32_t f = 0; f < 6u; ++f) {
        D3D12XBOX_FRAME_PIPELINE_TOKEN tok = 0;
        OS_REQUIRE(SUCCEEDED(dev->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &tok)));
        const uint8_t cr = static_cast<uint8_t>(0x20u + f * 32u);
        const uint8_t cg = static_cast<uint8_t>(0xE0u - f * 24u);
        const uint8_t cb = static_cast<uint8_t>(0x30u + f * 36u);
        RenderSolidFrameAndPresent(
            queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
            cr, cg, cb, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, tok);

        uint64_t hash = 0;
        uint32_t out_w = 0;
        uint32_t out_h = 0;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetLastPresentHash(queue.Get(), &hash, &out_w, &out_h)));
        OS_REQUIRE(out_w == kInitialW && out_h == kInitialH && hash != 0ull);
        unique_hashes.insert(hash);

        uint32_t copied = 0;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
            queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied)));
        OS_REQUIRE(copied == pixels.size());
        OS_REQUIRE(pixels[0] == cr && pixels[1] == cg && pixels[2] == cb && pixels[3] == 0xFFu);

        std::printf("  [Frame %u] RGBA=(0x%02X,0x%02X,0x%02X,0xFF) | FNV1a=0x%016llX\n",
                    f, cr, cg, cb, static_cast<unsigned long long>(hash));
    }
    OS_REQUIRE(unique_hashes.size() == 6u);

    D3D12XBOX_PRESENT_WINDOW_STATE state{};
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.present_count == 6ull);
    OS_REQUIRE(state.width == kInitialW && state.height == kInitialH);
    OS_REQUIRE(state.is_fullscreen == 0u && state.is_minimized == 0u && state.close_requested == 0u);

    // -------------------------------------------------------------------------
    // 2. Waitable frame latency object verification
    // -------------------------------------------------------------------------
    HANDLE waitable = OpenScarlettD3D12GetFrameLatencyWaitableObject(queue.Get());
    const bool has_waitable_flag =
        (state.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0u;
    if (has_waitable_flag) {
        OS_REQUIRE(waitable != nullptr);
        OS_REQUIRE(state.latency_waits >= 1ull);
    }
    std::printf(
        "  [Waitable] SwapChainFlags=0x%08X | WaitableHandle=%s | LatencyWaits=%llu\n",
        state.swapchain_flags,
        waitable ? "VALID" : "FALLBACK_NONE",
        static_cast<unsigned long long>(state.latency_waits));

    // -------------------------------------------------------------------------
    // 3. SetFrameIntervalX 120 Hz (8333 us) and 60 Hz (16667 us) frame pacing
    // -------------------------------------------------------------------------
    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 8333u, 2u, 0u)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.frame_interval_us == 8333u && state.max_frame_latency == 2u);

    const auto t120_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 4u; ++i) {
        RenderSolidFrameAndPresent(
            queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
            static_cast<uint8_t>(0x10u + i * 40u), 0xA0u, 0x50u, 0xFFu,
            D3D12XBOX_PRESENT_FLAG_NONE, 100u + i);
    }
    const double elapsed_120_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t120_start)
                                      .count();
    OS_REQUIRE(elapsed_120_ms >= 20.0 && elapsed_120_ms < 5000.0);

    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 16667u, 2u, 0u)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.frame_interval_us == 16667u);

    const auto t60_start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 3u; ++i) {
        RenderSolidFrameAndPresent(
            queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
            0x40u, static_cast<uint8_t>(0x30u + i * 50u), 0xC0u, 0xFFu,
            D3D12XBOX_PRESENT_FLAG_NONE, 200u + i);
    }
    const double elapsed_60_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t60_start)
                                     .count();
    OS_REQUIRE(elapsed_60_ms >= 30.0 && elapsed_60_ms < 5000.0);

    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.last_frame_ms > 0.0);

    std::printf(
        "  [Pacing] 120Hz (4 frames)=%.2f ms | 60Hz (3 frames)=%.2f ms | LastFrame=%.2f ms\n",
        elapsed_120_ms, elapsed_60_ms, state.last_frame_ms);

    // Switch to uncapped for remaining window-transition assertions.
    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 0u, 2u, 0u)));

    // -------------------------------------------------------------------------
    // 4. Borderless fullscreen toggle (API + synthetic Alt+Enter WM_SYSKEYDOWN)
    // -------------------------------------------------------------------------
    RECT orig_rect{};
    OS_REQUIRE(GetWindowRect(hwnd, &orig_rect) != FALSE);
    const LONG_PTR orig_style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    // 4a. Direct API toggle ON -> render -> toggle OFF
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12ToggleBorderlessFullscreen(queue.Get())));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_fullscreen == 1u);
    const LONG_PTR fs_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    OS_REQUIRE((fs_style & WS_POPUP) != 0);
    OS_REQUIRE((fs_style & WS_CAPTION) == 0);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x11u, 0xCCu, 0x88u, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 300u);

    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12ToggleBorderlessFullscreen(queue.Get())));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_fullscreen == 0u);
    RECT restored_rect{};
    OS_REQUIRE(GetWindowRect(hwnd, &restored_rect) != FALSE);
    OS_REQUIRE(RectsEqual(orig_rect, restored_rect));
    OS_REQUIRE(GetWindowLongPtrW(hwnd, GWL_STYLE) == orig_style);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x22u, 0xBBu, 0x77u, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 301u);

    // 4b. Synthetic Alt+Enter (WM_SYSKEYDOWN VK_RETURN with Alt bit 29 set) ON -> render -> OFF
    const LPARAM alt_enter_lparam = static_cast<LPARAM>(1) << 29;
    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, alt_enter_lparam);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_fullscreen == 1u);
    OS_REQUIRE((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP) != 0);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x33u, 0xAAu, 0x66u, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 302u);

    SendMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, alt_enter_lparam);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_fullscreen == 0u);
    OS_REQUIRE(GetWindowRect(hwnd, &restored_rect) != FALSE);
    OS_REQUIRE(RectsEqual(orig_rect, restored_rect));
    std::printf("  [Fullscreen] API + Alt+Enter borderless toggle & rect restore OK\n");

    // -------------------------------------------------------------------------
    // 5. Window resize (256x256 -> 320x240 -> 128x192), minimize, and Alt-Tab
    // -------------------------------------------------------------------------
    ResizeTestWindowClient(hwnd, 320u, 240u);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.width == 320u && state.height == 240u);
    // Present 256x256 RT onto 320x240 backbuffer (tests CopyTextureRegion clamping!)
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x55u, 0x77u, 0xDDu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 400u);

    ResizeTestWindowClient(hwnd, 128u, 192u);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.width == 128u && state.height == 192u);
    // Present 256x256 RT onto smaller 128x192 backbuffer (clamped in both X and Y!)
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x66u, 0x88u, 0xCCu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 401u);

    ResizeTestWindowClient(hwnd, 320u, 240u);

    // Simulated minimize via WM_SIZE SIZE_MINIMIZED
    SendMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, MAKELPARAM(0, 0));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_minimized == 1u);
    const uint64_t count_before_min = state.present_count;
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0x99u, 0x44u, 0xEEu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 402u);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.present_count == count_before_min + 1ull);

    // Restore from minimize
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(320, 240));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_minimized == 0u);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0xAAu, 0x55u, 0xDDu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 403u);

    // Alt-Tab focus transitions (WM_ACTIVATEAPP FALSE -> TRUE)
    SendMessageW(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_active == 0u);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0xBBu, 0x66u, 0xCCu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 404u);

    SendMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.is_active == 1u);
    RenderSolidFrameAndPresent(
        queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
        0xCCu, 0x77u, 0xBBu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 405u);

    // Verify pure interactive swapchain present with readback disabled, then re-enable.
    {
        TestComPtr<IOpenScarlettPresentCapture> cap;
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentCapture, reinterpret_cast<void**>(cap.Put()))));
        OS_REQUIRE(SUCCEEDED(cap->SetReadbackCaptureEnabled(FALSE)));
        RenderSolidFrameAndPresent(
            queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
            0x12u, 0x34u, 0x56u, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 406u);
        OS_REQUIRE(SUCCEEDED(cap->SetReadbackCaptureEnabled(TRUE)));
        OS_REQUIRE(SUCCEEDED(queue->PresentX(0u, nullptr, nullptr)));
    }
    std::printf("  [WindowEvents] Resize (320x240, 128x192), Minimize/Restore, Alt-Tab, No-Readback OK\n");

    // -------------------------------------------------------------------------
    // 6. Live multi-frame GameInput loop synchronized with PresentX
    // -------------------------------------------------------------------------
    {
        IGameInput* gi = nullptr;
        OS_REQUIRE(GameInputCreate(&gi) == GAMEINPUT_S_OK && gi != nullptr);
        OS_REQUIRE(OpenScarlettGameInputSetTargetWindow(gi, hwnd) == GAMEINPUT_S_OK);
        gi->SetFocusPolicy(GameInputEnableBackgroundInput);

        struct InputFrameSpec {
            GameInputGamepadState gp;
            std::vector<GameInputKeyState> keys;
        };

        auto make_key = [](uint8_t vk) -> GameInputKeyState {
            GameInputKeyState k{};
            k.virtualKey = vk;
            k.scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            k.codePoint = (vk >= 0x20 && vk <= 0x7E) ? vk : 0u;
            k.isDeadKey = false;
            return k;
        };

        const std::vector<InputFrameSpec> frames = {
            {GameInputGamepadState{GameInputGamepadA, 0.0f, 0.25f, 0.75f, 0.25f, 0.0f, 0.0f},
             {make_key('W')}},
            {GameInputGamepadState{GameInputGamepadB | GameInputGamepadX, 0.5f, 0.9f, -0.5f, 0.0f, 0.25f, -0.25f},
             {make_key('A'), make_key(VK_SPACE)}},
            {GameInputGamepadState{GameInputGamepadY | GameInputGamepadMenu, 1.0f, 0.0f, 0.0f, 1.0f, -0.5f, 1.0f},
             {make_key(VK_F1), make_key('0')}},
            {GameInputGamepadState{GameInputGamepadNone, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
             {}},
        };

        std::vector<IGameInputReading*> readings;
        const GameInputKind combined_kind =
            static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard);

        for (size_t i = 0; i < frames.size(); ++i) {
            const auto& spec = frames[i];
            OS_REQUIRE(OpenScarlettGameInputInjectTestState(
                           gi,
                           &spec.gp,
                           spec.keys.empty() ? nullptr : spec.keys.data(),
                           static_cast<uint32_t>(spec.keys.size()),
                           1) == GAMEINPUT_S_OK);

            IGameInputReading* r = nullptr;
            OS_REQUIRE(gi->GetCurrentReading(combined_kind, nullptr, &r) == GAMEINPUT_S_OK && r != nullptr);

            GameInputGamepadState got_gp{};
            OS_REQUIRE(r->GetGamepadState(&got_gp));
            OS_REQUIRE(got_gp.buttons == spec.gp.buttons);
            OS_REQUIRE(std::fabs(got_gp.leftTrigger - spec.gp.leftTrigger) < 1e-4f);
            OS_REQUIRE(std::fabs(got_gp.rightTrigger - spec.gp.rightTrigger) < 1e-4f);
            OS_REQUIRE(std::fabs(got_gp.leftThumbstickX - spec.gp.leftThumbstickX) < 1e-4f);
            OS_REQUIRE(std::fabs(got_gp.leftThumbstickY - spec.gp.leftThumbstickY) < 1e-4f);
            OS_REQUIRE(std::fabs(got_gp.rightThumbstickX - spec.gp.rightThumbstickX) < 1e-4f);
            OS_REQUIRE(std::fabs(got_gp.rightThumbstickY - spec.gp.rightThumbstickY) < 1e-4f);

            const uint32_t kc = r->GetKeyCount();
            OS_REQUIRE(kc == static_cast<uint32_t>(spec.keys.size()));
            if (kc > 0u) {
                std::vector<GameInputKeyState> got_keys(kc);
                OS_REQUIRE(r->GetKeyState(kc, got_keys.data()) == kc);
                for (uint32_t k = 0; k < kc; ++k) {
                    OS_REQUIRE(got_keys[k].virtualKey == spec.keys[k].virtualKey);
                }
            }

            if (!readings.empty()) {
                OS_REQUIRE(r->GetSequenceNumber(combined_kind) >
                           readings.back()->GetSequenceNumber(combined_kind));
                OS_REQUIRE(r->GetTimestamp() >= readings.back()->GetTimestamp());
            }

            // Drive rendered frame color from live gamepad state.
            const uint8_t red = static_cast<uint8_t>((got_gp.buttons & GameInputGamepadA) ? 0xEEu : 0x30u);
            const uint8_t grn = static_cast<uint8_t>((got_gp.buttons & GameInputGamepadX) ? 0xDDu : 0x50u);
            const uint8_t blu = static_cast<uint8_t>(std::clamp(got_gp.rightTrigger, 0.0f, 1.0f) * 200.0f + 40.0f);
            RenderSolidFrameAndPresent(
                queue.Get(), alloc.Get(), cmd.Get(), rt.Get(), rtv_handle,
                red, grn, blu, 0xFFu, D3D12XBOX_PRESENT_FLAG_IMMEDIATE, 500u + i);

            readings.push_back(r);
        }

        // Verify GetPreviousReading and GetNextReading history traversal across frames.
        for (size_t i = 1; i < readings.size(); ++i) {
            IGameInputReading* prev = nullptr;
            OS_REQUIRE(gi->GetPreviousReading(readings[i], combined_kind, nullptr, &prev) == GAMEINPUT_S_OK);
            OS_REQUIRE(prev != nullptr);
            OS_REQUIRE(prev->GetSequenceNumber(combined_kind) ==
                       readings[i - 1]->GetSequenceNumber(combined_kind));
            prev->Release();

            IGameInputReading* next = nullptr;
            OS_REQUIRE(gi->GetNextReading(readings[i - 1], combined_kind, nullptr, &next) == GAMEINPUT_S_OK);
            OS_REQUIRE(next != nullptr);
            OS_REQUIRE(next->GetSequenceNumber(combined_kind) ==
                       readings[i]->GetSequenceNumber(combined_kind));
            next->Release();
        }
        for (auto* r : readings) {
            r->Release();
        }
        readings.clear();

        // Focus policy gating: hidden non-foreground hwnd suppresses input when background input disabled.
        const GameInputGamepadState active_gp{GameInputGamepadA | GameInputGamepadY, 0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f};
        const GameInputKeyState active_key = make_key('W');
        OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, &active_gp, &active_key, 1u, 1) == GAMEINPUT_S_OK);

        gi->SetFocusPolicy(GameInputDisableBackgroundInput);
        {
            IGameInputReading* gated = nullptr;
            OS_REQUIRE(gi->GetCurrentReading(combined_kind, nullptr, &gated) == GAMEINPUT_S_OK && gated != nullptr);
            GameInputGamepadState g_gp{};
            OS_REQUIRE(gated->GetGamepadState(&g_gp));
            OS_REQUIRE(g_gp.buttons == GameInputGamepadNone);
            OS_REQUIRE(gated->GetKeyCount() == 0u);
            gated->Release();
        }

        gi->SetFocusPolicy(GameInputExclusiveForegroundInput);
        {
            IGameInputReading* gated = nullptr;
            OS_REQUIRE(gi->GetCurrentReading(combined_kind, nullptr, &gated) == GAMEINPUT_S_OK && gated != nullptr);
            GameInputGamepadState g_gp{};
            OS_REQUIRE(gated->GetGamepadState(&g_gp));
            OS_REQUIRE(g_gp.buttons == GameInputGamepadNone);
            OS_REQUIRE(gated->GetKeyCount() == 0u);
            gated->Release();
        }

        gi->SetFocusPolicy(GameInputEnableBackgroundInput);
        {
            IGameInputReading* ungated = nullptr;
            OS_REQUIRE(gi->GetCurrentReading(combined_kind, nullptr, &ungated) == GAMEINPUT_S_OK && ungated != nullptr);
            GameInputGamepadState g_gp{};
            OS_REQUIRE(ungated->GetGamepadState(&g_gp));
            OS_REQUIRE(g_gp.buttons == active_gp.buttons);
            OS_REQUIRE(ungated->GetKeyCount() == 1u);
            ungated->Release();
        }

        // Disable injection and confirm live hardware/keyboard polling path succeeds.
        OS_REQUIRE(OpenScarlettGameInputInjectTestState(gi, nullptr, nullptr, 0u, 0) == GAMEINPUT_S_OK);
        {
            IGameInputReading* live_r = nullptr;
            OS_REQUIRE(gi->GetCurrentReading(combined_kind, nullptr, &live_r) == GAMEINPUT_S_OK && live_r != nullptr);
            live_r->Release();
        }

        gi->Release();
        std::printf("  [GameInput] 4-frame synchronized loop, history nav, and focus policy gating OK\n");
    }

    // -------------------------------------------------------------------------
    // 7. WM_CLOSE clean exit notification, PostQuitMessage, and WndProc restore
    // -------------------------------------------------------------------------
    OS_REQUIRE(GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != orig_wndproc);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12SetWindowCloseBehavior(queue.Get(), 1)));

    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetPresentWindowState(queue.Get(), &state)));
    OS_REQUIRE(state.close_requested == 1u);

    MSG quit_msg{};
    OS_REQUIRE(PeekMessageW(&quit_msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE);
    OS_REQUIRE(quit_msg.message == WM_QUIT);

    // Tear down D3D12X objects and confirm original WndProc is restored cleanly.
    rtv_heap.Reset();
    rt.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    dev.Reset();

    OS_REQUIRE(GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == orig_wndproc);
    OS_REQUIRE(DestroyWindow(hwnd) != FALSE);
    OpenScarlettD3D12SetPreferWarp(0);

    std::printf("  [Teardown] WM_CLOSE notification + WM_QUIT + WndProc restoration OK\n");
}

}  // namespace

int main() {
    openscarlett::D3D12AdapterInfo hw_info{};
    const bool has_hw = HasHardwareD3D12Gpu(hw_info);
    if (has_hw) {
        std::printf("Detected hardware D3D12 GPU: %s\n", hw_info.description.c_str());
        RunWindowingAndInputSuite(false);
    } else {
        std::printf("No discrete hardware D3D12 GPU detected (WARP-only runner); running WARP suite.\n");
    }

    RunWindowingAndInputSuite(true);

    std::printf("\n[PASS] test_presentx_windowing (%s + D3D12 WARP verified)\n",
                has_hw ? hw_info.description.c_str() : "WARP-only");
    return 0;
}
