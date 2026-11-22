// OpenScarlett — Real _GAMING_XBOX_SCARLETT (GDKX) ABI & PLM Unit Test Suite
// Validates:
//  1. DeviceX / CommandQueueX / CommandListX COM QueryInterface zero raw-host escape (ID3D12Device..Device12 + unsupported Device14)
//  2. DeviceX IDXGIDevice / IDXGIDevice1 -> GetAdapter() -> EnumOutputs(0) -> GetDesc() on GPU + WARP
//  3. GXDK 221000+ D3D12XBOX_CREATE_DEVICE_PARAMETERS layout safety (AmplificationShader* fields never misread)
//  4. Real 5-arg WaitFrameEventX and 3-arg PresentX (blank PlaneCount=0 and live PlaneCount=1 + PNG write)
//  5. Xbox-only heap/resource flag sanitization (D3D12_HEAP_FLAG_ALLOW_DISPLAY, D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC)
//  6. appnotify.h PLM lifecycle exports on BOTH kernelx.dll and xgameruntime.dll

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/kernel/kernelx_abi.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* expr, const std::string& detail = {}) {
    if (!cond) {
        ++g_failures;
        std::cerr << "[FAIL] " << expr;
        if (!detail.empty()) std::cerr << " (" << detail << ")";
        std::cerr << "\n";
    }
}

#define EXPECT(c) Check((c), #c)
#define EXPECT_MSG(c, m) Check((c), #c, (m))

struct ScopedEnv {
    std::string name;
    bool had_old = false;
    std::string old_val;

    ScopedEnv(const char* k, const char* v) : name(k) {
        char buf[512]{};
        const DWORD n = ::GetEnvironmentVariableA(k, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            had_old = true;
            old_val.assign(buf, n);
        }
        ::SetEnvironmentVariableA(k, v);
    }
    ~ScopedEnv() {
        ::SetEnvironmentVariableA(name.c_str(), had_old ? old_val.c_str() : nullptr);
    }
};

// Standard Windows D3D12 device IIDs (Device9..Device14) defined explicitly so
// this test compiles identically across MSVC, Clang-CL, and MinGW-w64 SDKs.
static constexpr GUID kIID_ID3D12Device9 = {
    0x4c80e962, 0xf032, 0x4f60, {0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c}};
static constexpr GUID kIID_ID3D12Device10 = {
    0x517f8718, 0xaa66, 0x49f9, {0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0xf6}};
