// OpenScarlett MIT Wave 4 Standalone Xbox-ABI Guest Sample:
// VulkanMeshShowcase — Real RDNA2 XS2\0 Amplification (AS) + Mesh (MS) + Pixel (PS)
// + 32x32x32 3D Color-Grading LUT Volume Texture Showcase.
//
// Deliberately does NOT link openscarlett_core! Imports ONLY d3d12_xs.dll, xgameruntime.dll,
// and pixevt.dll (plus standard Windows system DLLs) and runs from a zero-DLL Xbox package
// folder via `openscarlett run <package_dir>` with MicrosoftGame.Config (TitleId = 0x5CA01005).
//
// Passes raw RDNA2 XS2\0 blobs directly to ID3D12DeviceX::CreatePipelineState (AS+MS+PS on
// D3D12 MeshTier=10 hardware GPUs) or ID3D12DeviceX::CreateGraphicsPipelineState (VS+PS on
// WARP / MeshTier=0 fallback) so d3d12_xs transparently auto-lifts XS2\0 -> DXIL!

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
#include "openscarlett/shims/secondary_shims_abi.h"

namespace {

constexpr uint32_t kExpectedTitleId = 0x5CA01005u;
constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kLutDim = 32u;
constexpr size_t kLutByteSize = static_cast<size_t>(kLutDim) * kLutDim * kLutDim * 4u;

constexpr float kClearColorF[4] = {18.0f / 255.0f, 23.0f / 255.0f, 36.0f / 255.0f, 1.0f};
constexpr uint8_t kBgR = 18u;
constexpr uint8_t kBgG = 23u;
constexpr uint8_t kBgB = 36u;
constexpr uint8_t kBgA = 255u;

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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // D3D12 pipeline state stream subobjects require pointer alignment
#endif
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeVal, typename InnerType>
struct alignas(void*) PsoStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = TypeVal;
    InnerType inner{};
};

struct ShowcaseMeshPsoStream {
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_sig;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE> as;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_vulkan_mesh_showcase] FAIL: %s (code=0x%llX)\n",
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

D3D12_RESOURCE_BARRIER MakeTransition(
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

std::string ReadEnvStringA(const char* name, const char* fallback) {
    char buf[128]{};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len > 0 && len < sizeof(buf)) {
        return std::string(buf, len);
    }
    return fallback;
}

bool HasXs2Magic(const std::vector<uint8_t>& blob) noexcept {
    return blob.size() >= 40u &&
           blob[0] == 'X' && blob[1] == 'S' && blob[2] == '2' && blob[3] == '\0';
}

}  // namespace

