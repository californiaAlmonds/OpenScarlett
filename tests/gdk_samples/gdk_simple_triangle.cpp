// OpenScarlett MIT Standalone Xbox-ABI Guest Sample 1:
// Mirrors Microsoft Xbox-GDK-Samples/Samples/IntroGraphics/SimpleTriangle.
//
// Deliberately does NOT link openscarlett_core. Runs inside an isolated package
// directory with ZERO shim DLLs next to the .exe, launched via `openscarlett run`.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/input/gameinput_abi.h"

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;

constexpr uint8_t kBgR = 0x14u;
constexpr uint8_t kBgG = 0x18u;
constexpr uint8_t kBgB = 0x24u;
constexpr uint8_t kBgA = 0xFFu;

template <typename T>
class GuestComPtr {
public:
    GuestComPtr() = default;
    ~GuestComPtr() { Reset(); }
    GuestComPtr(const GuestComPtr&) = delete;
    GuestComPtr& operator=(const GuestComPtr&) = delete;

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

struct Vertex {
    float position[4];
    float color[4];
};

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_simple_triangle] FAIL: %s (code=0x%llX)\n",
                 step, static_cast<unsigned long long>(code));
    std::fflush(stderr);
    return 1;
}

bool ReadWholeFileW(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD total = 0;
    while (total < static_cast<DWORD>(out.size())) {
        DWORD chunk = 0;
        if (!::ReadFile(h, out.data() + total,
                        static_cast<DWORD>(out.size()) - total, &chunk, nullptr) ||
            chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool WriteWholeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written_total = 0;
    while (written_total < size) {
        DWORD chunk = 0;
        const DWORD to_write = static_cast<DWORD>(size - written_total);
        if (!::WriteFile(h, bytes + written_total, to_write, &chunk, nullptr) || chunk == 0) {
            ::CloseHandle(h);
            return false;
        }
        written_total += chunk;
    }
    ::CloseHandle(h);
    return true;
}

bool WritePpmW(const wchar_t* path, uint32_t width, uint32_t height, const uint8_t* rgba8) {
    char header[64]{};
    const int hdr_len = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", width, height);
    if (hdr_len <= 0) return false;

    const size_t pixel_count = static_cast<size_t>(width) * height;
    std::vector<uint8_t> ppm;
    ppm.reserve(static_cast<size_t>(hdr_len) + pixel_count * 3u);
    ppm.insert(ppm.end(), header, header + hdr_len);
    for (size_t i = 0; i < pixel_count; ++i) {
        ppm.push_back(rgba8[i * 4u + 0u]);
        ppm.push_back(rgba8[i * 4u + 1u]);
        ppm.push_back(rgba8[i * 4u + 2u]);
    }
    return WriteWholeFileW(path, ppm.data(), ppm.size());
}

double QpcMs(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq) {
    if (freq.QuadPart == 0) return 0.0;
    return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
           static_cast<double>(freq.QuadPart);
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        }
    }

    // 1. GDK Game Runtime + Scarlett device type + GameInput probe
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        return Fail("XGameRuntimeInitialize");
    }
    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    {
        IGameInput* gi = nullptr;
        const int32_t gi_hr = GameInputCreate(&gi);
        if (gi_hr != GAMEINPUT_S_OK || !gi) {
            return Fail("GameInputCreate", gi_hr);
        }
        IGameInputReading* reading = nullptr;
        if (gi->GetCurrentReading(GameInputKindGamepad, nullptr, &reading) == GAMEINPUT_S_OK && reading) {
            GameInputGamepadState gp{};
            (void)reading->GetGamepadState(&gp);
            reading->Release();
        }
        gi->Release();
    }

    // 2. Create Xbox Scarlett D3D12X device
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.DisableGeometryShaderAllocations = 1;
    dev_params.DisableTessellationShaderAllocations = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if ((gpu_info.IsWarp != 0u) != use_warp) {
        return Fail("Adapter WARP mode mismatch", gpu_info.IsWarp);
    }

    // 3. Query supported Shader Model (GTX 1650 Ti = SM 6.5; Win10 WARP = SM 6.2)
    //    and load precompiled Microsoft SimpleTriangle DXIL from virtual G:\ package.
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    bool supports_sm65 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                         sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

    std::vector<uint8_t> vs_blob;
    std::vector<uint8_t> ps_blob;
    const wchar_t* vs_path = supports_sm65 ? L"G:\\VertexShader.cso" : L"G:\\VertexShader_sm62.cso";
    const wchar_t* ps_path = supports_sm65 ? L"G:\\PixelShader.cso" : L"G:\\PixelShader_sm62.cso";
    if (!ReadWholeFileW(vs_path, vs_blob)) {
        return Fail("Read VertexShader CSO from G:\\", ::GetLastError());
    }
    if (!ReadWholeFileW(ps_path, ps_blob)) {
        return Fail("Read PixelShader CSO from G:\\", ::GetLastError());
    }

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        return Fail("SetFrameIntervalX", hr);
    }
    D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
    hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
    if (FAILED(hr) || frame_token == 0) {
        return Fail("WaitFrameEventX", hr);
    }

    // 4. CommandQueueX, CommandAllocator, CommandListX
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        return Fail("CreateCommandQueue", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        return Fail("CreateCommandList", hr);
    }

    // 5. Create committed RT texture with REAL Xbox flags:
    //    D3D12_HEAP_FLAG_ALLOW_DISPLAY + D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC (0x80000000)
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kWidth;
    rt_desc.Height = kHeight;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_val.Color[0] = static_cast<float>(kBgR) / 255.0f;
    clear_val.Color[1] = static_cast<float>(kBgG) / 255.0f;
    clear_val.Color[2] = static_cast<float>(kBgB) / 255.0f;
    clear_val.Color[3] = 1.0f;

    GuestComPtr<ID3D12Resource> rt;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_ALLOW_DISPLAY,
        &rt_desc,
        D3D12_RESOURCE_STATE_COMMON,
        &clear_val,
        IID_PPV_ARGS(rt.Put()));
    if (FAILED(hr) || !rt) {
        return Fail("CreateCommittedResource(ALLOW_DISPLAY|ALLOW_DCC)", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(rt.Get(), nullptr, rtv_handle);

    // 6. Create Root Signature directly from compiled VertexShader.cso bytecode
    //    (exact Microsoft SimpleTriangle.cpp line 179 pattern!)
    GuestComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, vs_blob.data(), vs_blob.size(), IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        return Fail("CreateRootSignature(from VertexShader.cso)", hr);
    }

    // 7. Create Graphics PSO matching Microsoft SimpleTriangle.cpp
    static const D3D12_INPUT_ELEMENT_DESC kInputElements[2] = {
        {"SV_Position", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.InputLayout = {kInputElements, 2};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.VS = {vs_blob.data(), vs_blob.size()};
    pso_desc.PS = {ps_blob.data(), ps_blob.size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    GuestComPtr<ID3D12PipelineState> pso;
    hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()));
    if (FAILED(hr) || !pso) {
        return Fail("CreateGraphicsPipelineState", hr);
    }

    // 8. Upload 3-vertex RGB triangle VB (exact Microsoft SimpleTriangle.cpp geometry!)
    static const Vertex kVertices[3] = {
        {{0.0f, 0.5f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},   // Top / Red
        {{0.5f, -0.5f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},  // Right / Green
        {{-0.5f, -0.5f, 0.5f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}, // Left / Blue
    };

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC vb_desc{};
    vb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vb_desc.Width = sizeof(kVertices);
    vb_desc.Height = 1;
    vb_desc.DepthOrArraySize = 1;
    vb_desc.MipLevels = 1;
    vb_desc.Format = DXGI_FORMAT_UNKNOWN;
    vb_desc.SampleDesc.Count = 1;
    vb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    GuestComPtr<ID3D12Resource> vb;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vb.Put()));
    if (FAILED(hr) || !vb) {
        return Fail("CreateCommittedResource(VB)", hr);
    }

    void* vb_mapped = nullptr;
    D3D12_RANGE read_none{0, 0};
    hr = vb->Map(0, &read_none, &vb_mapped);
    if (FAILED(hr) || !vb_mapped) {
        return Fail("VB Map", hr);
    }
    std::memcpy(vb_mapped, kVertices, sizeof(kVertices));
    vb->Unmap(0, nullptr);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(kVertices);

    // 9. Record & execute frame + PresentX
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = rt.Get();
    to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &to_rt);

    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv_handle, clear_val.Color, 0, nullptr);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);

    cmd->SetGraphicsRootSignature(root_sig.Get());
    cmd->SetPipelineState(pso.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER to_present{};
    to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_present.Transition.pResource = rt.Get();
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &to_present);

    hr = cmd->Close();
    if (FAILED(hr)) {
        return Fail("CommandListX::Close", hr);
    }

    ID3D12CommandList* cmd_lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, cmd_lists);

    ID3D12Resource* plane_res[] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = frame_token;
    plane.ResourceCount = 1;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres_params{};
    pres_params.SyncInterval = 1;

    hr = queue->PresentX(1, &plane, &pres_params);
    if (FAILED(hr)) {
        return Fail("PresentX(1)", hr);
    }
    ::QueryPerformanceCounter(&qpc_end);
    const double frame_ms = QpcMs(qpc_start, qpc_end, qpc_freq);

    // 10. Read back frame hash & pixels, verify triangle colors and write T:\simple_triangle.ppm
    uint64_t frame_hash = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    hr = OpenScarlettD3D12GetLastPresentHash(queue.Get(), &frame_hash, &out_w, &out_h);
    if (FAILED(hr) || out_w != kWidth || out_h != kHeight || frame_hash == 0ull) {
        return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied);
    if (FAILED(hr) || copied != pixels.size()) {
        return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
    }

    auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return &pixels[(static_cast<size_t>(y) * kWidth + x) * 4u];
    };

    // Corner (0,0) must be exact background clear color
    const uint8_t* bg = pixel_at(0, 0);
    if (bg[0] != kBgR || bg[1] != kBgG || bg[2] != kBgB || bg[3] != kBgA) {
        return Fail("Background clear pixel mismatch");
    }

    // Center (128,128) must be interpolated non-background RGB (R~127, G~64, B~64)
    const uint8_t* center = pixel_at(128, 128);
    if (center[0] < 90u || center[1] < 40u || center[2] < 40u || center[3] != 255u) {
        return Fail("Center triangle pixel verification failed");
    }

    // Near top vertex (128, 72) -> predominantly Red
    const uint8_t* top_px = pixel_at(128, 72);
    if (top_px[0] < 200u || top_px[1] > 45u || top_px[2] > 45u) {
        return Fail("Top Red vertex pixel verification failed");
    }
    // Near right vertex (184, 188) -> predominantly Green
    const uint8_t* right_px = pixel_at(184, 188);
    if (right_px[1] < 200u || right_px[0] > 45u || right_px[2] > 45u) {
        return Fail("Right Green vertex pixel verification failed");
    }
    // Near left vertex (72, 188) -> predominantly Blue
    const uint8_t* left_px = pixel_at(72, 188);
    if (left_px[2] < 200u || left_px[0] > 45u || left_px[1] > 45u) {
        return Fail("Left Blue vertex pixel verification failed");
    }

    uint32_t non_bg_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(4096);
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
        if (r != kBgR || g != kBgG || b != kBgB) {
            ++non_bg_pixels;
        }
        unique_rgb.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b));
    }
    if (non_bg_pixels < 7500u || non_bg_pixels > 9000u) {
        return Fail("Unexpected triangle rasterized pixel count", non_bg_pixels);
    }
    if (unique_rgb.size() < 1000u) {
        return Fail("Too few interpolated colors in RGB triangle",
                    static_cast<int64_t>(unique_rgb.size()));
    }

    if (!WritePpmW(L"T:\\simple_triangle.ppm", kWidth, kHeight, pixels.data())) {
        return Fail("Write T:\\simple_triangle.ppm", ::GetLastError());
    }

    char metrics_buf[512]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=SimpleTriangle\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "width=%u\n"
        "height=%u\n"
        "fnv1a=0x%016llX\n"
        "non_bg_pixels=%u\n"
        "unique_colors=%zu\n"
        "frame_ms=%.3f\n",
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        out_w,
        out_h,
        static_cast<unsigned long long>(frame_hash),
        non_bg_pixels,
        unique_rgb.size(),
        frame_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    // 11. Blank teardown PresentX(0, nullptr, nullptr) — exact Microsoft DeviceResources pattern!
    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_simple_triangle] PASS | Adapter=%s (%s) | %ux%u | NonBG=%u | Colors=%zu | FNV1a=0x%016llX | %.2f ms\n",
        gpu_info.AdapterName,
        use_warp ? "WARP" : "HW",
        out_w,
        out_h,
        non_bg_pixels,
        unique_rgb.size(),
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    std::fflush(stdout);

    vb.Reset();
    pso.Reset();
    root_sig.Reset();
    rtv_heap.Reset();
    rt.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    XGameRuntimeUninitialize();
    return 0;
}