static constexpr GUID kIID_ID3D12Device11 = {
    0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
static constexpr GUID kIID_ID3D12Device12 = {
    0x5af0d7e3, 0x49a6, 0x4c60, {0x8e, 0xd8, 0x24, 0x10, 0x0b, 0x63, 0xf4, 0xd9}};
static constexpr GUID kIID_ID3D12Device14 = {
    0x5f656329, 0x0584, 0x4f3e, {0x9d, 0x9d, 0x67, 0x41, 0x59, 0x5f, 0x54, 0x1a}};
static constexpr GUID kIID_ID3D12GraphicsCommandList7 = {
    0xdd171223, 0x8b61, 0x4769, {0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1}};

void TestDeviceComZeroEscape(bool prefer_warp) {
    const std::string tag = prefer_warp ? "WARP" : "GPU";
    ScopedEnv warp_env("OPENSCARLETT_PREFER_WARP", prefer_warp ? "1" : "0");

    // 1. Populate real GXDK 221000+ fields (including AmplificationShader* at the
    //    exact byte offsets where legacy custom OutputWindow/PreferWarp used to sit!)
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;
    params.GraphicsCommandQueueRingSizeBytes = static_cast<uint32_t>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.DisableGeometryShaderAllocations = TRUE;
    params.DisableTessellationShaderAllocations = TRUE;
    params.DisableDXR = TRUE;
    params.CreateDeviceFlags = D3D12XBOX_CREATE_DEVICE_FLAG_NONE;
    params.AmplificationShaderIndirectArgsBufferSize = 0x12345678u;
    params.AmplificationShaderPayloadBufferSize = 0x87654321u;

    // Request ID3D12Device8 directly — exactly what untouched SimpleTriangle does!
    ID3D12Device8* dev8 = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12Device8, reinterpret_cast<void**>(&dev8));
    EXPECT_MSG(SUCCEEDED(hr) && dev8 != nullptr, tag + ": D3D12XboxCreateDevice(ID3D12Device8)");
    if (!dev8) return;

    // Canonical IUnknown identity of DeviceX
    IUnknown* canon_unk = nullptr;
    EXPECT_MSG(SUCCEEDED(dev8->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&canon_unk))) &&
                   canon_unk != nullptr,
               tag + ": dev8->QI(IUnknown)");

    // Prove dev8 IS DeviceX (not raw Windows ID3D12Device8!)
    ID3D12DeviceX* devx = nullptr;
    EXPECT_MSG(SUCCEEDED(dev8->QueryInterface(IID_ID3D12DeviceX, reinterpret_cast<void**>(&devx))) &&
                   devx != nullptr,
               tag + ": dev8->QI(ID3D12DeviceX) proves zero raw-host escape");

    // Verify ALL ID3D12Device..ID3D12Device8 IIDs return the same DeviceX identity
    const GUID core_device_iids[] = {
        IID_ID3D12Device,  IID_ID3D12Device1, IID_ID3D12Device2, IID_ID3D12Device3,
        IID_ID3D12Device4, IID_ID3D12Device5, IID_ID3D12Device6, IID_ID3D12Device7,
        IID_ID3D12Device8,
    };
    for (size_t i = 0; i < std::size(core_device_iids); ++i) {
        IUnknown* iface = nullptr;
        hr = dev8->QueryInterface(core_device_iids[i], reinterpret_cast<void**>(&iface));
        EXPECT_MSG(SUCCEEDED(hr) && iface != nullptr, tag + ": QI(ID3D12Device" + std::to_string(i) + ")");
        if (iface) {
            IUnknown* unk = nullptr;
            EXPECT(SUCCEEDED(iface->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unk))));
            EXPECT_MSG(unk == canon_unk, tag + ": COM identity match for ID3D12Device" + std::to_string(i));
            if (unk) unk->Release();

            ID3D12DeviceX* back_to_x = nullptr;
            EXPECT_MSG(SUCCEEDED(iface->QueryInterface(IID_ID3D12DeviceX, reinterpret_cast<void**>(&back_to_x))) &&
                           back_to_x != nullptr,
                       tag + ": round-trip QI(ID3D12DeviceX) from ID3D12Device" + std::to_string(i));
            if (back_to_x) back_to_x->Release();
            iface->Release();
        }
    }

    // Verify ID3D12Device9..12 (if supported by host OS) ALSO keep DeviceX identity (never raw host!)
    const GUID ext_device_iids[] = {
        kIID_ID3D12Device9, kIID_ID3D12Device10, kIID_ID3D12Device11, kIID_ID3D12Device12};
    for (size_t i = 0; i < std::size(ext_device_iids); ++i) {
        IUnknown* iface = nullptr;
        hr = dev8->QueryInterface(ext_device_iids[i], reinterpret_cast<void**>(&iface));
        if (SUCCEEDED(hr) && iface) {
            IUnknown* unk = nullptr;
            EXPECT(SUCCEEDED(iface->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unk))));
            EXPECT_MSG(unk == canon_unk, tag + ": COM identity match for Device" + std::to_string(9 + i));
            if (unk) unk->Release();

            ID3D12DeviceX* back_to_x = nullptr;
            EXPECT_MSG(SUCCEEDED(iface->QueryInterface(IID_ID3D12DeviceX, reinterpret_cast<void**>(&back_to_x))) &&
                           back_to_x != nullptr,
                       tag + ": round-trip QI(ID3D12DeviceX) from Device" + std::to_string(9 + i));
            if (back_to_x) back_to_x->Release();
            iface->Release();
        }
    }

    // Unsupported future ID3D12Device14 MUST return E_NOINTERFACE (never fall through to raw host!)
    IUnknown* dev14 = nullptr;
    hr = dev8->QueryInterface(kIID_ID3D12Device14, reinterpret_cast<void**>(&dev14));
    EXPECT_MSG(hr == E_NOINTERFACE && dev14 == nullptr,
               tag + ": QI(ID3D12Device14) blocked from raw-host escape");

    // 2. Test IDXGIDevice1 -> GetAdapter() -> EnumOutputs(0) -> GetDesc()
    IDXGIDevice1* dxgi_dev1 = nullptr;
    hr = dev8->QueryInterface(IID_IDXGIDevice1, reinterpret_cast<void**>(&dxgi_dev1));
    EXPECT_MSG(SUCCEEDED(hr) && dxgi_dev1 != nullptr, tag + ": DeviceX->QI(IDXGIDevice1)");
    if (dxgi_dev1) {
        // Verify DXGI sub-interface shares COM identity with DeviceX
        IUnknown* dxgi_unk = nullptr;
        EXPECT(SUCCEEDED(dxgi_dev1->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&dxgi_unk))));
        EXPECT_MSG(dxgi_unk == canon_unk, tag + ": IDXGIDevice1 COM identity matches DeviceX");
        if (dxgi_unk) dxgi_unk->Release();

        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_dev1->GetAdapter(&adapter);
        EXPECT_MSG(SUCCEEDED(hr) && adapter != nullptr, tag + ": IDXGIDevice1::GetAdapter()");
        if (adapter) {
            DXGI_ADAPTER_DESC adesc{};
            EXPECT_MSG(SUCCEEDED(adapter->GetDesc(&adesc)), tag + ": IDXGIAdapter::GetDesc()");

            IDXGIOutput* output0 = nullptr;
            hr = adapter->EnumOutputs(0u, &output0);
            EXPECT_MSG(SUCCEEDED(hr) && output0 != nullptr, tag + ": IDXGIAdapter::EnumOutputs(0)");
            if (output0) {
                DXGI_OUTPUT_DESC odesc{};
                EXPECT_MSG(SUCCEEDED(output0->GetDesc(&odesc)), tag + ": IDXGIOutput::GetDesc()");
                EXPECT_MSG((odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left) >= 640,
                           tag + ": IDXGIOutput width >= 640");

                // Pass output0 into SetFrameIntervalX — exact DeviceResources::RegisterFrameEvents flow!
                EXPECT_MSG(SUCCEEDED(devx->SetFrameIntervalX(
                               output0, D3D12XBOX_FRAME_INTERVAL_60_HZ, 1u,
                               D3D12XBOX_FRAME_INTERVAL_FLAG_NONE)),
                           tag + ": SetFrameIntervalX(output0)");
                output0->Release();
            }
            adapter->Release();
        }
        dxgi_dev1->Release();
    }

    // 3. Test 5-arg WaitFrameEventX ABI
    EXPECT_MSG(SUCCEEDED(devx->ScheduleFrameEventX(
                   D3D12XBOX_FRAME_EVENT_ORIGIN, 0ull, nullptr,
                   D3D12XBOX_SCHEDULE_FRAME_EVENT_FLAG_NONE)),
               tag + ": ScheduleFrameEventX");

    // Real GDKX WaitFrameEventX has _Out_opt_ pOutToken — NULL must succeed safely with zero crash!
    hr = devx->WaitFrameEventX(
        D3D12XBOX_FRAME_EVENT_ORIGIN, 100u, nullptr,
        D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, nullptr);
    EXPECT_MSG(SUCCEEDED(hr), tag + ": WaitFrameEventX(_Out_opt_ nullptr) succeeds safely");

    D3D12XBOX_FRAME_PIPELINE_TOKEN tok1 = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
    D3D12XBOX_FRAME_PIPELINE_TOKEN tok2 = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
    EXPECT_MSG(SUCCEEDED(devx->WaitFrameEventX(
                   D3D12XBOX_FRAME_EVENT_ORIGIN, 100u, nullptr,
                   D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &tok1)) &&
                   tok1 != D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL,
               tag + ": WaitFrameEventX #1 non-zero token");
    EXPECT_MSG(SUCCEEDED(devx->WaitFrameEventX(
                   D3D12XBOX_FRAME_EVENT_ORIGIN, 100u, nullptr,
                   D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &tok2)) &&
                   tok2 > tok1,
               tag + ": WaitFrameEventX #2 monotonic token");

    // 4. Test Xbox-only heap/resource flag sanitization on CreateCommittedResource & CreateCommittedResource2
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CreationNodeMask = 1u;
    heap_props.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = 64u;
    rt_desc.Height = 64u;
    rt_desc.DepthOrArraySize = 1u;
    rt_desc.MipLevels = 1u;
    rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rt_desc.SampleDesc.Count = 1u;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC |
        D3D12XBOX_RESOURCE_FLAG_FORCE_TEXTURE_COMPATIBILITY);

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    clear_val.Color[0] = 0.0f;
    clear_val.Color[1] = 0.5f;
    clear_val.Color[2] = 1.0f;
    clear_val.Color[3] = 1.0f;

    ID3D12Resource* rt_res = nullptr;
    hr = dev8->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_val,
        IID_ID3D12Resource, reinterpret_cast<void**>(&rt_res));
    EXPECT_MSG(SUCCEEDED(hr) && rt_res != nullptr,
               tag + ": CreateCommittedResource with ALLOW_DISPLAY + ALLOW_DCC");

    D3D12_RESOURCE_DESC1 rt_desc1{};
    rt_desc1.Dimension = rt_desc.Dimension;
    rt_desc1.Width = rt_desc.Width;
    rt_desc1.Height = rt_desc.Height;
    rt_desc1.DepthOrArraySize = rt_desc.DepthOrArraySize;
    rt_desc1.MipLevels = rt_desc.MipLevels;
    rt_desc1.Format = rt_desc.Format;
    rt_desc1.SampleDesc = rt_desc.SampleDesc;
    rt_desc1.Layout = rt_desc.Layout;
    rt_desc1.Flags = rt_desc.Flags;

    ID3D12Resource* rt_res2 = nullptr;
    hr = dev8->CreateCommittedResource2(
        &heap_props, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc1,
        D3D12_RESOURCE_STATE_PRESENT, &clear_val, nullptr,
        IID_ID3D12Resource, reinterpret_cast<void**>(&rt_res2));
    EXPECT_MSG(SUCCEEDED(hr) && rt_res2 != nullptr,
               tag + ": ID3D12Device8::CreateCommittedResource2 with ALLOW_DISPLAY + ALLOW_DCC");
    if (rt_res2) rt_res2->Release();

    // 5. CommandQueueX & CommandListX QI zero-escape + 3-arg PresentX (blank + live + PNG)
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* raw_q = nullptr;
    hr = dev8->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&raw_q));
    EXPECT_MSG(SUCCEEDED(hr) && raw_q != nullptr, tag + ": CreateCommandQueue(ID3D12CommandQueue)");

    ID3D12CommandQueueX* qx = nullptr;
    if (raw_q) {
        EXPECT_MSG(SUCCEEDED(raw_q->QueryInterface(IID_ID3D12CommandQueueX, reinterpret_cast<void**>(&qx))) &&
                       qx != nullptr,
                   tag + ": CommandQueue->QI(ID3D12CommandQueueX) zero raw-host escape");
    }

    ID3D12CommandAllocator* alloc = nullptr;
    EXPECT_MSG(SUCCEEDED(dev8->CreateCommandAllocator(
                   D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                   reinterpret_cast<void**>(&alloc))) &&
                   alloc != nullptr,
               tag + ": CreateCommandAllocator");

    ID3D12GraphicsCommandList6* cl6 = nullptr;
    if (alloc) {
        hr = dev8->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
            IID_ID3D12GraphicsCommandList6, reinterpret_cast<void**>(&cl6));
        EXPECT_MSG(SUCCEEDED(hr) && cl6 != nullptr, tag + ": CreateCommandList(ID3D12GraphicsCommandList6)");
    }

    if (cl6) {
        ID3D12GraphicsCommandListX* clx = nullptr;
        EXPECT_MSG(SUCCEEDED(cl6->QueryInterface(
                       IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(&clx))) &&
                       clx != nullptr,
                   tag + ": CommandList6->QI(ID3D12GraphicsCommandListX) zero raw-host escape");
        if (clx) clx->Release();

        // Check extended CL7 QI (if host supports it) preserves CommandListX identity
        IUnknown* cl_canon = nullptr;
        EXPECT(SUCCEEDED(cl6->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&cl_canon))));
        IUnknown* cl7 = nullptr;
        if (SUCCEEDED(cl6->QueryInterface(kIID_ID3D12GraphicsCommandList7, reinterpret_cast<void**>(&cl7))) && cl7) {
            IUnknown* cl7_unk = nullptr;
            EXPECT(SUCCEEDED(cl7->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&cl7_unk))));
            EXPECT_MSG(cl7_unk == cl_canon, tag + ": CommandList7 COM identity matches CommandListX");
            if (cl7_unk) cl7_unk->Release();
            cl7->Release();
        }
        if (cl_canon) cl_canon->Release();
    }

    if (qx && cl6 && rt_res) {
        // Blank PresentX (PlaneCount=0, nullptr, nullptr) — exact DeviceResources init/teardown path!
        EXPECT_MSG(SUCCEEDED(qx->PresentX(0u, nullptr, nullptr)),
                   tag + ": PresentX(0, nullptr, nullptr) blank present");

        // Clear RTV to a known color and PresentX(1, &plane, nullptr)
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1u;
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        EXPECT(SUCCEEDED(dev8->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtv_heap))));
        if (rtv_heap) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            dev8->CreateRenderTargetView(rt_res, nullptr, rtv);
            cl6->ClearRenderTargetView(rtv, clear_val.Color, 0u, nullptr);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = rt_res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cl6->ResourceBarrier(1u, &b);
            EXPECT(SUCCEEDED(cl6->Close()));

            ID3D12CommandList* lists[] = {cl6};
            qx->ExecuteCommandLists(1u, lists);

            std::filesystem::path out_dir;
            char tmp_buf[1024]{};
            const DWORD tmp_len = ::GetEnvironmentVariableA("TEMP", tmp_buf, sizeof(tmp_buf));
            if (tmp_len > 0 && tmp_len < sizeof(tmp_buf)) {
                out_dir = std::filesystem::path(tmp_buf);
            } else {
                wchar_t self_buf[MAX_PATH]{};
                const DWORD slen = ::GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
                out_dir = (slen > 0 && slen < MAX_PATH)
                              ? std::filesystem::path(self_buf).parent_path()
                              : std::filesystem::path(OPENSCARLETT_BINARY_DIR);
            }
            const std::filesystem::path png_path = out_dir / ("test_gdkx_abi_" + tag + ".png");
            std::error_code ec;
            std::filesystem::remove(png_path, ec);

            ScopedEnv cap_env("OPENSCARLETT_PRESENTX_CAPTURE_PNG", png_path.string().c_str());
            ScopedEnv hl_env("OPENSCARLETT_PRESENTX_HEADLESS", "1");

            D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
            plane.Token = tok2;
            plane.ResourceCount = 1u;
            plane.ppResources = &rt_res;
            plane.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            EXPECT_MSG(SUCCEEDED(qx->PresentX(1u, &plane, nullptr)),
                       tag + ": 3-arg PresentX(1, &plane, nullptr)");

            // Verify PNG file was written with valid PNG signature
            std::ifstream pf(png_path, std::ios::binary);
            uint8_t sig[8]{};
            pf.read(reinterpret_cast<char*>(sig), 8);
            const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            EXPECT_MSG(pf.good() && std::memcmp(sig, kPngSig, 8) == 0,
                       tag + ": PresentX PNG capture has valid PNG header");
            rtv_heap->Release();
        }
    }

    if (cl6) cl6->Release();
    if (alloc) alloc->Release();
    if (rt_res) rt_res->Release();
    if (qx) qx->Release();
    if (raw_q) raw_q->Release();
    if (devx) devx->Release();
    if (canon_unk) canon_unk->Release();
    dev8->Release();
}

