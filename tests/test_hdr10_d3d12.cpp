#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/shims/secondary_shims_abi.h"
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

LRESULT CALLBACK HdrTestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateHiddenTestWindow(uint32_t client_w, uint32_t client_h) {
    static const wchar_t* kClassName = L"OpenScarlettHdr10D3D12TestClass";
    static bool s_registered = false;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &HdrTestWndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kClassName;
        OS_REQUIRE(RegisterClassExW(&wc) != 0);
        s_registered = true;
    }
    RECT wr{0, 0, static_cast<LONG>(client_w), static_cast<LONG>(client_h)};
    OS_REQUIRE(AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0) != FALSE);
    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"OpenScarlett HDR10 D3D12 Test",
        WS_OVERLAPPEDWINDOW,
        80,
        80,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);
    OS_REQUIRE(hwnd != nullptr);
    return hwnd;
}

bool HasHardwareD3D12Gpu(openscarlett::D3D12AdapterInfo& out_info) {
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    TestComPtr<ID3D12Device> dev;
    TestComPtr<IDXGIAdapter1> adapter;
    const openscarlett::Status st =
        openscarlett::SelectD3D12AdapterAndCreateDevice(opts, dev.Put(), adapter.Put(), &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

float HalfToFloatTest(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x03FFu;
    uint32_t f_bits = 0u;
    if (exp == 0u) {
        if (mant == 0u) {
            f_bits = sign;
        } else {
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x0400u) == 0u) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFu;
            f_bits = sign | (static_cast<uint32_t>(e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        f_bits = sign | 0x7F800000u | (mant << 13);
    } else {
        f_bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &f_bits, sizeof(out));
    return out;
}

void WaitForQueueIdle(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    TestComPtr<ID3D12Fence> fence;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()))));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);
    OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), 1ull)));
    if (fence->GetCompletedValue() < 1ull) {
        OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1ull, ev)));
        OS_REQUIRE(WaitForSingleObject(ev, 15000u) == WAIT_OBJECT_0);
    }
    CloseHandle(ev);
}

void CompileComputeShader(const char* source, const char* entry, TestComPtr<ID3DBlob>& out_blob) {
    TestComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        out_blob.Put(),
        err.Put());
    if (FAILED(hr) && err) {
        std::fprintf(stderr, "D3DCompile error:\n%.*s\n",
                     static_cast<int>(err->GetBufferSize()),
                     static_cast<const char*>(err->GetBufferPointer()));
    }
    OS_REQUIRE(SUCCEEDED(hr) && out_blob.Get() != nullptr);
}

// ---------------------------------------------------------------------------
// 1. PQ ST.2084 Math Round-Trip & ACES/Reinhard Tone-Map Determinism
// ---------------------------------------------------------------------------
void TestPqAndToneMapMath() {
    std::printf("[Test 1] PQ ST.2084 OETF/EOTF round-trip & ACES/Reinhard tone-map math...\n");

    const float test_nits[] = {0.0f, 0.005f, 0.1f, 1.0f, 10.0f, 80.0f, 200.0f, 500.0f, 1000.0f, 4000.0f, 10000.0f};
    float prev_pq = -1.0f;
    for (float nits : test_nits) {
        const float pq = openscarlett::d3d12_xs::LinearNitsToPqSt2084(nits);
        OS_REQUIRE(std::isfinite(pq) && pq >= 0.0f && pq <= 1.0f);
        OS_REQUIRE(pq > prev_pq || (nits == 0.0f && prev_pq < 0.0f));
        prev_pq = pq;

        const float decoded = openscarlett::d3d12_xs::PqSt2084ToLinearNits(pq);
        OS_REQUIRE(std::isfinite(decoded));
        const float tol = std::max(1e-3f, nits * 1e-4f);
        OS_REQUIRE(std::fabs(decoded - nits) <= tol);
    }

    // Verify NaN, -Inf, +Inf, and >1.0 HDR values map safely without NaN or premature clipping.
    const float hdr_levels[] = {0.0f, 0.25f, 0.5f, 1.0f, 2.5f, 5.0f, 12.5f, 50.0f};
    uint8_t prev_aces = 0u;
    uint8_t prev_rein = 0u;
    for (size_t i = 0; i < sizeof(hdr_levels) / sizeof(hdr_levels[0]); ++i) {
        const float L = hdr_levels[i];
        uint8_t aces[4]{};
        uint8_t rein[4]{};
        OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(L, L, L, 1.0f, D3D12XBOX_TONEMAP_ACES, aces);
        OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(L, L, L, 1.0f, D3D12XBOX_TONEMAP_REINHARD, rein);
        OS_REQUIRE(aces[3] == 255u && rein[3] == 255u);
        if (i > 0) {
            OS_REQUIRE(aces[0] >= prev_aces);
            OS_REQUIRE(rein[0] >= prev_rein);
        }
        prev_aces = aces[0];
        prev_rein = rein[0];
    }
    OS_REQUIRE(prev_aces >= 240u && prev_rein >= 230u);

    uint8_t nan_out[4]{};
    OpenScarlettD3D12ToneMapLinearRgbToSdrRgba8(
        std::numeric_limits<float>::quiet_NaN(),
        -5.0f,
        std::numeric_limits<float>::infinity(),
        1.0f,
        D3D12XBOX_TONEMAP_ACES,
        nan_out);
    OS_REQUIRE(nan_out[0] == 0u && nan_out[1] == 0u && nan_out[2] == 0u && nan_out[3] == 255u);

    std::printf("  PQ ST.2084 round-trip error <1e-4 across 0..10000 nits; ACES/Reinhard monotonic OK\n");
}