int main(int argc, char** argv) {
    bool force_warp = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            force_warp = true;
        }
    }

    const std::string backend_env = ReadEnvStringA("OPENSCARLETT_GPU_BACKEND", "d3d12");

    // 1. GDK Game Runtime initialization, Scarlett device identity & TitleId check
    PIXBeginEventOnCpu(0xFF33CCFFull, "VulkanMeshShowcase_Main");

    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }
    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        PIXEndEventOnCpu();
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    uint32_t title_id = 0;
    if (XGameGetXboxTitleId(&title_id) != XGR_S_OK || title_id != kExpectedTitleId) {
        PIXEndEventOnCpu();
        return Fail("XGameGetXboxTitleId mismatch (expected 0x5CA01005)", static_cast<int64_t>(title_id));
    }

    // 2. Load staged 32x32x32 3D LUT volume asset and RDNA2 XS2\0 shaders from virtual G:\ package
    std::vector<uint8_t> lut3d_bytes;
    if (!ReadWholeFileW(L"G:\\lut3d_32x32x32.bin", lut3d_bytes) || lut3d_bytes.size() != kLutByteSize) {
        PIXEndEventOnCpu();
        return Fail("Read G:\\lut3d_32x32x32.bin (expected 131072 bytes)", static_cast<int64_t>(lut3d_bytes.size()));
    }

    std::vector<uint8_t> ps_xs2;
    if (!ReadWholeFileW(L"G:\\ShowcasePS.xs2", ps_xs2) || !HasXs2Magic(ps_xs2)) {
        PIXEndEventOnCpu();
        return Fail("Read G:\\ShowcasePS.xs2", ::GetLastError());
    }

    // 3. Create Xbox D3D12X device and probe MeshShaderTier + HighestShaderModel
    OpenScarlettD3D12SetPreferWarp(force_warp ? 1 : 0);
    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        PIXEndEventOnCpu();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if (force_warp && gpu_info.IsWarp == 0u) {
        PIXEndEventOnCpu();
        return Fail("Expected WARP adapter when --warp specified");
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
        opt7.MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    }

    const bool use_hw_mesh =
        (!force_warp) &&
        (gpu_info.IsWarp == 0u) &&
        (sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5) &&
        (static_cast<uint32_t>(opt7.MeshShaderTier) >= 10u);

    hr = device->SetFrameIntervalX(nullptr, 16667u, 2u, 0u);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("SetFrameIntervalX", hr);
    }
    D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
    hr = device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token);
    if (FAILED(hr) || frame_token == 0) {
        PIXEndEventOnCpu();
        return Fail("WaitFrameEventX", hr);
    }

    // 4. Create Command Queue, Allocator, CommandListX, and Synchronization Fence
    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    GuestComPtr<ID3D12CommandQueueX> queue;
    hr = device->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    if (FAILED(hr) || !queue) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandQueue", hr);
    }

    GuestComPtr<ID3D12CommandAllocator> alloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    if (FAILED(hr) || !alloc) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandAllocator", hr);
    }

    GuestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    if (FAILED(hr) || !cmd) {
        PIXEndEventOnCpu();
        return Fail("CreateCommandList", hr);
    }

    GuestComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()));
    if (FAILED(hr) || !fence) {
        PIXEndEventOnCpu();
        return Fail("CreateFence", hr);
    }
    HANDLE fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        PIXEndEventOnCpu();
        return Fail("CreateEventW", ::GetLastError());
    }

    // 5. Create 256x256 Render Target + 32x32x32 3D LUT Volume Texture + Descriptor Heaps
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

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

    D3D12_CLEAR_VALUE rt_clear{};
    rt_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_clear.Color[0] = kClearColorF[0];
    rt_clear.Color[1] = kClearColorF[1];
    rt_clear.Color[2] = kClearColorF[2];
    rt_clear.Color[3] = kClearColorF[3];

    GuestComPtr<ID3D12Resource> rt_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, &rt_clear, IID_PPV_ARGS(rt_tex.Put()));
    if (FAILED(hr) || !rt_tex) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(RT)", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    GuestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    if (FAILED(hr) || !rtv_heap) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(RTV)", hr);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_handle);

    // 32x32x32 3D LUT volume texture (Texture3D<float4> _tex3d0 : register(t0))
    D3D12_RESOURCE_DESC lut_desc{};
    lut_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    lut_desc.Width = kLutDim;
    lut_desc.Height = kLutDim;
    lut_desc.DepthOrArraySize = kLutDim;
    lut_desc.MipLevels = 1;
    lut_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lut_desc.SampleDesc.Count = 1;
    lut_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    lut_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    GuestComPtr<ID3D12Resource> lut_tex;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &lut_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(lut_tex.Put()));
    if (FAILED(hr) || !lut_tex) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(Texture3D LUT)", hr);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lut_fp{};
    UINT lut_num_rows = 0;
    UINT64 lut_row_bytes = 0;
    UINT64 lut_total_bytes = 0;
    device->GetCopyableFootprints(&lut_desc, 0, 1, 0, &lut_fp, &lut_num_rows, &lut_row_bytes, &lut_total_bytes);

    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = lut_total_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    GuestComPtr<ID3D12Resource> lut_upload;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(lut_upload.Put()));
    if (FAILED(hr) || !lut_upload) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(LUT Upload)", hr);
    }

    {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        hr = lut_upload->Map(0, &none, &mapped);
        if (FAILED(hr) || !mapped) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("LUT Upload Map", hr);
        }
        auto* dst = static_cast<uint8_t*>(mapped);
        std::memset(dst, 0, static_cast<size_t>(lut_total_bytes));
        const size_t slice_pitch = static_cast<size_t>(lut_num_rows) * static_cast<size_t>(lut_fp.Footprint.RowPitch);
        const size_t src_row_bytes = static_cast<size_t>(kLutDim) * 4u;
        for (uint32_t z = 0; z < kLutDim; ++z) {
            for (uint32_t y = 0; y < kLutDim; ++y) {
                const size_t dst_off =
                    static_cast<size_t>(lut_fp.Offset) +
                    static_cast<size_t>(z) * slice_pitch +
                    static_cast<size_t>(y) * static_cast<size_t>(lut_fp.Footprint.RowPitch);
                const size_t src_off = (static_cast<size_t>(z) * kLutDim + y) * src_row_bytes;
                std::memcpy(dst + dst_off, lut3d_bytes.data() + src_off, src_row_bytes);
            }
        }
        lut_upload->Unmap(0, nullptr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.NumDescriptors = 1;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    GuestComPtr<ID3D12DescriptorHeap> srv_heap;
    hr = device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(srv_heap.Put()));
    if (FAILED(hr) || !srv_heap) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateDescriptorHeap(SRV)", hr);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture3D.MostDetailedMip = 0;
    srv_desc.Texture3D.MipLevels = 1;
    srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(lut_tex.Get(), &srv_desc, srv_heap->GetCPUDescriptorHandleForHeapStart());

    // 6. Create Root Signature: Descriptor Table with 1 Texture3D SRV at register(t0) for PS
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.DescriptorTable.NumDescriptorRanges = 1;
    root_param.DescriptorTable.pDescriptorRanges = &srv_range;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &root_param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    GuestComPtr<ID3DBlob> rs_blob;
    GuestComPtr<ID3DBlob> rs_err;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put());
    if (FAILED(hr) || !rs_blob) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("D3D12SerializeRootSignature", hr);
    }

    GuestComPtr<ID3D12RootSignature> root_sig;
    hr = device->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()));
    if (FAILED(hr) || !root_sig) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CreateRootSignature", hr);
    }

    // 7. Create PSO directly from RAW RDNA2 XS2\0 blobs (d3d12_xs auto-lifts XS2\0 -> DXIL!)
    D3D12_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rast_desc{};
    rast_desc.FillMode = D3D12_FILL_MODE_SOLID;
    rast_desc.CullMode = D3D12_CULL_MODE_NONE;
    rast_desc.FrontCounterClockwise = FALSE;
    rast_desc.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC ds_desc{};
    ds_desc.DepthEnable = FALSE;
    ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds_desc.StencilEnable = FALSE;

    GuestComPtr<ID3D12PipelineState> pso;
    if (use_hw_mesh) {
        std::vector<uint8_t> as_xs2;
        std::vector<uint8_t> ms_xs2;
        if (!ReadWholeFileW(L"G:\\ShowcaseAS.xs2", as_xs2) || !HasXs2Magic(as_xs2)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\ShowcaseAS.xs2", ::GetLastError());
        }
        if (!ReadWholeFileW(L"G:\\ShowcaseMS.xs2", ms_xs2) || !HasXs2Magic(ms_xs2)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\ShowcaseMS.xs2", ::GetLastError());
        }

        ShowcaseMeshPsoStream stream{};
        stream.root_sig.inner = root_sig.Get();
        stream.as.inner = {as_xs2.data(), as_xs2.size()};
        stream.ms.inner = {ms_xs2.data(), ms_xs2.size()};
        stream.ps.inner = {ps_xs2.data(), ps_xs2.size()};
        stream.blend.inner = blend_desc;
        stream.sample_mask.inner = UINT_MAX;
        stream.rasterizer.inner = rast_desc;
        stream.depth_stencil.inner = ds_desc;
        stream.rt_formats.inner.NumRenderTargets = 1;
        stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample_desc.inner = {1, 0};

        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{};
        stream_desc.SizeInBytes = sizeof(stream);
        stream_desc.pPipelineStateSubobjectStream = &stream;

        hr = device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(pso.Put()));
        if (FAILED(hr) || !pso) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("ID3D12DeviceX::CreatePipelineState(XS2 AS+MS+PS)", hr);
        }
    } else {
        std::vector<uint8_t> vs_xs2;
        if (!ReadWholeFileW(L"G:\\ShowcaseFallbackVS.xs2", vs_xs2) || !HasXs2Magic(vs_xs2)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Read G:\\ShowcaseFallbackVS.xs2", ::GetLastError());
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx_desc{};
        gfx_desc.pRootSignature = root_sig.Get();
        gfx_desc.VS = {vs_xs2.data(), vs_xs2.size()};
        gfx_desc.PS = {ps_xs2.data(), ps_xs2.size()};
        gfx_desc.BlendState = blend_desc;
        gfx_desc.SampleMask = UINT_MAX;
        gfx_desc.RasterizerState = rast_desc;
        gfx_desc.DepthStencilState = ds_desc;
        gfx_desc.InputLayout = {nullptr, 0};
        gfx_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gfx_desc.NumRenderTargets = 1;
        gfx_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gfx_desc.SampleDesc = {1, 0};

        hr = device->CreateGraphicsPipelineState(&gfx_desc, IID_PPV_ARGS(pso.Put()));
        if (FAILED(hr) || !pso) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("ID3D12DeviceX::CreateGraphicsPipelineState(XS2 VS+PS)", hr);
        }
    }

    // 8. Record Command List: Upload 3D LUT, Render 256x256 Prism + 3D LUT Grade, PresentX
    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    PIXBeginEventOnCommandList(
        cmd.Get(),
        0xFF44DDAAull,
        use_hw_mesh ? "VulkanMeshShowcase_HW_XS2_AS_MS_PS" : "VulkanMeshShowcase_WARP_XS2_VS_PS");

    D3D12_TEXTURE_COPY_LOCATION copy_dst{};
    copy_dst.pResource = lut_tex.Get();
    copy_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION copy_src{};
    copy_src.pResource = lut_upload.Get();
    copy_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_src.PlacedFootprint = lut_fp;
    cmd->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);

    D3D12_RESOURCE_BARRIER pre_barriers[2] = {
        MakeTransition(lut_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        MakeTransition(rt_tex.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(2, pre_barriers);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv_handle, rt_clear.Color, 0, nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(root_sig.Get());
    cmd->SetPipelineState(pso.Get());
    cmd->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());

    if (use_hw_mesh) {
        PIXSetMarkerOnCommandList(cmd.Get(), 0xFFFFAA33ull, "DispatchMesh_XS2_Prism3DLut");
        cmd->DispatchMesh(1, 1, 1);
    } else {
        PIXSetMarkerOnCommandList(cmd.Get(), 0xFFFFAA33ull, "DrawInstanced_XS2_FallbackPrism3DLut");
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(12, 1, 0, 0);
    }

    D3D12_RESOURCE_BARRIER post_rt =
        MakeTransition(rt_tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &post_rt);
    PIXEndEventOnCommandList(cmd.Get());

    hr = cmd->Close();
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CommandListX::Close", hr);
    }

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    PIXBeginEventOnCommandQueue(queue.Get(), 0xFFFF6644ull, "VulkanMeshShowcase_PresentX");
    ID3D12Resource* plane_res[] = {rt_tex.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = frame_token;
    plane.ResourceCount = 1;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres_params{};
    pres_params.SyncInterval = 1;

    hr = queue->PresentX(1, &plane, &pres_params);
    PIXEndEventOnCommandQueue(queue.Get());
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("PresentX(1)", hr);
    }

    hr = queue->Signal(fence.Get(), 1u);
    if (FAILED(hr)) {
        ::CloseHandle(fence_event);
        PIXEndEventOnCpu();
        return Fail("CommandQueueX::Signal", hr);
    }
    if (fence->GetCompletedValue() < 1u) {
        hr = fence->SetEventOnCompletion(1u, fence_event);
        if (FAILED(hr)) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("Fence::SetEventOnCompletion", hr);
        }
        const DWORD wait_res = ::WaitForSingleObject(fence_event, 15000u);
        if (wait_res != WAIT_OBJECT_0) {
            ::CloseHandle(fence_event);
            PIXEndEventOnCpu();
            return Fail("GPU fence wait timed out", wait_res);
        }
        PIXNotifyWakeFromFenceSignal(fence_event);
    }
    ::CloseHandle(fence_event);

    ::QueryPerformanceCounter(&qpc_end);
    const double frame_ms = QpcMs(qpc_start, qpc_end, qpc_freq);

    // 9. Read back 256x256 rendered frame and verify background, geometry coverage & 3D LUT grading
    uint64_t frame_hash = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    hr = OpenScarlettD3D12GetLastPresentHash(queue.Get(), &frame_hash, &out_w, &out_h);
    if (FAILED(hr) || out_w != kWidth || out_h != kHeight || frame_hash == 0ull) {
        PIXEndEventOnCpu();
        return Fail("OpenScarlettD3D12GetLastPresentHash", hr);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied);
    if (FAILED(hr) || copied != pixels.size()) {
        PIXEndEventOnCpu();
        return Fail("OpenScarlettD3D12CopyLastPresentPixels", hr);
    }

    auto pixel_at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        return &pixels[(static_cast<size_t>(y) * kWidth + x) * 4u];
    };

    const uint8_t* bg = pixel_at(4, 4);
    if (bg[0] != kBgR || bg[1] != kBgG || bg[2] != kBgB || bg[3] != kBgA) {
        PIXEndEventOnCpu();
        return Fail("Background clear pixel mismatch at (4,4)");
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

    // 4-triangle diamond covering [-0.78, +0.78] -> ~19,936 non-background pixels
    if (non_bg_pixels < 18500u || non_bg_pixels > 21500u) {
        PIXEndEventOnCpu();
        return Fail("Unexpected prism pixel coverage", non_bg_pixels);
    }
    if (unique_rgb.size() < 200u) {
        PIXEndEventOnCpu();
        return Fail("Too few 3D LUT graded colors", static_cast<int64_t>(unique_rgb.size()));
    }

    const uint8_t* p_center = pixel_at(128, 128);
    const uint8_t* p_top = pixel_at(128, 48);
    const uint8_t* p_right = pixel_at(208, 128);
    const uint8_t* p_bottom = pixel_at(128, 208);
    const uint8_t* p_left = pixel_at(48, 128);

    PIXReportCounter(L"VulkanMeshShowcase.NonBgPixels", static_cast<float>(non_bg_pixels));
    PIXReportCounter(L"VulkanMeshShowcase.UniqueColors", static_cast<float>(unique_rgb.size()));

    // 10. Export T:\vulkan_mesh_showcase.ppm and T:\metrics.txt
    if (!WritePpmW(L"T:\\vulkan_mesh_showcase.ppm", kWidth, kHeight, pixels.data())) {
        PIXEndEventOnCpu();
        return Fail("Write T:\\vulkan_mesh_showcase.ppm", ::GetLastError());
    }

    const char* mode_str = use_hw_mesh ? "hw_xs2_as_ms" : "warp_xs2_vs_ps";
    char metrics_buf[1536]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=VulkanMeshShowcase\n"
        "title_id=0x%08X\n"
        "mode=%s\n"
        "backend_env=%s\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "mesh_tier=%u\n"
        "shader_model=0x%02X\n"
        "width=%u\n"
        "height=%u\n"
        "lut_dim=%u\n"
        "non_bg_pixels=%u\n"
        "unique_colors=%zu\n"
        "center_rgb=%u,%u,%u\n"
        "top_rgb=%u,%u,%u\n"
        "right_rgb=%u,%u,%u\n"
        "bottom_rgb=%u,%u,%u\n"
        "left_rgb=%u,%u,%u\n"
        "fnv1a=0x%016llX\n"
        "frame_ms=%.3f\n",
        title_id,
        mode_str,
        backend_env.c_str(),
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        static_cast<unsigned>(opt7.MeshShaderTier),
        static_cast<unsigned>(sm.HighestShaderModel),
        out_w,
        out_h,
        kLutDim,
        non_bg_pixels,
        unique_rgb.size(),
        p_center[0], p_center[1], p_center[2],
        p_top[0], p_top[1], p_top[2],
        p_right[0], p_right[1], p_right[2],
        p_bottom[0], p_bottom[1], p_bottom[2],
        p_left[0], p_left[1], p_left[2],
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        PIXEndEventOnCpu();
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_vulkan_mesh_showcase] PASS | Mode=%s | BackendEnv=%s | Adapter=%s (MeshTier=%u, SM=0x%02X) | "
        "NonBG=%u | Colors=%zu | Center=(%u,%u,%u) | FNV1a=0x%016llX | %.2f ms\n",
        mode_str,
        backend_env.c_str(),
        gpu_info.AdapterName,
        static_cast<unsigned>(opt7.MeshShaderTier),
        static_cast<unsigned>(sm.HighestShaderModel),
        non_bg_pixels,
        unique_rgb.size(),
        p_center[0], p_center[1], p_center[2],
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    std::fflush(stdout);

    PIXEndEventOnCpu();

    pso.Reset();
    root_sig.Reset();
    rs_err.Reset();
    rs_blob.Reset();
    srv_heap.Reset();
    lut_upload.Reset();
    lut_tex.Reset();
    rtv_heap.Reset();
    rt_tex.Reset();
    fence.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device.Reset();
    XGameRuntimeUninitialize();
    return 0;
}