struct PlmCounter {
    std::atomic<int> state_calls{0};
    std::atomic<int> last_quiesced{-1};
    std::atomic<int> constrained_calls{0};
    std::atomic<int> last_constrained{-1};
};

void CALLBACK OnStateCallback(BOOLEAN quiesced, PVOID ctx) {
    auto* c = static_cast<PlmCounter*>(ctx);
    c->state_calls.fetch_add(1, std::memory_order_acq_rel);
    c->last_quiesced.store(quiesced ? 1 : 0, std::memory_order_release);
}

void CALLBACK OnConstrainedCallback(BOOLEAN constrained, PVOID ctx) {
    auto* c = static_cast<PlmCounter*>(ctx);
    c->constrained_calls.fetch_add(1, std::memory_order_acq_rel);
    c->last_constrained.store(constrained ? 1 : 0, std::memory_order_release);
}

struct SelfUnregCtx {
    std::atomic<int> calls{0};
    PAPPSTATE_REGISTRATION reg = nullptr;
    void(WINAPI* unreg_fn)(PAPPSTATE_REGISTRATION) = nullptr;
};

void CALLBACK OnStateSelfUnregister(BOOLEAN /*quiesced*/, PVOID ctx) {
    auto* s = static_cast<SelfUnregCtx*>(ctx);
    s->calls.fetch_add(1, std::memory_order_acq_rel);
    if (s->unreg_fn && s->reg) {
        s->unreg_fn(s->reg);
        s->reg = nullptr;
    }
}