// ---------------------------------------------------------------------------
// 2-5. Live GPU Suite (runs on BOTH Hardware GPU and D3D12 WARP)
// ---------------------------------------------------------------------------
void RunLiveGpuSuite(bool use_warp) {
    const char* label = use_warp ? "D3D12 WARP" : "Hardware GPU";
    std::printf("\n=== HDR10 / Default Array SRV / Unified Memory Suite (%s) ===\n", label);

    constexpr uint32_t kWinW = 128u;
    constexpr uint32_t kWinH = 64u;
    HWND hwnd = CreateHiddenTestWindow(kWinW, kWinH);

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
    std::printf("  Adapter: %s (Warp=%u)\n", gpu_info.AdapterName, gpu_info.IsWarp);

    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    TestComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 0u, 2u, 0u)));

    TestComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));

    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    // -----------------------------------------------------------------------
    // 2. HDR10 (R10G10B10A2_UNORM + G2084_NONE_P2020) Swapchain + SetHDRMetaData
    // -----------------------------------------------------------------------
    {
        D3D12XBOX_HDR10_METADATA meta{};
        meta.MaxMasteringLuminance = 1000u;
        meta.MinMasteringLuminance = 50u;
        meta.MaxContentLightLevel = 1000u;
        meta.MaxFrameAverageLightLevel = 400u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12SetHdrMetaData(queue.Get(), &meta)));

        D3D12XBOX_PRESENTX_CONFIG cfg{};
        cfg.OutputWindow = hwnd;
        cfg.PreferredFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
        cfg.ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        cfg.ToneMapMode = D3D12XBOX_TONEMAP_ACES;
        cfg.PaperWhiteNits = 200.0f;
        cfg.MaxDisplayNits = 1000.0f;
        cfg.ForceSdrToneMapFallback = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12SetPresentConfig(queue.Get(), &cfg)));

        D3D12_HEAP_PROPERTIES def_heap{};
        def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = kWinW;
        rt_desc.Height = kWinH;
        rt_desc.DepthOrArraySize = 1u;
        rt_desc.MipLevels = 1u;
        rt_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        rt_desc.SampleDesc.Count = 1u;
        rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        TestComPtr<ID3D12Resource> hdr10_rt;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
            D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(hdr10_rt.Put()))));

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.NumDescriptors = 1u;
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        TestComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.Put()))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(hdr10_rt.Get(), nullptr, rtv);

        const float target_r_nits = 500.0f;
        const float target_g_nits = 200.0f;
        const float target_b_nits = 100.0f;
        const float pq_clear[4] = {
            OpenScarlettD3D12LinearNitsToPqSt2084(target_r_nits),
            OpenScarlettD3D12LinearNitsToPqSt2084(target_g_nits),
            OpenScarlettD3D12LinearNitsToPqSt2084(target_b_nits),
            1.0f,
        };

        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
        D3D12_RESOURCE_BARRIER b1{};
        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b1.Transition.pResource = hdr10_rt.Get();
        b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1u, &b1);
        cmd->ClearRenderTargetView(rtv, pq_clear, 0u, nullptr);
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1u, &b1);
        OS_REQUIRE(SUCCEEDED(cmd->Close()));

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);

        ID3D12Resource* plane_res[] = {hdr10_rt.Get()};
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.ResourceCount = 1u;
        plane.ppResources = plane_res;
        plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

        D3D12XBOX_PRESENT_PARAMETERS pres{};
        pres.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
        pres.SyncInterval = 0u;
        OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));

        D3D12XBOX_HDR_SWAPCHAIN_STATE hdr_state{};
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetHdrSwapChainState(queue.Get(), &hdr_state)));
        OS_REQUIRE(hdr_state.LastPresentedSourceFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
        OS_REQUIRE(hdr_state.LastPresentedColorSpace == D3D12XBOX_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        OS_REQUIRE(hdr_state.ActiveMetaData.MaxMasteringLuminance == 1000u);
        OS_REQUIRE(hdr_state.ActiveMetaData.MaxContentLightLevel == 1000u);
        OS_REQUIRE(hdr_state.ActiveMetaData.MaxFrameAverageLightLevel == 400u);
        if (hdr_state.SwapChainFormat != DXGI_FORMAT_UNKNOWN) {
            OS_REQUIRE(hdr_state.HasSwapChain4 == 1u);
        }

        // Verify raw R10G10B10A2 PQ readback decodes back to target nits!
        std::vector<uint8_t> raw_hdr10(static_cast<size_t>(kWinW) * kWinH * 4u, 0u);
        uint32_t raw_copied = 0u;
        DXGI_FORMAT raw_fmt = DXGI_FORMAT_UNKNOWN;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentRawPixels(
            queue.Get(), raw_hdr10.data(), static_cast<uint32_t>(raw_hdr10.size()), &raw_copied, &raw_fmt)));
        OS_REQUIRE(raw_copied == raw_hdr10.size());
        OS_REQUIRE(raw_fmt == DXGI_FORMAT_R10G10B10A2_UNORM);

        uint32_t packed0 = 0u;
        std::memcpy(&packed0, raw_hdr10.data(), sizeof(packed0));
        const float r_pq_rb = static_cast<float>(packed0 & 0x3FFu) / 1023.0f;
        const float g_pq_rb = static_cast<float>((packed0 >> 10) & 0x3FFu) / 1023.0f;
        const float b_pq_rb = static_cast<float>((packed0 >> 20) & 0x3FFu) / 1023.0f;
        const float r_nits_rb = OpenScarlettD3D12PqSt2084ToLinearNits(r_pq_rb);
        const float g_nits_rb = OpenScarlettD3D12PqSt2084ToLinearNits(g_pq_rb);
        const float b_nits_rb = OpenScarlettD3D12PqSt2084ToLinearNits(b_pq_rb);
        // 10-bit PQ step at 500 nits is ~6 nits; check within 15 nits tolerance.
        OS_REQUIRE(std::fabs(r_nits_rb - target_r_nits) < 15.0f);
        OS_REQUIRE(std::fabs(g_nits_rb - target_g_nits) < 8.0f);
        OS_REQUIRE(std::fabs(b_nits_rb - target_b_nits) < 5.0f);

        // Verify tone-mapped SDR RGBA8 readback preserves ordering & valid alpha.
        std::vector<uint8_t> sdr_rgba(static_cast<size_t>(kWinW) * kWinH * 4u, 0u);
        uint32_t sdr_copied = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
            queue.Get(), sdr_rgba.data(), static_cast<uint32_t>(sdr_rgba.size()), &sdr_copied)));
        OS_REQUIRE(sdr_copied == sdr_rgba.size());
        OS_REQUIRE(sdr_rgba[0] > sdr_rgba[1] && sdr_rgba[1] > sdr_rgba[2] && sdr_rgba[2] > 0u);
        OS_REQUIRE(sdr_rgba[3] == 255u);

        std::printf(
            "  [HDR10] SwapFmt=%u SC4=%u CSApplied=%u MetaApplied=%u TM=%u | DecodedNits=(%.1f,%.1f,%.1f) | SDR=(%u,%u,%u,%u)\n",
            static_cast<unsigned>(hdr_state.SwapChainFormat),
            hdr_state.HasSwapChain4,
            hdr_state.ColorSpaceApplied,
            hdr_state.HdrMetaDataApplied,
            hdr_state.ToneMapFallbackActive,
            r_nits_rb, g_nits_rb, b_nits_rb,
            sdr_rgba[0], sdr_rgba[1], sdr_rgba[2], sdr_rgba[3]);
    }

    // -----------------------------------------------------------------------
    // 3. scRGB (R16G16B16A16_FLOAT + G10_NONE_P709) + ACES & Reinhard SDR Fallback
    // -----------------------------------------------------------------------
    {
        D3D12_HEAP_PROPERTIES def_heap{};
        def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rt_desc{};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = kWinW;
        rt_desc.Height = kWinH;
        rt_desc.DepthOrArraySize = 1u;
        rt_desc.MipLevels = 1u;
        rt_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rt_desc.SampleDesc.Count = 1u;
        rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        TestComPtr<ID3D12Resource> scrgb_rt;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
            D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(scrgb_rt.Put()))));

        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.NumDescriptors = 1u;
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        TestComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.Put()))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(scrgb_rt.Get(), nullptr, rtv);

        // Render 4 horizontal HDR bands with values > 1.0 (0.5, 1.0, 4.0, 12.5).
        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = scrgb_rt.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1u, &b);

        const float bands[4][4] = {
            {0.5f, 0.5f, 0.5f, 1.0f},     // 40 nits
            {1.0f, 1.0f, 1.0f, 1.0f},     // 80 nits
            {4.0f, 2.0f, 0.5f, 1.0f},     // 320 / 160 / 40 nits (>1.0 HDR!)
            {12.5f, 12.5f, 12.5f, 1.0f},  // 1000 nits bright HDR!
        };
        for (uint32_t i = 0; i < 4u; ++i) {
            D3D12_RECT rect{
                static_cast<LONG>(i * 32u), 0,
                static_cast<LONG>((i + 1u) * 32u), static_cast<LONG>(kWinH)};
            cmd->ClearRenderTargetView(rtv, bands[i], 1u, &rect);
        }

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1u, &b);
        OS_REQUIRE(SUCCEEDED(cmd->Close()));

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);

        // 3a. Native scRGB swapchain + ACES tone-mapped readback
        D3D12XBOX_PRESENTX_CONFIG scrgb_cfg{};
        scrgb_cfg.OutputWindow = hwnd;
        scrgb_cfg.PreferredFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        scrgb_cfg.ColorSpace = D3D12XBOX_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        scrgb_cfg.ToneMapMode = D3D12XBOX_TONEMAP_ACES;
        scrgb_cfg.PaperWhiteNits = 200.0f;
        scrgb_cfg.ForceSdrToneMapFallback = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12SetPresentConfig(queue.Get(), &scrgb_cfg)));

        ID3D12Resource* plane_res[] = {scrgb_rt.Get()};
        D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
        plane.ResourceCount = 1u;
        plane.ppResources = plane_res;
        plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;
        D3D12XBOX_PRESENT_PARAMETERS pres{};
        pres.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
        OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));

        // Verify raw FP16 readback preserves exact >1.0 HDR values!
        std::vector<uint8_t> raw_fp16(static_cast<size_t>(kWinW) * kWinH * 8u, 0u);
        uint32_t fp16_copied = 0u;
        DXGI_FORMAT fp16_fmt = DXGI_FORMAT_UNKNOWN;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentRawPixels(
            queue.Get(), raw_fp16.data(), static_cast<uint32_t>(raw_fp16.size()), &fp16_copied, &fp16_fmt)));
        OS_REQUIRE(fp16_copied == raw_fp16.size() && fp16_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

        auto read_fp16_r = [&](uint32_t x) -> float {
            uint16_t h = 0u;
            std::memcpy(&h, raw_fp16.data() + static_cast<size_t>(x) * 8u, sizeof(h));
            return HalfToFloatTest(h);
        };
        OS_REQUIRE(std::fabs(read_fp16_r(16u) - 0.5f) < 0.01f);
        OS_REQUIRE(std::fabs(read_fp16_r(48u) - 1.0f) < 0.01f);
        OS_REQUIRE(std::fabs(read_fp16_r(80u) - 4.0f) < 0.02f);
        OS_REQUIRE(std::fabs(read_fp16_r(112u) - 12.5f) < 0.05f);

        std::vector<uint8_t> aces_rgba(static_cast<size_t>(kWinW) * kWinH * 4u, 0u);
        uint32_t aces_copied = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
            queue.Get(), aces_rgba.data(), static_cast<uint32_t>(aces_rgba.size()), &aces_copied)));
        OS_REQUIRE(aces_copied == aces_rgba.size());
        const uint8_t a0 = aces_rgba[16u * 4u];
        const uint8_t a1 = aces_rgba[48u * 4u];
        const uint8_t a2 = aces_rgba[80u * 4u];
        const uint8_t a3 = aces_rgba[112u * 4u];
        OS_REQUIRE(a0 > 0u && a0 < a1 && a1 < a2 && a2 < a3 && a3 <= 255u);

        // 3b. Forced SDR swapchain fallback with Reinhard tone-mapping
        scrgb_cfg.ToneMapMode = D3D12XBOX_TONEMAP_REINHARD;
        scrgb_cfg.ForceSdrToneMapFallback = 1u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12SetPresentConfig(queue.Get(), &scrgb_cfg)));
        OS_REQUIRE(SUCCEEDED(queue->PresentX(1u, &plane, &pres)));

        D3D12XBOX_HDR_SWAPCHAIN_STATE fb_state{};
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetHdrSwapChainState(queue.Get(), &fb_state)));
        OS_REQUIRE(fb_state.ToneMapFallbackActive == 1u);
        OS_REQUIRE(fb_state.ToneMapMode == D3D12XBOX_TONEMAP_REINHARD);

        std::vector<uint8_t> rein_rgba(static_cast<size_t>(kWinW) * kWinH * 4u, 0u);
        uint32_t rein_copied = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
            queue.Get(), rein_rgba.data(), static_cast<uint32_t>(rein_rgba.size()), &rein_copied)));
        OS_REQUIRE(rein_copied == rein_rgba.size());
        const uint8_t r0 = rein_rgba[16u * 4u];
        const uint8_t r1 = rein_rgba[48u * 4u];
        const uint8_t r2 = rein_rgba[80u * 4u];
        const uint8_t r3 = rein_rgba[112u * 4u];
        OS_REQUIRE(r0 > 0u && r0 < r1 && r1 < r2 && r2 < r3 && r3 <= 255u);
        OS_REQUIRE(r2 != a2);  // Reinhard curve differs distinctly from ACES curve!

        std::printf(
            "  [scRGB] FP16=(%.1f,%.1f,%.1f,%.1f) | ACES=(%u,%u,%u,%u) | ReinhardFallback=(%u,%u,%u,%u)\n",
            read_fp16_r(16u), read_fp16_r(48u), read_fp16_r(80u), read_fp16_r(112u),
            a0, a1, a2, a3, r0, r1, r2, r3);
    }

    // -----------------------------------------------------------------------
    // 4. Default (nullptr desc) SRV on Texture2DArray (ArraySize = 4) — ScarlettX #46
    // -----------------------------------------------------------------------
    {
        static constexpr const char* kArrayCsHlsl = R"(
Texture2DArray<float4> gTexArray : register(t0);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CsMain(uint3 tid : SV_DispatchThreadID) {
    uint slice = tid.x / 8;
    uint sx = tid.x % 8;
    uint sy = tid.y;
    gOut[tid.xy] = gTexArray.Load(int4(sx, sy, slice, 0));
}
)";
        TestComPtr<ID3DBlob> cs_blob;
        CompileComputeShader(kArrayCsHlsl, "CsMain", cs_blob);

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1u;
        ranges[0].BaseShaderRegister = 0u;
        ranges[0].OffsetInDescriptorsFromTableStart = 0u;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1u;
        ranges[1].BaseShaderRegister = 0u;
        ranges[1].OffsetInDescriptorsFromTableStart = 1u;

        D3D12_ROOT_PARAMETER root_param{};
        root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_param.DescriptorTable.NumDescriptorRanges = 2u;
        root_param.DescriptorTable.pDescriptorRanges = ranges;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 1u;
        rs_desc.pParameters = &root_param;

        using PfnSerRS = HRESULT(WINAPI*)(
            const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto ser_rs = reinterpret_cast<PfnSerRS>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        OS_REQUIRE(ser_rs != nullptr);

        TestComPtr<ID3DBlob> rs_blob;
        TestComPtr<ID3DBlob> rs_err;
        OS_REQUIRE(SUCCEEDED(ser_rs(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));

        TestComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
            0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_sig.Get();
        pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
        TestComPtr<ID3D12PipelineState> pso;
        OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()))));

        // Create Texture2DArray with 4 slices (8x8 RGBA8)
        constexpr uint32_t kSliceW = 8u;
        constexpr uint32_t kSliceH = 8u;
        constexpr uint32_t kSlices = 4u;

        D3D12_HEAP_PROPERTIES def_heap{};
        def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC arr_desc{};
        arr_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        arr_desc.Width = kSliceW;
        arr_desc.Height = kSliceH;
        arr_desc.DepthOrArraySize = kSlices;
        arr_desc.MipLevels = 1u;
        arr_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        arr_desc.SampleDesc.Count = 1u;
        arr_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        TestComPtr<ID3D12Resource> tex_array;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_heap, D3D12_HEAP_FLAG_NONE, &arr_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(tex_array.Put()))));

        D3D12_RESOURCE_DESC out_desc{};
        out_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        out_desc.Width = kSliceW * kSlices;
        out_desc.Height = kSliceH;
        out_desc.DepthOrArraySize = 1u;
        out_desc.MipLevels = 1u;
        out_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        out_desc.SampleDesc.Count = 1u;
        out_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        TestComPtr<ID3D12Resource> out_tex;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out_tex.Put()))));

        // Upload 4 distinct colors into slices 0..3
        const uint8_t slice_colors[4][4] = {
            {240u, 20u, 20u, 255u},   // Slice 0: Red
            {20u, 240u, 20u, 255u},   // Slice 1: Green
            {20u, 20u, 240u, 255u},   // Slice 2: Blue
            {240u, 240u, 20u, 255u},  // Slice 3: Yellow
        };

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[kSlices]{};
        UINT num_rows[kSlices]{};
        UINT64 row_sizes[kSlices]{};
        UINT64 total_up_bytes = 0ull;
        dev->GetCopyableFootprints(&arr_desc, 0u, kSlices, 0ull, fps, num_rows, row_sizes, &total_up_bytes);

        D3D12_HEAP_PROPERTIES up_heap{};
        up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = total_up_bytes;
        buf_desc.Height = 1u;
        buf_desc.DepthOrArraySize = 1u;
        buf_desc.MipLevels = 1u;
        buf_desc.SampleDesc.Count = 1u;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        TestComPtr<ID3D12Resource> up_buf;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &up_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up_buf.Put()))));

        void* up_ptr = nullptr;
        D3D12_RANGE rnone{0u, 0u};
        OS_REQUIRE(SUCCEEDED(up_buf->Map(0u, &rnone, &up_ptr)));
        auto* up_bytes = static_cast<uint8_t*>(up_ptr);
        for (uint32_t s = 0u; s < kSlices; ++s) {
            for (uint32_t y = 0u; y < kSliceH; ++y) {
                uint8_t* dst = up_bytes + fps[s].Offset + static_cast<size_t>(y) * fps[s].Footprint.RowPitch;
                for (uint32_t x = 0u; x < kSliceW; ++x) {
                    std::memcpy(dst + x * 4u, slice_colors[s], 4u);
                }
            }
        }
        up_buf->Unmap(0u, nullptr);

        // Descriptor heap: slot 0 = DEFAULT (nullptr desc!) SRV on Texture2DArray, slot 1 = UAV on out_tex
        D3D12_DESCRIPTOR_HEAP_DESC dh_desc{};
        dh_desc.NumDescriptors = 2u;
        dh_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        dh_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        TestComPtr<ID3D12DescriptorHeap> dh;
        OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dh_desc, IID_PPV_ARGS(dh.Put()))));

        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = dh->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{srv_cpu.ptr + inc};

        // Pass nullptr desc — DeviceX MUST preserve all 4 array slices (ScarlettX #46)!
        dev->CreateShaderResourceView(tex_array.Get(), nullptr, srv_cpu);
        dev->CreateUnorderedAccessView(out_tex.Get(), nullptr, nullptr, uav_cpu);

        // Readback buffer for out_tex
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT out_fp{};
        UINT out_rows = 0u;
        UINT64 out_row_bytes = 0ull;
        UINT64 out_total = 0ull;
        dev->GetCopyableFootprints(&out_desc, 0u, 1u, 0ull, &out_fp, &out_rows, &out_row_bytes, &out_total);

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        buf_desc.Width = out_total;
        TestComPtr<ID3D12Resource> rb_buf;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &rb_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb_buf.Put()))));

        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc.Get(), pso.Get())));
        for (uint32_t s = 0u; s < kSlices; ++s) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = tex_array.Get();
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_loc.SubresourceIndex = s;
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = up_buf.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint = fps[s];
            cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
        }

        D3D12_RESOURCE_BARRIER arr_to_srv{};
        arr_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        arr_to_srv.Transition.pResource = tex_array.Get();
        arr_to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        arr_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        arr_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1u, &arr_to_srv);

        ID3D12DescriptorHeap* heaps[] = {dh.Get()};
        cmd->SetDescriptorHeaps(1u, heaps);
        cmd->SetComputeRootSignature(root_sig.Get());
        cmd->SetComputeRootDescriptorTable(0u, dh->GetGPUDescriptorHandleForHeapStart());
        cmd->Dispatch(kSlices, 1u, 1u);

        D3D12_RESOURCE_BARRIER out_to_src{};
        out_to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        out_to_src.Transition.pResource = out_tex.Get();
        out_to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        out_to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        out_to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->ResourceBarrier(1u, &out_to_src);

        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = rb_buf.Get();
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint = out_fp;
        D3D12_TEXTURE_COPY_LOCATION rb_src{};
        rb_src.pResource = out_tex.Get();
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0u;
        cmd->CopyTextureRegion(&rb_dst, 0u, 0u, 0u, &rb_src, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd->Close()));
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1u, lists);
        WaitForQueueIdle(dev.Get(), queue.Get());

        void* rb_ptr = nullptr;
        D3D12_RANGE rb_range{0u, static_cast<SIZE_T>(out_total)};
        OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rb_range, &rb_ptr)));
        const auto* rb_bytes = static_cast<const uint8_t*>(rb_ptr);
        for (uint32_t s = 0u; s < kSlices; ++s) {
            const uint8_t* px = rb_bytes + static_cast<size_t>(4u) * out_fp.Footprint.RowPitch +
                                (s * kSliceW + 4u) * 4u;
            OS_REQUIRE(px[0] == slice_colors[s][0]);
            OS_REQUIRE(px[1] == slice_colors[s][1]);
            OS_REQUIRE(px[2] == slice_colors[s][2]);
            OS_REQUIRE(px[3] == slice_colors[s][3]);
        }
        rb_buf->Unmap(0u, &rnone);

        int32_t srv_slot = -1;
        uint64_t srv_calls = 0ull;
        OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry(
                       "ID3D12DeviceX::CreateShaderResourceView[DefaultArrayPreserved]",
                       &srv_slot, &srv_calls) == 1);
        OS_REQUIRE(srv_slot == 18 && srv_calls >= 1ull);
        std::printf("  [DefaultArraySRV] All 4 slices verified live on GPU (ScarlettX #46 telemetry slot=%d)\n",
                    srv_slot);
    }

    // -----------------------------------------------------------------------
    // 5. Title Unified-Memory Texture Remap Tracking (ivall/xcl fd74662/bdae5a4)
    // -----------------------------------------------------------------------
    {
        static constexpr const char* kSample2dCsHlsl = R"(
Texture2D<float4> gTex : register(t0);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CsMain(uint3 tid : SV_DispatchThreadID) {
    gOut[tid.xy] = gTex.Load(int3(tid.xy, 0));
}
)";
        TestComPtr<ID3DBlob> cs_blob;
        CompileComputeShader(kSample2dCsHlsl, "CsMain", cs_blob);

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1u;
        ranges[0].BaseShaderRegister = 0u;
        ranges[0].OffsetInDescriptorsFromTableStart = 0u;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1u;
        ranges[1].BaseShaderRegister = 0u;
        ranges[1].OffsetInDescriptorsFromTableStart = 1u;

        D3D12_ROOT_PARAMETER root_param{};
        root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_param.DescriptorTable.NumDescriptorRanges = 2u;
        root_param.DescriptorTable.pDescriptorRanges = ranges;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 1u;
        rs_desc.pParameters = &root_param;

        using PfnSerRS = HRESULT(WINAPI*)(
            const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
        auto ser_rs = reinterpret_cast<PfnSerRS>(
            openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        OS_REQUIRE(ser_rs != nullptr);

        TestComPtr<ID3DBlob> rs_blob;
        TestComPtr<ID3DBlob> rs_err;
        OS_REQUIRE(SUCCEEDED(ser_rs(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
        TestComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
            0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_sig.Get();
        pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
        TestComPtr<ID3D12PipelineState> pso;
        OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()))));

        constexpr uint32_t kTexW = 16u;
        constexpr uint32_t kTexH = 16u;
        constexpr size_t kTexBytes = kTexW * kTexH * 4u;

        auto fill_solid = [](std::vector<uint8_t>& buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
            buf.resize(kTexBytes);
            for (size_t i = 0; i < kTexW * kTexH; ++i) {
                buf[i * 4u + 0u] = r;
                buf[i * 4u + 1u] = g;
                buf[i * 4u + 2u] = b;
                buf[i * 4u + 3u] = a;
            }
        };

        std::vector<uint8_t> cpu_buf_a;
        std::vector<uint8_t> cpu_buf_b;
        fill_solid(cpu_buf_a, 0u, 220u, 240u, 255u);    // Cyan
        fill_solid(cpu_buf_b, 240u, 30u, 220u, 255u);   // Magenta

        D3D12_RESOURCE_DESC u_desc{};
        u_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        u_desc.Width = kTexW;
        u_desc.Height = kTexH;
        u_desc.DepthOrArraySize = 1u;
        u_desc.MipLevels = 1u;
        u_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        u_desc.SampleDesc.Count = 1u;
        u_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        // Automatically registers unified texture mapping over cpu_buf_a!
        TestComPtr<ID3D12Resource> placed_tex;
        const uint64_t va_a = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cpu_buf_a.data()));
        const uint64_t va_b = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cpu_buf_b.data()));
        OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
            va_a, &u_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(placed_tex.Put()))));

        D3D12_HEAP_PROPERTIES def_heap{};
        def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC out_desc = u_desc;
        out_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        TestComPtr<ID3D12Resource> out_tex;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &def_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out_tex.Put()))));

        D3D12_DESCRIPTOR_HEAP_DESC dh_desc{};
        dh_desc.NumDescriptors = 2u;
        dh_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        dh_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        TestComPtr<ID3D12DescriptorHeap> dh;
        OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dh_desc, IID_PPV_ARGS(dh.Put()))));
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = dh->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{srv_cpu.ptr + inc};
        dev->CreateShaderResourceView(placed_tex.Get(), nullptr, srv_cpu);
        dev->CreateUnorderedAccessView(out_tex.Get(), nullptr, nullptr, uav_cpu);

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT out_fp{};
        UINT out_rows = 0u;
        UINT64 out_row_bytes = 0ull;
        UINT64 out_total = 0ull;
        dev->GetCopyableFootprints(&out_desc, 0u, 1u, 0ull, &out_fp, &out_rows, &out_row_bytes, &out_total);

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = out_total;
        rb_desc.Height = 1u;
        rb_desc.DepthOrArraySize = 1u;
        rb_desc.MipLevels = 1u;
        rb_desc.SampleDesc.Count = 1u;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        TestComPtr<ID3D12Resource> rb_buf;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb_buf.Put()))));

        auto sample_and_verify = [&](uint8_t exp_r, uint8_t exp_g, uint8_t exp_b, uint8_t exp_a) {
            OS_REQUIRE(SUCCEEDED(alloc->Reset()));
            OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc.Get(), pso.Get())));

            ID3D12DescriptorHeap* heaps[] = {dh.Get()};
            cmd->SetDescriptorHeaps(1u, heaps);
            cmd->SetComputeRootSignature(root_sig.Get());
            cmd->SetComputeRootDescriptorTable(0u, dh->GetGPUDescriptorHandleForHeapStart());
            cmd->Dispatch(kTexW / 8u, kTexH / 8u, 1u);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = out_tex.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd->ResourceBarrier(1u, &b);

            D3D12_TEXTURE_COPY_LOCATION rb_dst{};
            rb_dst.pResource = rb_buf.Get();
            rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rb_dst.PlacedFootprint = out_fp;
            D3D12_TEXTURE_COPY_LOCATION rb_src{};
            rb_src.pResource = out_tex.Get();
            rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rb_src.SubresourceIndex = 0u;
            cmd->CopyTextureRegion(&rb_dst, 0u, 0u, 0u, &rb_src, nullptr);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cmd->ResourceBarrier(1u, &b);

            OS_REQUIRE(SUCCEEDED(cmd->Close()));
            ID3D12CommandList* lists[] = {cmd.Get()};
            queue->ExecuteCommandLists(1u, lists);
            WaitForQueueIdle(dev.Get(), queue.Get());

            void* ptr = nullptr;
            D3D12_RANGE rr{0u, static_cast<SIZE_T>(out_total)};
            OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rr, &ptr)));
            const auto* px = static_cast<const uint8_t*>(ptr) +
                             static_cast<size_t>(8u) * out_fp.Footprint.RowPitch + 8u * 4u;
            OS_REQUIRE(px[0] == exp_r && px[1] == exp_g && px[2] == exp_b && px[3] == exp_a);
            D3D12_RANGE wn{0u, 0u};
            rb_buf->Unmap(0u, &wn);
        };

        // Step 5a: Initial auto-registered upload (explicit flush) -> Cyan
        uint32_t flushed = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12FlushDirtyUnifiedTextures(dev.Get(), queue.Get(), &flushed)));
        OS_REQUIRE(flushed == 1u);
        sample_and_verify(0u, 220u, 240u, 255u);

        // Step 5b: Mutate backing memory in-place -> Orange; NotifyVirtualRemap; auto-flush on ExecuteCommandLists!
        fill_solid(cpu_buf_a, 250u, 140u, 10u, 255u);
        uint32_t flagged = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12NotifyVirtualRemap(
            dev.Get(), va_a, kTexBytes, 0ull, &flagged)));
        OS_REQUIRE(flagged == 1u);
        sample_and_verify(250u, 140u, 10u, 255u);

        // Step 5c: Remap virtual range to new backing buffer cpu_buf_b -> Magenta; explicit flush & verify!
        flagged = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12NotifyVirtualRemap(
            dev.Get(), va_a, kTexBytes, va_b, &flagged)));
        OS_REQUIRE(flagged == 1u);
        flushed = 0u;
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12FlushDirtyUnifiedTextures(dev.Get(), queue.Get(), &flushed)));
        OS_REQUIRE(flushed == 1u);
        sample_and_verify(240u, 30u, 220u, 255u);

        D3D12XBOX_UNIFIED_TEXTURE_STATS stats{};
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetUnifiedTextureStats(dev.Get(), &stats)));
        OS_REQUIRE(stats.RegisteredCount == 1u);
        OS_REQUIRE(stats.DirtyCount == 0u);
        OS_REQUIRE(stats.TotalRemapNotifications >= 2ull);
        OS_REQUIRE(stats.TotalFlushes >= 3ull);
        OS_REQUIRE(stats.TotalBytesUploaded >= kTexBytes * 3u);

        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12UnregisterUnifiedTexture(dev.Get(), placed_tex.Get())));
        OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetUnifiedTextureStats(dev.Get(), &stats)));
        OS_REQUIRE(stats.RegisteredCount == 0u);

        std::printf(
            "  [UnifiedMem] Cyan -> InPlaceRemap(Orange) -> NewBackingRemap(Magenta) | Flushes=%llu Uploaded=%llu B OK\n",
            static_cast<unsigned long long>(stats.TotalFlushes),
            static_cast<unsigned long long>(stats.TotalBytesUploaded));
    }

    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    dev.Reset();
    OS_REQUIRE(DestroyWindow(hwnd) != FALSE);
    OpenScarlettD3D12SetPreferWarp(0);
}

