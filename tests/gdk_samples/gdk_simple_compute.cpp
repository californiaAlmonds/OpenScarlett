// OpenScarlett MIT Standalone Xbox-ABI Guest Sample 3:
// Mirrors Microsoft Xbox-GDK-Samples/Samples/IntroGraphics/SimpleCompute (fractal.hlsl Mandelbrot).
//
// Deliberately does NOT link openscarlett_core. Runs on BOTH NVIDIA GTX 1650 Ti (HW)
// and D3D12 WARP via `openscarlett run` in an isolated package directory.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "openscarlett/gdk/xgameruntime_abi.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kPaletteWidth = 256u;

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

struct FractalConstants {
    float max_thread_iter[4];
    float window[4];
    float pad[56];
};
static_assert(sizeof(FractalConstants) == 256, "D3D12 CBV requires 256-byte size");

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_simple_compute] FAIL: %s (code=0x%llX)\n",
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

void BuildVibrantPalette(std::vector<uint8_t>& rgba) {
    rgba.resize(static_cast<size_t>(kPaletteWidth) * 4u, 0u);
    // Index 0 = pure black interior of Mandelbrot set (iter == maxiter -> frac(iter/z) == 0.0)
    rgba[0] = 0u;
    rgba[1] = 0u;
    rgba[2] = 0u;
    rgba[3] = 255u;

    // Indices 1..255 = rich rainbow HSV spectrum with distinct RGB values for every bucket
    for (uint32_t i = 1; i < kPaletteWidth; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kPaletteWidth - 1u);
        const float h = t * 6.0f;
        const int sector = static_cast<int>(h) % 6;
        const float f = h - std::floor(h);
        const float v = 0.35f + 0.65f * std::sin(t * 3.14159265f * 0.85f + 0.25f);
        const float s = 0.95f;
        const float p = v * (1.0f - s);
        const float q = v * (1.0f - s * f);
        const float u = v * (1.0f - s * (1.0f - f));
        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (sector) {
            case 0: r = v; g = u; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = u; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = u; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
        rgba[i * 4u + 0u] = static_cast<uint8_t>(std::clamp(r * 255.0f, 1.0f, 255.0f));
        rgba[i * 4u + 1u] = static_cast<uint8_t>(std::clamp(g * 255.0f, 1.0f, 255.0f));
        rgba[i * 4u + 2u] = static_cast<uint8_t>(std::clamp(b * 255.0f, 1.0f, 255.0f));
        rgba[i * 4u + 3u] = 255u;
    }
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

    if (XGameRuntimeInitialize() != XGR_S_OK) {
        return Fail("XGameRuntimeInitialize");
    }
    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    // 1. Create Xbox D3D12X device
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr) || ((gpu_info.IsWarp != 0u) != use_warp)) {
        return Fail("GetGpuHardwareInfoX / WARP mismatch", hr);
    }

    // 2. Query Shader Model (GTX 1650 Ti = SM 6.5; Win10 WARP = SM 6.2)
    //    and load precompiled Microsoft SimpleCompute/fractal.hlsl DXIL from G:\ .
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    bool supports_sm65 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                         sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;

    std::vector<uint8_t> cs_blob;
    const wchar_t* cs_path = supports_sm65 ? L"G:\\fractal.cso" : L"G:\\fractal_sm62.cso";
    if (!ReadWholeFileW(cs_path, cs_blob)) {
        return Fail("Read fractal CSO from G:\\", ::GetLastError());
    }

    D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
    hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
    if (FAILED(hr) || frame_token == 0) {
        return Fail("WaitFrameEventX", hr);
    }

    // 3. Create Root Signature directly from fractal.cso embedded [RootSignature(...)]
    //    Param 0: CBV(b0)
    //    Param 1: DescriptorTable(Sampler(s0))
    //    Param 2: DescriptorTable(SRV(t0))
    //    Param 3: DescriptorTable(UAV(u0))
    GuestComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, cs_blob.data(), cs_blob.size(), IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        return Fail("CreateRootSignature(from fractal.cso)", hr);
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cs_pso_desc{};
    cs_pso_desc.pRootSignature = root_sig.Get();
    cs_pso_desc.CS = {cs_blob.data(), cs_blob.size()};

    GuestComPtr<ID3D12PipelineState> compute_pso;
    hr = device->CreateComputePipelineState(&cs_pso_desc, IID_PPV_ARGS(compute_pso.Put()));
    if (FAILED(hr) || !compute_pso) {
        return Fail("CreateComputePipelineState", hr);
    }

    // 4. Queue, Allocator, CommandListX
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
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), compute_pso.Get(),
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        return Fail("CreateCommandList", hr);
    }

    // 5. Constant buffer (b0): g_MaxThreadIter={256,256,32,0}, g_Window={3.0,2.25,-0.5,0.0}
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC cb_desc{};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Width = sizeof(FractalConstants);
    cb_desc.Height = 1;
    cb_desc.DepthOrArraySize = 1;
    cb_desc.MipLevels = 1;
    cb_desc.Format = DXGI_FORMAT_UNKNOWN;
    cb_desc.SampleDesc.Count = 1;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    GuestComPtr<ID3D12Resource> cb_buf;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(cb_buf.Put()));
    if (FAILED(hr) || !cb_buf) {
        return Fail("CreateCommittedResource(CB)", hr);
    }

    {
        FractalConstants fc{};
        fc.max_thread_iter[0] = static_cast<float>(kWidth);
        fc.max_thread_iter[1] = static_cast<float>(kHeight);
        fc.max_thread_iter[2] = 32.0f;
        fc.max_thread_iter[3] = 0.0f;
        fc.window[0] = 3.0f;
        fc.window[1] = 2.25f;
        fc.window[2] = -0.5f;
        fc.window[3] = 0.0f;

        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        hr = cb_buf->Map(0, &read_none, &mapped);
        if (FAILED(hr) || !mapped) return Fail("CB Map", hr);
        std::memcpy(mapped, &fc, sizeof(fc));
        cb_buf->Unmap(0, nullptr);
    }

    // 6. Color palette texture (t0: 256x1 RGBA8) + staging upload buffer
    D3D12_RESOURCE_DESC pal_desc{};
    pal_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pal_desc.Width = kPaletteWidth;
    pal_desc.Height = 1;
    pal_desc.DepthOrArraySize = 1;
    pal_desc.MipLevels = 1;
    pal_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    pal_desc.SampleDesc.Count = 1;
    pal_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    GuestComPtr<ID3D12Resource> pal_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &pal_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(pal_tex.Put()));
    if (FAILED(hr) || !pal_tex) {
        return Fail("CreateCommittedResource(PaletteTex)", hr);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pal_fp{};
    UINT pal_rows = 0;
    UINT64 pal_row_bytes = 0;
    UINT64 pal_total_bytes = 0;
    device->GetCopyableFootprints(
        &pal_desc, 0, 1, 0, &pal_fp, &pal_rows, &pal_row_bytes, &pal_total_bytes);

    D3D12_RESOURCE_DESC pal_up_desc = cb_desc;
    pal_up_desc.Width = pal_total_bytes;
    GuestComPtr<ID3D12Resource> pal_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &pal_up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(pal_upload.Put()));
    if (FAILED(hr) || !pal_upload) {
        return Fail("CreateCommittedResource(PaletteUpload)", hr);
    }

    {
        std::vector<uint8_t> pal_rgba;
        BuildVibrantPalette(pal_rgba);
        void* mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        hr = pal_upload->Map(0, &read_none, &mapped);
        if (FAILED(hr) || !mapped) return Fail("PaletteUpload Map", hr);
        std::memcpy(mapped, pal_rgba.data(), pal_rgba.size());
        pal_upload->Unmap(0, nullptr);
    }

    // 7. UAV output texture (u0: 256x256 RGBA8) + Presentation RT texture (with Xbox flags)
    D3D12_RESOURCE_DESC uav_desc{};
    uav_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    uav_desc.Width = kWidth;
    uav_desc.Height = kHeight;
    uav_desc.DepthOrArraySize = 1;
    uav_desc.MipLevels = 1;
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.SampleDesc.Count = 1;
    uav_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    GuestComPtr<ID3D12Resource> uav_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(uav_tex.Put()));
    if (FAILED(hr) || !uav_tex) {
        return Fail("CreateCommittedResource(UavTex)", hr);
    }

    D3D12_RESOURCE_DESC rt_desc = uav_desc;
    rt_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);
    GuestComPtr<ID3D12Resource> rt_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rt_tex.Put()));
    if (FAILED(hr) || !rt_tex) {
        return Fail("CreateCommittedResource(RtTex)", hr);
    }

    // 8. Descriptor Heaps (CBV/SRV/UAV + Sampler)
    D3D12_DESCRIPTOR_HEAP_DESC srv_uav_heap_desc{};
    srv_uav_heap_desc.NumDescriptors = 2;
    srv_uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    GuestComPtr<ID3D12DescriptorHeap> srv_uav_heap;
    hr = device->CreateDescriptorHeap(&srv_uav_heap_desc, IID_PPV_ARGS(srv_uav_heap.Put()));
    if (FAILED(hr) || !srv_uav_heap) {
        return Fail("CreateDescriptorHeap(SRV/UAV)", hr);
    }

    const UINT srv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_uav_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{srv_cpu.ptr + srv_inc};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = srv_uav_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu{srv_gpu.ptr + srv_inc};

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(pal_tex.Get(), &srv_desc, srv_cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view_desc{};
    uav_view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_view_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(uav_tex.Get(), nullptr, &uav_view_desc, uav_cpu);

    D3D12_DESCRIPTOR_HEAP_DESC samp_heap_desc{};
    samp_heap_desc.NumDescriptors = 1;
    samp_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samp_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    GuestComPtr<ID3D12DescriptorHeap> samp_heap;
    hr = device->CreateDescriptorHeap(&samp_heap_desc, IID_PPV_ARGS(samp_heap.Put()));
    if (FAILED(hr) || !samp_heap) {
        return Fail("CreateDescriptorHeap(Sampler)", hr);
    }

    D3D12_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp_desc.MinLOD = 0.0f;
    samp_desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&samp_desc, samp_heap->GetCPUDescriptorHandleForHeapStart());

    // 9. Upload palette, dispatch Mandelbrot compute shader (32x32 groups -> 256x256), copy to RT, PresentX
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    D3D12_TEXTURE_COPY_LOCATION dst_pal{};
    dst_pal.pResource = pal_tex.Get();
    dst_pal.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_pal.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_pal{};
    src_pal.pResource = pal_upload.Get();
    src_pal.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_pal.PlacedFootprint = pal_fp;

    cmd->CopyTextureRegion(&dst_pal, 0, 0, 0, &src_pal, nullptr);

    D3D12_RESOURCE_BARRIER pal_to_srv{};
    pal_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pal_to_srv.Transition.pResource = pal_tex.Get();
    pal_to_srv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pal_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    pal_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &pal_to_srv);

    ID3D12DescriptorHeap* heaps[] = {srv_uav_heap.Get(), samp_heap.Get()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetComputeRootSignature(root_sig.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_buf->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, samp_heap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRootDescriptorTable(2, srv_gpu);
    cmd->SetComputeRootDescriptorTable(3, uav_gpu);
    cmd->SetPipelineState(compute_pso.Get());

    cmd->Dispatch(kWidth / 8u, kHeight / 8u, 1u);

    D3D12_RESOURCE_BARRIER uav_to_src{};
    uav_to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    uav_to_src.Transition.pResource = uav_tex.Get();
    uav_to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uav_to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uav_to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &uav_to_src);

    cmd->CopyResource(rt_tex.Get(), uav_tex.Get());

    D3D12_RESOURCE_BARRIER rt_to_present{};
    rt_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rt_to_present.Transition.pResource = rt_tex.Get();
    rt_to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rt_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    rt_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &rt_to_present);

    hr = cmd->Close();
    if (FAILED(hr)) {
        return Fail("CommandListX::Close", hr);
    }

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Resource* plane_res[] = {rt_tex.Get()};
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

    // 10. Read back Mandelbrot fractal frame & verify interior black set + colorful bands
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

    // Center (128,128) maps to c=(-0.5, 0.0), squarely inside the main Mandelbrot cardioid -> black
    const uint8_t* cardioid = pixel_at(128, 128);
    if (cardioid[0] != 0u || cardioid[1] != 0u || cardioid[2] != 0u || cardioid[3] != 255u) {
        return Fail("Mandelbrot cardioid interior pixel was not black");
    }

    // Far corner (0,0) maps to c=(-2.0, 1.125), escapes immediately -> non-black palette color
    const uint8_t* corner = pixel_at(0, 0);
    if (corner[0] == 0u && corner[1] == 0u && corner[2] == 0u) {
        return Fail("Mandelbrot exterior corner pixel was unexpectedly black");
    }

    uint32_t interior_black = 0;
    uint32_t colored_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(256);
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
        if (r == 0u && g == 0u && b == 0u) {
            ++interior_black;
        } else {
            ++colored_pixels;
        }
        unique_rgb.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b));
    }

    // With g_MaxThreadIter.z = 32, frac(iter / 32) hits up to 32 distinct color bands
    if (interior_black < 5000u || colored_pixels < 40000u) {
        return Fail("Unexpected Mandelbrot interior/exterior distribution", interior_black);
    }
    if (unique_rgb.size() < 20u) {
        return Fail("Too few distinct Mandelbrot fractal color bands",
                    static_cast<int64_t>(unique_rgb.size()));
    }

    if (!WritePpmW(L"T:\\mandelbrot.ppm", kWidth, kHeight, pixels.data())) {
        return Fail("Write T:\\mandelbrot.ppm", ::GetLastError());
    }

    char metrics_buf[512]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=SimpleCompute\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "width=%u\n"
        "height=%u\n"
        "fnv1a=0x%016llX\n"
        "non_bg_pixels=%u\n"
        "interior_black_pixels=%u\n"
        "unique_colors=%zu\n"
        "frame_ms=%.3f\n",
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        out_w,
        out_h,
        static_cast<unsigned long long>(frame_hash),
        colored_pixels,
        interior_black,
        unique_rgb.size(),
        frame_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_simple_compute] PASS | Adapter=%s (%s) | %ux%u | Colored=%u | Interior=%u | Bands=%zu | FNV1a=0x%016llX | %.2f ms\n",
        gpu_info.AdapterName,
        use_warp ? "WARP" : "HW",
        out_w,
        out_h,
        colored_pixels,
        interior_black,
        unique_rgb.size(),
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    std::fflush(stdout);

    samp_heap.Reset();
    srv_uav_heap.Reset();
    rt_tex.Reset();
    uav_tex.Reset();
    pal_upload.Reset();
    pal_tex.Reset();
    cb_buf.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    compute_pso.Reset();
    root_sig.Reset();
    device.Reset();
    XGameRuntimeUninitialize();
    return 0;
}