void TestPlmExportsKernelX() {
    // Invalid parameter checks
    PAPPSTATE_REGISTRATION bad_state = nullptr;
    PAPPCONSTRAIN_REGISTRATION bad_cons = nullptr;
    EXPECT(RegisterAppStateChangeNotification(nullptr, nullptr, &bad_state) == ERROR_INVALID_PARAMETER);
    EXPECT(RegisterAppStateChangeNotification(OnStateCallback, nullptr, nullptr) == ERROR_INVALID_PARAMETER);
    EXPECT(RegisterAppConstrainedChangeNotification(nullptr, nullptr, &bad_cons) == ERROR_INVALID_PARAMETER);
    EXPECT(RegisterAppConstrainedChangeNotification(OnConstrainedCallback, nullptr, nullptr) ==
           ERROR_INVALID_PARAMETER);

    // Null unregister must safely no-op
    UnregisterAppStateChangeNotification(nullptr);
    UnregisterAppConstrainedChangeNotification(nullptr);

    PlmCounter c1{};
    PlmCounter c2{};
    PAPPSTATE_REGISTRATION r_state1 = nullptr;
    PAPPSTATE_REGISTRATION r_state2 = nullptr;
    PAPPCONSTRAIN_REGISTRATION r_cons1 = nullptr;

    EXPECT(RegisterAppStateChangeNotification(OnStateCallback, &c1, &r_state1) == ERROR_SUCCESS);
    EXPECT(r_state1 != nullptr);
    EXPECT(RegisterAppStateChangeNotification(OnStateCallback, &c2, &r_state2) == ERROR_SUCCESS);
    EXPECT(r_state2 != nullptr && r_state2 != r_state1);
    EXPECT(RegisterAppConstrainedChangeNotification(OnConstrainedCallback, &c1, &r_cons1) == ERROR_SUCCESS);
    EXPECT(r_cons1 != nullptr);

    // Re-entrant self-unregister callback proves snapshot dispatch doesn't deadlock
    SelfUnregCtx self_ctx{};
    self_ctx.unreg_fn = &UnregisterAppStateChangeNotification;
    EXPECT(RegisterAppStateChangeNotification(OnStateSelfUnregister, &self_ctx, &self_ctx.reg) ==
           ERROR_SUCCESS);

    uint32_t sc = 0, cc = 0;
    KernelXGetAppNotificationCounts(&sc, &cc);
    EXPECT(sc >= 3u && cc >= 1u);

    // Suspend -> Resume dispatch
    EXPECT(KernelXDispatchAppStateChange(TRUE) >= 3u);
    EXPECT(c1.state_calls.load() == 1 && c1.last_quiesced.load() == 1);
    EXPECT(c2.state_calls.load() == 1 && c2.last_quiesced.load() == 1);
    EXPECT(self_ctx.calls.load() == 1);

    EXPECT(KernelXDispatchAppStateChange(FALSE) >= 2u);
    EXPECT(c1.state_calls.load() == 2 && c1.last_quiesced.load() == 0);
    EXPECT(c2.state_calls.load() == 2 && c2.last_quiesced.load() == 0);
    EXPECT(self_ctx.calls.load() == 1);  // Unregistered itself on first dispatch!

    // Constrained -> Unconstrained dispatch
    EXPECT(KernelXDispatchAppConstrainedChange(TRUE) >= 1u);
    EXPECT(c1.constrained_calls.load() == 1 && c1.last_constrained.load() == 1);
    EXPECT(KernelXDispatchAppConstrainedChange(FALSE) >= 1u);
    EXPECT(c1.constrained_calls.load() == 2 && c1.last_constrained.load() == 0);

    // Unregister c1, keep c2
    UnregisterAppStateChangeNotification(r_state1);
    UnregisterAppConstrainedChangeNotification(r_cons1);

    KernelXDispatchAppStateChange(TRUE);
    EXPECT(c1.state_calls.load() == 2);
    EXPECT(c2.state_calls.load() == 3);

    KernelXDispatchAppConstrainedChange(TRUE);
    EXPECT(c1.constrained_calls.load() == 2);

    UnregisterAppStateChangeNotification(r_state2);
}