// ---------------------------------------------------------------------------
// 6. Fail-Closed Unimplemented Stubs + CallTelemetry Blocker Verification
// ---------------------------------------------------------------------------
void TestFailClosedStubs() {
    std::printf("\n[Test 6] Fail-closed unimplemented stubs (d3d12_xs, xcrdapi, xg_xs)...\n");

    // 6a. d3d12_xs.dll
    OpenScarlettD3D12ResetTelemetry();
    void* dummy_q = reinterpret_cast<void*>(0x1);
    OS_REQUIRE(D3D12XboxCreateDmaCommandQueue(nullptr, nullptr, IID_ID3D12CommandQueueX, &dummy_q) == E_NOTIMPL);
    OS_REQUIRE(dummy_q == nullptr);
    OS_REQUIRE(D3D12XboxEnableDccMetadata(nullptr, nullptr, 0u) == E_NOTIMPL);

    int32_t slot = 0;
    uint64_t total = 0, ok_c = 0, ni_c = 0, err_c = 0;
    int32_t last_hr = 0;
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryDetail(
                   "D3D12XboxCreateDmaCommandQueue", &slot, &total, &ok_c, &ni_c, &err_c, &last_hr) == 1);
    OS_REQUIRE(total == 1ull && ok_c == 0ull && ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));

    OS_REQUIRE(OpenScarlettD3D12GetTelemetryDetail(
                   "D3D12XboxEnableDccMetadata", &slot, &total, &ok_c, &ni_c, &err_c, &last_hr) == 1);
    OS_REQUIRE(total == 1ull && ok_c == 0ull && ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));

    // 6b. xcrdapi.dll (keep implemented XCrd* working, verify unimplemented crash dump APIs fail closed)
    OS_REQUIRE(XCrdInitialize(1u) == OS_SHIM_S_OK);
    HMODULE crd_mod = GetModuleHandleA("xcrdapi.dll");
    OS_REQUIRE(crd_mod != nullptr);

    using PfnCrdMiniDump = int32_t(OS_SHIM_CALL*)(uint32_t, const char*);
    using PfnCrdUpload = int32_t(OS_SHIM_CALL*)(const char*, uint32_t);
    using PfnCrdTelReset = void(OS_SHIM_CALL*)(void);
    using PfnCrdTelDetail = int(OS_SHIM_CALL*)(const char*, uint64_t*, uint64_t*, int32_t*);

    auto fn_minidump = reinterpret_cast<PfnCrdMiniDump>(GetProcAddress(crd_mod, "XCrdStartMiniDumpCapture"));
    auto fn_upload = reinterpret_cast<PfnCrdUpload>(GetProcAddress(crd_mod, "XCrdUploadCrashDump"));
    auto fn_crd_reset = reinterpret_cast<PfnCrdTelReset>(GetProcAddress(crd_mod, "OpenScarlettXCrdResetTelemetry"));
    auto fn_crd_detail = reinterpret_cast<PfnCrdTelDetail>(GetProcAddress(crd_mod, "OpenScarlettXCrdGetTelemetryDetail"));
    OS_REQUIRE(fn_minidump && fn_upload && fn_crd_reset && fn_crd_detail);

    fn_crd_reset();
    OS_REQUIRE(fn_minidump(0u, "dump.dmp") == static_cast<int32_t>(E_NOTIMPL));
    OS_REQUIRE(fn_upload("dump.dmp", 0u) == static_cast<int32_t>(E_NOTIMPL));

    OS_REQUIRE(fn_crd_detail("XCrdStartMiniDumpCapture", &total, &ni_c, &last_hr) == 1);
    OS_REQUIRE(total == 1ull && ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));
    OS_REQUIRE(fn_crd_detail("XCrdUploadCrashDump", &total, &ni_c, &last_hr) == 1);
    OS_REQUIRE(total == 1ull && ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));
    XCrdUninitialize();

    // 6c. xg_xs.dll (MSAA sample > 0 & hardware DCC layout fail closed with E_NOTIMPL + telemetry)
    OpenScarlettXgResetTelemetry();
    XG_TEXTURE2D_DESC xg_desc{};
    xg_desc.Width = 64u;
    xg_desc.Height = 64u;
    xg_desc.MipLevels = 1u;
    xg_desc.ArraySize = 1u;
    xg_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    xg_desc.SampleCount = 1u;
    xg_desc.TileMode = XG_TILE_MODE_2D_THIN_64KB_S;
    IXGTextureAddressComputer* comp = nullptr;
    OS_REQUIRE(XGCreateTexture2DComputer(&xg_desc, &comp) == 0 && comp != nullptr);
    OS_REQUIRE(comp->GetTexelElementOffsetBytes(0u, 0u, 0u, 0u, 0u, 0u) != UINT64_MAX);
    OS_REQUIRE(comp->GetTexelElementOffsetBytes(0u, 0u, 0u, 0u, 0u, 1u) == UINT64_MAX);
    comp->Release();

    HMODULE xg_mod = GetModuleHandleA("xg_xs.dll");
    OS_REQUIRE(xg_mod != nullptr);
    using PfnXgDcc = int32_t(__stdcall*)(const XG_TEXTURE2D_DESC*, void*, uint64_t);
    using PfnXgDetail = int(__stdcall*)(const char*, uint64_t*, uint64_t*, int32_t*);
    auto fn_xg_dcc = reinterpret_cast<PfnXgDcc>(GetProcAddress(xg_mod, "XGComputeDccMetadataLayout"));
    auto fn_xg_detail = reinterpret_cast<PfnXgDetail>(GetProcAddress(xg_mod, "OpenScarlettXgGetTelemetryDetail"));
    OS_REQUIRE(fn_xg_dcc && fn_xg_detail);

    OS_REQUIRE(fn_xg_dcc(&xg_desc, nullptr, 0ull) == static_cast<int32_t>(E_NOTIMPL));
    OS_REQUIRE(fn_xg_detail("XGComputeDccMetadataLayout", &total, &ni_c, &last_hr) == 1);
    OS_REQUIRE(total == 1ull && ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));
    OS_REQUIRE(fn_xg_detail("IXGTextureAddressComputer::GetTexelElementOffsetBytes", &total, &ni_c, &last_hr) == 1);
    OS_REQUIRE(ni_c == 1ull && last_hr == static_cast<int32_t>(E_NOTIMPL));

    std::printf("  All unimplemented stubs returned explicit E_NOTIMPL + NotImplemented telemetry OK\n");
}

}  // namespace

int main() {
    TestPqAndToneMapMath();

    openscarlett::D3D12AdapterInfo hw_info{};
    const bool has_hw = HasHardwareD3D12Gpu(hw_info);
    if (has_hw) {
        std::printf("Detected hardware D3D12 GPU: %s\n", hw_info.description.c_str());
        RunLiveGpuSuite(false);
    } else {
        std::printf("No discrete hardware D3D12 GPU detected (WARP-only runner); running WARP suite.\n");
    }

    RunLiveGpuSuite(true);
    TestFailClosedStubs();

    std::printf("\n[PASS] test_hdr10_d3d12 (%s + D3D12 WARP verified)\n",
                has_hw ? hw_info.description.c_str() : "WARP-only");
    return 0;
}