void TestPlmExportsXGameRuntime() {
    HMODULE xgr = ::LoadLibraryA("xgameruntime.dll");
    EXPECT_MSG(xgr != nullptr, "LoadLibraryA(xgameruntime.dll)");
    if (!xgr) return;

    using FnRegState = ULONG(WINAPI*)(PAPPSTATE_CHANGE_ROUTINE, PVOID, PAPPSTATE_REGISTRATION*);
    using FnUnregState = VOID(WINAPI*)(PAPPSTATE_REGISTRATION);
    using FnRegCons = ULONG(WINAPI*)(PAPPCONSTRAIN_CHANGE_ROUTINE, PVOID, PAPPCONSTRAIN_REGISTRATION*);
    using FnUnregCons = VOID(WINAPI*)(PAPPCONSTRAIN_REGISTRATION);
    using FnDispatch = uint32_t(WINAPI*)(BOOLEAN);

    auto* fn_reg_s = reinterpret_cast<FnRegState>(::GetProcAddress(xgr, "RegisterAppStateChangeNotification"));
    auto* fn_unreg_s = reinterpret_cast<FnUnregState>(::GetProcAddress(xgr, "UnregisterAppStateChangeNotification"));
    auto* fn_reg_c = reinterpret_cast<FnRegCons>(::GetProcAddress(xgr, "RegisterAppConstrainedChangeNotification"));
    auto* fn_unreg_c = reinterpret_cast<FnUnregCons>(::GetProcAddress(xgr, "UnregisterAppConstrainedChangeNotification"));
    auto* fn_disp_s = reinterpret_cast<FnDispatch>(::GetProcAddress(xgr, "XgrDispatchAppStateChange"));
    auto* fn_disp_c = reinterpret_cast<FnDispatch>(::GetProcAddress(xgr, "XgrDispatchAppConstrainedChange"));

    EXPECT(fn_reg_s != nullptr && fn_unreg_s != nullptr);
    EXPECT(fn_reg_c != nullptr && fn_unreg_c != nullptr);
    EXPECT(fn_disp_s != nullptr && fn_disp_c != nullptr);

    if (fn_reg_s && fn_unreg_s && fn_reg_c && fn_unreg_c && fn_disp_s && fn_disp_c) {
        PlmCounter c{};
        PAPPSTATE_REGISTRATION rs = nullptr;
        PAPPCONSTRAIN_REGISTRATION rc = nullptr;
        EXPECT(fn_reg_s(OnStateCallback, &c, &rs) == ERROR_SUCCESS && rs != nullptr);
        EXPECT(fn_reg_c(OnConstrainedCallback, &c, &rc) == ERROR_SUCCESS && rc != nullptr);

        EXPECT(fn_disp_s(TRUE) >= 1u && c.state_calls.load() == 1 && c.last_quiesced.load() == 1);
        EXPECT(fn_disp_c(TRUE) >= 1u && c.constrained_calls.load() == 1 && c.last_constrained.load() == 1);

        fn_unreg_s(rs);
        fn_unreg_c(rc);

        fn_disp_s(FALSE);
        fn_disp_c(FALSE);
        EXPECT(c.state_calls.load() == 1 && c.constrained_calls.load() == 1);
    }

    ::FreeLibrary(xgr);
}

}  // namespace

int main() {
    std::cout << "[test_gdkx_abi] Running COM zero-escape, DXGI output, GDKX ABI, and PLM tests...\n";
    TestDeviceComZeroEscape(/*prefer_warp=*/false);
    TestDeviceComZeroEscape(/*prefer_warp=*/true);
    TestPlmExportsKernelX();
    TestPlmExportsXGameRuntime();

    if (g_failures != 0) {
        std::cerr << "[test_gdkx_abi] FAILED with " << g_failures << " error(s).\n";
        return 1;
    }
    std::cout << "[test_gdkx_abi] ALL PASSED (0 failures).\n";
    return 0;
}
