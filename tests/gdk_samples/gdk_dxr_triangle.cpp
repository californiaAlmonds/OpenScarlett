// OpenScarlett MIT Standalone Xbox-ABI Guest Sample — DXRTriangle (Wave 2)
//
// End-to-end ID3D12Device5 BLAS/TLAS/SBT + DispatchRays workload running as a pure
// standalone Xbox-ABI executable launched via `openscarlett run <package_dir>` from
// a zero-DLL package folder with MicrosoftGame.Config (TitleId = 0x5CA01003).
//
//   Path A (RaytracingTier >= 10 && !WARP && !--force-sw):
//     Native hardware RT-core DXR pipeline on ID3D12Device5 + ID3D12GraphicsCommandList4:
//       - Loads G:\DxrTriangleLib.cso (DXIL lib_6_3: RaygenShader, ClosestHitShader, MissShader)
//       - PrebuildInfo -> 256B-aligned BLAS + TLAS GPU build -> CreateStateObject -> 64B SBT
//       - SetPipelineState1 -> DispatchRays(256, 256, 1) -> CopyResource -> PresentX
//
//   Path B (RaytracingTier == 0 on GTX 1650 Ti / WARP, or --force-sw):
//     Bit-exact SAH Software BVH fallback via XboxBvhBuilder.dll + live d3d12_xs PresentX:
//       - Builds SAH BVH BLAS + TLAS for the exact same two-triangle scene
//       - Constructs and uploads a real 64B-aligned GPU SBT buffer on d3d12_xs
//       - Traces all 256x256 primary rays through XboxBvhTraceRay with identical shading math
//       - Uploads RGBA8 framebuffer via CopyTextureRegion -> PresentX
//
// Deliberately does NOT link openscarlett_core!

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

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 256u;
constexpr uint32_t kExpectedTitleId = 0x5CA01003u;
constexpr uint32_t kExpectedHitPixels = 10112u;  // 8192 (Tri0) + 2048 (Tri1) - 128 (overlap)

constexpr uint8_t kBgR = 0x14u;
constexpr uint8_t kBgG = 0x18u;
constexpr uint8_t kBgB = 0x24u;
constexpr uint8_t kBgA = 0xFFu;

constexpr uint64_t kPixColorBlue = 0xFF3388FFull;
constexpr uint64_t kPixColorGreen = 0xFF33CC66ull;
constexpr uint64_t kPixColorGold = 0xFFFFCC33ull;

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

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

struct Float3 {
    float x;
    float y;
    float z;
};

// Shared Canonical DXR Triangle Scene (2 triangles, dyadic coordinates with zero pixel-center edge ties):
//   Tri 0 (z = 0.0, front): canonical barycentric RGB triangle
//     v0 = ( 0.0,  0.5, 0.0) -> Red   (1, 0, 0)
//     v1 = ( 0.5, -0.5, 0.0) -> Green (0, 1, 0)
//     v2 = (-0.5, -0.5, 0.0) -> Blue  (0, 0, 1)
//   Tri 1 (z = 0.5, behind & offset upper-right): verifies multi-primitive BVH traversal & depth sorting
//     s0 = (0.375, 0.5, 0.5) -> Yellow  (1.0, 0.90, 0.20)
//     s1 = (0.625, 0.0, 0.5) -> Cyan    (0.1, 0.85, 1.00)
//     s2 = (0.125, 0.0, 0.5) -> Magenta (0.9, 0.20, 0.95)
constexpr Float3 kSceneVertices[6] = {
    // Primitive 0 (front RGB triangle at z = 0.0)
    {0.0f, 0.5f, 0.0f},
    {0.5f, -0.5f, 0.0f},
    {-0.5f, -0.5f, 0.0f},
    // Primitive 1 (behind/offset secondary triangle at z = 0.5)
    {0.375f, 0.5f, 0.5f},
    {0.625f, 0.0f, 0.5f},
    {0.125f, 0.0f, 0.5f},
};

int Fail(const char* step, int64_t code = 0) {
    std::fprintf(stderr, "[gdk_dxr_triangle] FAIL: %s (code=0x%llX)\n",
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

inline uint8_t FloatToUnorm8(float v) noexcept {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::floor(clamped * 255.0f + 0.5f));
}

void FillSyntheticShaderId(const char* export_name, uint8_t out_id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES]) {
    for (uint32_t lane = 0; lane < 4u; ++lane) {
        uint64_t h = 14695981039346656037ull ^ (static_cast<uint64_t>(lane) * 0x9E3779B97F4A7C15ull);
        for (const char* p = export_name; *p != '\0'; ++p) {
            h ^= static_cast<uint8_t>(*p);
            h *= 1099511628211ull;
        }
        std::memcpy(out_id + static_cast<size_t>(lane) * 8u, &h, 8u);
    }
}

HRESULT CreateBuffer(
    ID3D12Device* dev,
    D3D12_HEAP_TYPE heap_type,
    uint64_t byte_size,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    GuestComPtr<ID3D12Resource>& out_res) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = byte_size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    return dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr, IID_PPV_ARGS(out_res.Put()));
}

HRESULT UploadToBuffer(ID3D12Resource* upload_buf, const void* data, size_t size_bytes) {
    void* mapped = nullptr;
    D3D12_RANGE read_none{0, 0};
    HRESULT hr = upload_buf->Map(0, &read_none, &mapped);
    if (FAILED(hr) || !mapped) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    std::memcpy(mapped, data, size_bytes);
    upload_buf->Unmap(0, nullptr);
    return S_OK;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_warp = false;
    bool force_sw = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warp") == 0) {
            use_warp = true;
        } else if (std::strcmp(argv[i], "--force-sw") == 0) {
            force_sw = true;
        }
    }

    PIXBeginEventOnCpu(kPixColorBlue, "DXRTriangle::Init");

    // 1. GDK Game Runtime initialization + TitleId (0x5CA01003) + Scarlett device check
    if (XGameRuntimeInitialize() != XGR_S_OK) {
        PIXEndEventOnCpu();
        return Fail("XGameRuntimeInitialize");
    }

    uint32_t title_id = 0;
    const XGameResult tid_hr = XGameGetXboxTitleId(&title_id);
    if (tid_hr != XGR_S_OK || title_id != kExpectedTitleId) {
        PIXEndEventOnCpu();
        return Fail("XGameGetXboxTitleId expected 0x5CA01003", title_id);
    }

    const XSystemDeviceType dev_type = XSystemGetDeviceType();
    if (dev_type != XSystemDeviceType_XboxScarlettAnaconda &&
        dev_type != XSystemDeviceType_XboxScarlettLockhart &&
        dev_type != XSystemDeviceType_XboxScarlettDevkit) {
        PIXEndEventOnCpu();
        return Fail("XSystemGetDeviceType not Scarlett", static_cast<int64_t>(dev_type));
    }

    // 2. Always verify G:\DxrTriangleLib.cso (DXIL lib_6_3) is readable from virtual G:\ package!
    std::vector<uint8_t> dxr_lib_blob;
    if (!ReadWholeFileW(L"G:\\DxrTriangleLib.cso", dxr_lib_blob) ||
        dxr_lib_blob.size() < 64u ||
        dxr_lib_blob[0] != 'D' || dxr_lib_blob[1] != 'X' ||
        dxr_lib_blob[2] != 'B' || dxr_lib_blob[3] != 'C') {
        PIXEndEventOnCpu();
        return Fail("Read/Validate G:\\DxrTriangleLib.cso", ::GetLastError());
    }

    // 3. Create Xbox Scarlett ID3D12DeviceX and QI ID3D12Device5
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;
    dev_params.DisableGeometryShaderAllocations = 1;
    dev_params.DisableTessellationShaderAllocations = 1;

    GuestComPtr<ID3D12DeviceX> device;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(device.Put()));
    if (FAILED(hr) || !device) {
        PIXEndEventOnCpu();
        return Fail("D3D12XboxCreateDevice", hr);
    }

    GuestComPtr<ID3D12Device5> device5;
    hr = device->QueryInterface(IID_PPV_ARGS(device5.Put()));
    if (FAILED(hr) || !device5) {
        PIXEndEventOnCpu();
        return Fail("QueryInterface(ID3D12Device5)", hr);
    }

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    hr = device->GetGpuHardwareInfoX(&gpu_info);
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("GetGpuHardwareInfoX", hr);
    }
    if (use_warp && gpu_info.IsWarp == 0u) {
        PIXEndEventOnCpu();
        return Fail("Requested WARP adapter but got hardware adapter", gpu_info.IsWarp);
    }

    // 4. Probe D3D12_FEATURE_D3D12_OPTIONS5::RaytracingTier
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
    if (FAILED(hr)) {
        PIXEndEventOnCpu();
        return Fail("CheckFeatureSupport(D3D12_OPTIONS5)", hr);
    }
    const uint32_t dxr_tier = static_cast<uint32_t>(opt5.RaytracingTier);
    const bool use_hw_dxr =
        (!force_sw && gpu_info.IsWarp == 0u &&
         dxr_tier >= static_cast<uint32_t>(D3D12_RAYTRACING_TIER_1_0));

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

    // 5. Create ID3D12CommandQueueX, CommandAllocator, ID3D12GraphicsCommandListX + QI ID3D12GraphicsCommandList4
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

    GuestComPtr<ID3D12GraphicsCommandList4> cmd4;
    hr = cmd->QueryInterface(IID_PPV_ARGS(cmd4.Put()));
    if (FAILED(hr) || !cmd4) {
        PIXEndEventOnCpu();
        return Fail("QueryInterface(ID3D12GraphicsCommandList4)", hr);
    }

    // 6. Create Xbox scanout presentation texture (ALLOW_DISPLAY + ALLOW_DCC sanitized by d3d12_xs)
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC present_desc{};
    present_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    present_desc.Width = kWidth;
    present_desc.Height = kHeight;
    present_desc.DepthOrArraySize = 1;
    present_desc.MipLevels = 1;
    present_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    present_desc.SampleDesc.Count = 1;
    present_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    present_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        D3D12XBOX_RESOURCE_FLAG_ALLOW_DCC);

    GuestComPtr<ID3D12Resource> present_tex;
    hr = device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_ALLOW_DISPLAY,
        &present_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(present_tex.Put()));
    if (FAILED(hr) || !present_tex) {
        PIXEndEventOnCpu();
        return Fail("CreateCommittedResource(present_tex)", hr);
    }

    PIXEndEventOnCpu();  // DXRTriangle::Init

    // Resources held alive until after GPU PresentX fence wait completes
    GuestComPtr<ID3D12Resource> vb;
    GuestComPtr<ID3D12Resource> blas_scratch;
    GuestComPtr<ID3D12Resource> blas_result;
    GuestComPtr<ID3D12Resource> tlas_scratch;
    GuestComPtr<ID3D12Resource> tlas_result;
    GuestComPtr<ID3D12Resource> instance_upload;
    GuestComPtr<ID3D12Resource> sbt_buf;
    GuestComPtr<ID3D12Resource> uav_tex;
    GuestComPtr<ID3D12Resource> sw_upload_buf;
    GuestComPtr<ID3D12DescriptorHeap> uav_heap;
    GuestComPtr<ID3D12RootSignature> global_rs;
    GuestComPtr<ID3D12StateObject> rtpso;
    GuestComPtr<ID3D12StateObjectProperties> rtpso_props;

    LARGE_INTEGER qpc_freq{};
    LARGE_INTEGER qpc_start{};
    LARGE_INTEGER qpc_end{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&qpc_start);

    if (use_hw_dxr) {
        // =====================================================================
        // PATH A: Native Hardware RT-Core DXR (ID3D12Device5 + CommandList4)
        // =====================================================================
        PIXBeginEventOnCpu(kPixColorGreen, "DXRTriangle::PathA_HardwareDXR");

        // A1. Global Root Signature:
        //   Param 0: DescriptorTable(UAV u0, space0) -> g_Output
        //   Param 1: Root SRV (t0, space0)           -> g_Scene (TLAS)
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1;
        uav_range.BaseShaderRegister = 0;
        uav_range.RegisterSpace = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2]{};
        root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[0].DescriptorTable.NumDescriptorRanges = 1;
        root_params[0].DescriptorTable.pDescriptorRanges = &uav_range;
        root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        root_params[1].Descriptor.ShaderRegister = 0;
        root_params[1].Descriptor.RegisterSpace = 0;
        root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 2;
        rs_desc.pParameters = root_params;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        GuestComPtr<ID3DBlob> rs_blob;
        GuestComPtr<ID3DBlob> rs_err;
        hr = D3D12SerializeRootSignature(
            &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put());
        if (FAILED(hr) || !rs_blob) {
            PIXEndEventOnCpu();
            return Fail("D3D12SerializeRootSignature(DXR GlobalRS)", hr);
        }

        hr = device->CreateRootSignature(
            0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
            IID_PPV_ARGS(global_rs.Put()));
        if (FAILED(hr) || !global_rs) {
            PIXEndEventOnCpu();
            return Fail("CreateRootSignature(DXR GlobalRS)", hr);
        }

        // A2. Upload triangle vertices (2 triangles = 6 float3 vertices)
        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256ull,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, vb);
        if (FAILED(hr) || !vb) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(VB)", hr);
        }
        hr = UploadToBuffer(vb.Get(), kSceneVertices, sizeof(kSceneVertices));
        if (FAILED(hr)) {
            PIXEndEventOnCpu();
            return Fail("UploadToBuffer(VB)", hr);
        }

        // A3. BLAS PrebuildInfo (slot 63) + 256B-aligned scratch & result buffers
        D3D12_RAYTRACING_GEOMETRY_DESC geom_desc{};
        geom_desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom_desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom_desc.Triangles.Transform3x4 = 0;
        geom_desc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        geom_desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom_desc.Triangles.IndexCount = 0;
        geom_desc.Triangles.VertexCount = 6;
        geom_desc.Triangles.IndexBuffer = 0;
        geom_desc.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        geom_desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Float3);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
        blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blas_inputs.NumDescs = 1;
        blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs.pGeometryDescs = &geom_desc;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_prebuild{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_prebuild);
        if (blas_prebuild.ResultDataMaxSizeInBytes == 0ull) {
            PIXEndEventOnCpu();
            return Fail("GetRaytracingAccelerationStructurePrebuildInfo(BLAS) returned 0");
        }

        const uint64_t blas_result_bytes =
            AlignUp(blas_prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const uint64_t blas_scratch_bytes =
            AlignUp(std::max<uint64_t>(blas_prebuild.ScratchDataSizeInBytes, 256ull),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, blas_scratch_bytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blas_scratch);
        if (FAILED(hr) || !blas_scratch) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(blas_scratch)", hr);
        }
        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, blas_result_bytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, blas_result);
        if (FAILED(hr) || !blas_result) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(blas_result)", hr);
        }

        // A4. TLAS PrebuildInfo (slot 63) + D3D12_RAYTRACING_INSTANCE_DESC upload + buffers
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
        tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlas_inputs.NumDescs = 1;
        tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_prebuild{};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_prebuild);
        if (tlas_prebuild.ResultDataMaxSizeInBytes == 0ull) {
            PIXEndEventOnCpu();
            return Fail("GetRaytracingAccelerationStructurePrebuildInfo(TLAS) returned 0");
        }

        const uint64_t tlas_result_bytes =
            AlignUp(tlas_prebuild.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const uint64_t tlas_scratch_bytes =
            AlignUp(std::max<uint64_t>(tlas_prebuild.ScratchDataSizeInBytes, 256ull),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);

        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, tlas_scratch_bytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tlas_scratch);
        if (FAILED(hr) || !tlas_scratch) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(tlas_scratch)", hr);
        }
        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, tlas_result_bytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlas_result);
        if (FAILED(hr) || !tlas_result) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(tlas_result)", hr);
        }

        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256ull,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, instance_upload);
        if (FAILED(hr) || !instance_upload) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(instance_upload)", hr);
        }

        D3D12_RAYTRACING_INSTANCE_DESC inst_desc{};
        inst_desc.Transform[0][0] = 1.0f;
        inst_desc.Transform[1][1] = 1.0f;
        inst_desc.Transform[2][2] = 1.0f;
        inst_desc.InstanceID = 0;
        inst_desc.InstanceMask = 0xFFu;
        inst_desc.InstanceContributionToHitGroupIndex = 0;
        inst_desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        inst_desc.AccelerationStructure = blas_result->GetGPUVirtualAddress();

        hr = UploadToBuffer(instance_upload.Get(), &inst_desc, sizeof(inst_desc));
        if (FAILED(hr)) {
            PIXEndEventOnCpu();
            return Fail("UploadToBuffer(instance_desc)", hr);
        }
        tlas_inputs.InstanceDescs = instance_upload->GetGPUVirtualAddress();

        // A5. CreateStateObject (slot 62): DXIL_LIBRARY + HIT_GROUP + SHADER_CONFIG + GLOBAL_RS + PIPELINE_CONFIG
        D3D12_EXPORT_DESC exports[3]{};
        exports[0] = {L"RaygenShader", nullptr, D3D12_EXPORT_FLAG_NONE};
        exports[1] = {L"ClosestHitShader", nullptr, D3D12_EXPORT_FLAG_NONE};
        exports[2] = {L"MissShader", nullptr, D3D12_EXPORT_FLAG_NONE};

        D3D12_DXIL_LIBRARY_DESC dxil_lib{};
        dxil_lib.DXILLibrary = {dxr_lib_blob.data(), dxr_lib_blob.size()};
        dxil_lib.NumExports = 3;
        dxil_lib.pExports = exports;

        D3D12_HIT_GROUP_DESC hit_group{};
        hit_group.HitGroupExport = L"TriangleHitGroup";
        hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hit_group.AnyHitShaderImport = nullptr;
        hit_group.ClosestHitShaderImport = L"ClosestHitShader";
        hit_group.IntersectionShaderImport = nullptr;

        D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
        shader_cfg.MaxPayloadSizeInBytes = sizeof(float) * 4u;
        shader_cfg.MaxAttributeSizeInBytes = sizeof(float) * 2u;

        D3D12_GLOBAL_ROOT_SIGNATURE global_rs_sub{};
        global_rs_sub.pGlobalRootSignature = global_rs.Get();

        D3D12_RAYTRACING_PIPELINE_CONFIG pipe_cfg{};
        pipe_cfg.MaxTraceRecursionDepth = 1;

        D3D12_STATE_SUBOBJECT subobjects[5]{};
        subobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil_lib};
        subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_group};
        subobjects[2] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_cfg};
        subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_rs_sub};
        subobjects[4] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipe_cfg};

        D3D12_STATE_OBJECT_DESC rtpso_desc{};
        rtpso_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        rtpso_desc.NumSubobjects = 5;
        rtpso_desc.pSubobjects = subobjects;

        hr = device5->CreateStateObject(&rtpso_desc, IID_PPV_ARGS(rtpso.Put()));
        if (FAILED(hr) || !rtpso) {
            PIXEndEventOnCpu();
            return Fail("ID3D12Device5::CreateStateObject", hr);
        }

        hr = rtpso->QueryInterface(IID_PPV_ARGS(rtpso_props.Put()));
        if (FAILED(hr) || !rtpso_props) {
            PIXEndEventOnCpu();
            return Fail("QueryInterface(ID3D12StateObjectProperties)", hr);
        }

        void* raygen_id = rtpso_props->GetShaderIdentifier(L"RaygenShader");
        void* miss_id = rtpso_props->GetShaderIdentifier(L"MissShader");
        void* hitgroup_id = rtpso_props->GetShaderIdentifier(L"TriangleHitGroup");
        if (!raygen_id || !miss_id || !hitgroup_id) {
            PIXEndEventOnCpu();
            return Fail("GetShaderIdentifier returned null");
        }

        // A6. Upload real 64B-aligned GPU Shader Binding Table (SBT)
        constexpr uint64_t kSbtStride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64B
        constexpr uint64_t kSbtBytes = kSbtStride * 4ull;                              // 256B
        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kSbtBytes,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, sbt_buf);
        if (FAILED(hr) || !sbt_buf) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(SBT)", hr);
        }

        uint8_t sbt_cpu[kSbtBytes]{};
        std::memcpy(sbt_cpu + 0u * kSbtStride, raygen_id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(sbt_cpu + 1u * kSbtStride, miss_id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(sbt_cpu + 2u * kSbtStride, hitgroup_id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

        hr = UploadToBuffer(sbt_buf.Get(), sbt_cpu, sizeof(sbt_cpu));
        if (FAILED(hr)) {
            PIXEndEventOnCpu();
            return Fail("UploadToBuffer(SBT)", hr);
        }

        const D3D12_GPU_VIRTUAL_ADDRESS sbt_va = sbt_buf->GetGPUVirtualAddress();
        if ((sbt_va % D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT) != 0ull) {
            PIXEndEventOnCpu();
            return Fail("SBT GPU VA is not 64B-aligned");
        }

        // A7. Create 256x256 R8G8B8A8_UNORM UAV output texture + shader-visible descriptor heap
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

        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(uav_tex.Put()));
        if (FAILED(hr) || !uav_tex) {
            PIXEndEventOnCpu();
            return Fail("CreateCommittedResource(uav_tex)", hr);
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 1;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(uav_heap.Put()));
        if (FAILED(hr) || !uav_heap) {
            PIXEndEventOnCpu();
            return Fail("CreateDescriptorHeap(UAV)", hr);
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view{};
        uav_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(
            uav_tex.Get(), nullptr, &uav_view, uav_heap->GetCPUDescriptorHandleForHeapStart());

        // A8. Record BLAS -> UAV barrier -> TLAS -> UAV barrier -> SetPipelineState1 -> DispatchRays -> CopyResource
        PIXBeginEventOnCommandList(cmd4.Get(), kPixColorGreen, "BuildAS_And_DispatchRays");

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
        blas_build.DestAccelerationStructureData = blas_result->GetGPUVirtualAddress();
        blas_build.Inputs = blas_inputs;
        blas_build.SourceAccelerationStructureData = 0;
        blas_build.ScratchAccelerationStructureData = blas_scratch->GetGPUVirtualAddress();
        cmd4->BuildRaytracingAccelerationStructure(&blas_build, 0, nullptr);

        D3D12_RESOURCE_BARRIER blas_uav{};
        blas_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blas_uav.UAV.pResource = blas_result.Get();
        cmd4->ResourceBarrier(1, &blas_uav);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
        tlas_build.DestAccelerationStructureData = tlas_result->GetGPUVirtualAddress();
        tlas_build.Inputs = tlas_inputs;
        tlas_build.SourceAccelerationStructureData = 0;
        tlas_build.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();
        cmd4->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);

        D3D12_RESOURCE_BARRIER tlas_uav{};
        tlas_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        tlas_uav.UAV.pResource = tlas_result.Get();
        cmd4->ResourceBarrier(1, &tlas_uav);

        ID3D12DescriptorHeap* heaps[] = {uav_heap.Get()};
        cmd4->SetDescriptorHeaps(1, heaps);
        cmd4->SetComputeRootSignature(global_rs.Get());
        cmd4->SetComputeRootDescriptorTable(0, uav_heap->GetGPUDescriptorHandleForHeapStart());
        cmd4->SetComputeRootShaderResourceView(1, tlas_result->GetGPUVirtualAddress());
        cmd4->SetPipelineState1(rtpso.Get());

        D3D12_DISPATCH_RAYS_DESC dispatch_desc{};
        dispatch_desc.RayGenerationShaderRecord.StartAddress = sbt_va + 0u * kSbtStride;
        dispatch_desc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatch_desc.MissShaderTable.StartAddress = sbt_va + 1u * kSbtStride;
        dispatch_desc.MissShaderTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatch_desc.MissShaderTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatch_desc.HitGroupTable.StartAddress = sbt_va + 2u * kSbtStride;
        dispatch_desc.HitGroupTable.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatch_desc.HitGroupTable.StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatch_desc.CallableShaderTable = {0, 0, 0};
        dispatch_desc.Width = kWidth;
        dispatch_desc.Height = kHeight;
        dispatch_desc.Depth = 1;
        cmd4->DispatchRays(&dispatch_desc);

        D3D12_RESOURCE_BARRIER copy_barriers[2]{};
        copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copy_barriers[0].Transition.pResource = uav_tex.Get();
        copy_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copy_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        copy_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copy_barriers[1].Transition.pResource = present_tex.Get();
        copy_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copy_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        copy_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd4->ResourceBarrier(2, copy_barriers);

        cmd4->CopyResource(present_tex.Get(), uav_tex.Get());

        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource = present_tex.Get();
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd4->ResourceBarrier(1, &to_present);

        PIXEndEventOnCommandList(cmd4.Get());
        PIXEndEventOnCpu();  // PathA_HardwareDXR
    } else {
        // =====================================================================
        // PATH B: Bit-Exact SAH Software BVH Fallback (XboxBvhBuilder.dll)
        //         + 64B-Aligned GPU SBT Upload + CopyTextureRegion + PresentX
        // =====================================================================
        PIXBeginEventOnCpu(kPixColorGold, "DXRTriangle::PathB_SoftwareSahBvh");

        // B1. Construct and upload a real 64B-aligned GPU SBT buffer on d3d12_xs
        constexpr uint64_t kSbtStride = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64B
        constexpr uint64_t kSbtBytes = kSbtStride * 4ull;                              // 256B
        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, kSbtBytes,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, sbt_buf);
        if (FAILED(hr) || !sbt_buf) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(SW SBT)", hr);
        }

        uint8_t sbt_cpu[kSbtBytes]{};
        FillSyntheticShaderId("RaygenShader", sbt_cpu + 0u * kSbtStride);
        FillSyntheticShaderId("MissShader", sbt_cpu + 1u * kSbtStride);
        FillSyntheticShaderId("TriangleHitGroup", sbt_cpu + 2u * kSbtStride);
        hr = UploadToBuffer(sbt_buf.Get(), sbt_cpu, sizeof(sbt_cpu));
        if (FAILED(hr)) {
            PIXEndEventOnCpu();
            return Fail("UploadToBuffer(SW SBT)", hr);
        }

        const D3D12_GPU_VIRTUAL_ADDRESS sbt_va = sbt_buf->GetGPUVirtualAddress();
        if (sbt_va == 0ull || (sbt_va % D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT) != 0ull) {
            PIXEndEventOnCpu();
            return Fail("SW SBT GPU VA alignment check failed");
        }

        // B2. Build real SAH BVH BLAS + TLAS via XboxBvhBuilder.dll
        XboxBvhCreateDesc bvh_desc{};
        bvh_desc.preferWarp = use_warp ? 1u : 0u;
        bvh_desc.forceSoftwarePath = 1u;
        bvh_desc.enableDebugLayer = 0u;
        bvh_desc.nativeD3D12Device = nullptr;

        XboxBvhBuilderHandle builder = nullptr;
        int32_t bvh_hr = XboxBvhCreateBuilder(&bvh_desc, &builder);
        if (bvh_hr != OS_SHIM_S_OK || !builder) {
            PIXEndEventOnCpu();
            return Fail("XboxBvhCreateBuilder", bvh_hr);
        }

        XboxBvhCapabilities bvh_caps{};
        bvh_hr = XboxBvhGetCapabilities(builder, &bvh_caps);
        if (bvh_hr != OS_SHIM_S_OK || bvh_caps.adapterDescription[0] == '\0') {
            XboxBvhDestroyBuilder(builder);
            PIXEndEventOnCpu();
            return Fail("XboxBvhGetCapabilities", bvh_hr);
        }

        XboxBvhPrebuildInputs blas_pre_in{};
        blas_pre_in.asType = XBOX_BVH_AS_TYPE_BOTTOM_LEVEL;
        blas_pre_in.buildFlags = XBOX_BVH_BUILD_PREFER_FAST_TRACE;
        blas_pre_in.primitiveCount = 2u;
        blas_pre_in.geometryCount = 1u;
        blas_pre_in.vertexStride = sizeof(Float3);
        blas_pre_in.isOpaque = 1u;

        XboxBvhPrebuildInfo blas_pre_info{};
        bvh_hr = XboxBvhGetPrebuildInfo(builder, &blas_pre_in, &blas_pre_info);
        if (bvh_hr != OS_SHIM_S_OK ||
            blas_pre_info.resultDataMaxSizeBytes == 0ull ||
            (blas_pre_info.resultDataMaxSizeBytes % 256ull) != 0ull) {
            XboxBvhDestroyBuilder(builder);
            PIXEndEventOnCpu();
            return Fail("XboxBvhGetPrebuildInfo(BLAS)", bvh_hr);
        }

        XboxBvhPrebuildInputs tlas_pre_in{};
        tlas_pre_in.asType = XBOX_BVH_AS_TYPE_TOP_LEVEL;
        tlas_pre_in.buildFlags = XBOX_BVH_BUILD_PREFER_FAST_TRACE;
        tlas_pre_in.primitiveCount = 1u;
        tlas_pre_in.geometryCount = 1u;
        tlas_pre_in.isOpaque = 1u;

        XboxBvhPrebuildInfo tlas_pre_info{};
        bvh_hr = XboxBvhGetPrebuildInfo(builder, &tlas_pre_in, &tlas_pre_info);
        if (bvh_hr != OS_SHIM_S_OK ||
            tlas_pre_info.resultDataMaxSizeBytes == 0ull ||
            (tlas_pre_info.resultDataMaxSizeBytes % 256ull) != 0ull) {
            XboxBvhDestroyBuilder(builder);
            PIXEndEventOnCpu();
            return Fail("XboxBvhGetPrebuildInfo(TLAS)", bvh_hr);
        }

        XboxBvhTriangle bvh_tris[2]{};
        for (uint32_t t = 0; t < 2u; ++t) {
            const Float3& v0 = kSceneVertices[t * 3u + 0u];
            const Float3& v1 = kSceneVertices[t * 3u + 1u];
            const Float3& v2 = kSceneVertices[t * 3u + 2u];
            bvh_tris[t].v0[0] = v0.x; bvh_tris[t].v0[1] = v0.y; bvh_tris[t].v0[2] = v0.z;
            bvh_tris[t].v1[0] = v1.x; bvh_tris[t].v1[1] = v1.y; bvh_tris[t].v1[2] = v1.z;
            bvh_tris[t].v2[0] = v2.x; bvh_tris[t].v2[1] = v2.y; bvh_tris[t].v2[2] = v2.z;
            bvh_tris[t].color[0] = 1.0f; bvh_tris[t].color[1] = 1.0f; bvh_tris[t].color[2] = 1.0f;
            bvh_tris[t].reflectivity = 0.0f;
        }

        uint64_t blas_id = 0ull;
        bvh_hr = XboxBvhBuildBlas(builder, bvh_tris, 2u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &blas_id);
        if (bvh_hr != OS_SHIM_S_OK || blas_id == 0ull) {
            XboxBvhDestroyBuilder(builder);
            PIXEndEventOnCpu();
            return Fail("XboxBvhBuildBlas", bvh_hr);
        }

        XboxBvhInstanceDesc inst{};
        inst.blasId = blas_id;
        inst.transform[0][0] = 1.0f;
        inst.transform[1][1] = 1.0f;
        inst.transform[2][2] = 1.0f;
        inst.instanceId = 0u;
        inst.instanceMask = 0xFFu;
        inst.hitGroupContribution = 0u;
        inst.flags = 0u;

        uint64_t tlas_id = 0ull;
        bvh_hr = XboxBvhBuildTlas(builder, &inst, 1u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_id);
        if (bvh_hr != OS_SHIM_S_OK || tlas_id == 0ull) {
            XboxBvhDestroyBuilder(builder);
            PIXEndEventOnCpu();
            return Fail("XboxBvhBuildTlas", bvh_hr);
        }

        // B3. Trace all 256x256 primary rays through XboxBvhTraceRay using identical shading math to Path A!
        std::vector<uint8_t> sw_rgba(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
        for (uint32_t y = 0; y < kHeight; ++y) {
            const float uv_y = (static_cast<float>(y) + 0.5f) / static_cast<float>(kHeight);
            const float ndc_y = 1.0f - uv_y * 2.0f;
            for (uint32_t x = 0; x < kWidth; ++x) {
                const float uv_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(kWidth);
                const float ndc_x = uv_x * 2.0f - 1.0f;

                XboxBvhRay ray{};
                ray.origin[0] = ndc_x;
                ray.origin[1] = ndc_y;
                ray.origin[2] = -1.0f;
                ray.direction[0] = 0.0f;
                ray.direction[1] = 0.0f;
                ray.direction[2] = 1.0f;
                ray.tMin = 0.001f;
                ray.tMax = 10.0f;

                XboxBvhHit hit{};
                bvh_hr = XboxBvhTraceRay(builder, tlas_id, &ray, &hit);
                if (bvh_hr != OS_SHIM_S_OK) {
                    XboxBvhDestroyBuilder(builder);
                    PIXEndEventOnCpu();
                    return Fail("XboxBvhTraceRay", bvh_hr);
                }

                const size_t px_off = (static_cast<size_t>(y) * kWidth + x) * 4u;
                if (hit.hit != 0u) {
                    const float w1 = hit.u;
                    const float w2 = hit.v;
                    const float w0 = 1.0f - w1 - w2;
                    float r = 0.0f;
                    float g = 0.0f;
                    float b = 0.0f;
                    if (hit.triangleIndex == 0u) {
                        r = w0;
                        g = w1;
                        b = w2;
                    } else {
                        r = 1.0f * w0 + 0.1f * w1 + 0.9f * w2;
                        g = 0.9f * w0 + 0.85f * w1 + 0.2f * w2;
                        b = 0.2f * w0 + 1.0f * w1 + 0.95f * w2;
                    }
                    sw_rgba[px_off + 0u] = FloatToUnorm8(r);
                    sw_rgba[px_off + 1u] = FloatToUnorm8(g);
                    sw_rgba[px_off + 2u] = FloatToUnorm8(b);
                    sw_rgba[px_off + 3u] = 0xFFu;
                } else {
                    sw_rgba[px_off + 0u] = kBgR;
                    sw_rgba[px_off + 1u] = kBgG;
                    sw_rgba[px_off + 2u] = kBgB;
                    sw_rgba[px_off + 3u] = kBgA;
                }
            }
        }

        XboxBvhDestroyBuilder(builder);

        // B4. Upload 256x256 RGBA8 framebuffer via CopyTextureRegion (slot 16) to present_tex
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0;
        UINT64 row_size_bytes = 0;
        UINT64 total_bytes = 0;
        const D3D12_RESOURCE_DESC actual_present_desc = present_tex->GetDesc();
        device->GetCopyableFootprints(
            &actual_present_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);

        hr = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total_bytes,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, sw_upload_buf);
        if (FAILED(hr) || !sw_upload_buf) {
            PIXEndEventOnCpu();
            return Fail("CreateBuffer(sw_upload_buf)", hr);
        }

        void* upload_mapped = nullptr;
        D3D12_RANGE read_none{0, 0};
        hr = sw_upload_buf->Map(0, &read_none, &upload_mapped);
        if (FAILED(hr) || !upload_mapped) {
            PIXEndEventOnCpu();
            return Fail("Map(sw_upload_buf)", hr);
        }
        auto* dst_bytes = static_cast<uint8_t*>(upload_mapped);
        const size_t src_row_pitch = static_cast<size_t>(kWidth) * 4u;
        for (uint32_t y = 0; y < kHeight; ++y) {
            std::memcpy(
                dst_bytes + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                sw_rgba.data() + static_cast<size_t>(y) * src_row_pitch,
                src_row_pitch);
        }
        sw_upload_buf->Unmap(0, nullptr);

        PIXBeginEventOnCommandList(cmd4.Get(), kPixColorGold, "UploadSwBvh_CopyTextureRegion");

        D3D12_RESOURCE_BARRIER to_copy_dst{};
        to_copy_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_dst.Transition.pResource = present_tex.Get();
        to_copy_dst.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_copy_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmd4->ResourceBarrier(1, &to_copy_dst);

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = present_tex.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = sw_upload_buf.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = footprint;

        cmd4->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource = present_tex.Get();
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd4->ResourceBarrier(1, &to_present);

        PIXEndEventOnCommandList(cmd4.Get());
        PIXEndEventOnCpu();  // PathB_SoftwareSahBvh
    }

    // 7. Close, ExecuteCommandLists, and PresentX (slot 19)
    hr = cmd4->Close();
    if (FAILED(hr)) {
        return Fail("CommandList4::Close", hr);
    }

    PIXBeginEventOnCommandQueue(queue.Get(), kPixColorBlue, "ExecuteAndPresentX");
    ID3D12CommandList* cmd_lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, cmd_lists);

    ID3D12Resource* plane_res[] = {present_tex.Get()};
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
        return Fail("PresentX(1)", hr);
    }

    ::QueryPerformanceCounter(&qpc_end);
    const double frame_ms = QpcMs(qpc_start, qpc_end, qpc_freq);
    PIXReportCounter(L"DXRTriangle.FrameMs", static_cast<float>(frame_ms));
    if (PIXGetEventCountForTest() == 0ull) {
        return Fail("PIXGetEventCountForTest returned 0");
    }

    // 8. Read back presented frame & verify exact geometry, depth sorting, and shading invariants
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

    // Corner miss pixels must be exact dark navy-blue (0x14, 0x18, 0x24, 0xFF)
    const uint32_t corners[4][2] = {{0, 0}, {kWidth - 1, 0}, {0, kHeight - 1}, {kWidth - 1, kHeight - 1}};
    for (const auto& c : corners) {
        const uint8_t* bg = pixel_at(c[0], c[1]);
        if (bg[0] != kBgR || bg[1] != kBgG || bg[2] != kBgB || bg[3] != kBgA) {
            return Fail("Miss background pixel mismatch");
        }
    }

    // Primary Triangle (Tri 0): center (128,128) interpolated barycentric RGB (R~127, G~65, B~63)
    const uint8_t* center = pixel_at(128, 128);
    if (center[0] < 100u || center[0] > 150u ||
        center[1] < 50u || center[1] > 85u ||
        center[2] < 50u || center[2] > 85u ||
        center[3] != 255u) {
        return Fail("Center triangle pixel verification failed");
    }

    // Near top vertex v0=(0.0, 0.5, 0.0) -> Red
    const uint8_t* top_px = pixel_at(128, 72);
    if (top_px[0] < 210u || top_px[1] > 35u || top_px[2] > 35u || top_px[3] != 255u) {
        return Fail("Top Red vertex pixel verification failed");
    }
    // Near right vertex v1=(0.5, -0.5, 0.0) -> Green
    const uint8_t* right_px = pixel_at(184, 188);
    if (right_px[1] < 210u || right_px[0] > 35u || right_px[2] > 35u || right_px[3] != 255u) {
        return Fail("Right Green vertex pixel verification failed");
    }
    // Near left vertex v2=(-0.5, -0.5, 0.0) -> Blue
    const uint8_t* left_px = pixel_at(72, 188);
    if (left_px[2] < 210u || left_px[0] > 35u || left_px[1] > 35u || left_px[3] != 255u) {
        return Fail("Left Blue vertex pixel verification failed");
    }

    // Depth-sorting overlap check at (151, 123):
    // Both Tri 0 (z=0.0, t=1.0 -> R~136, G~106, B~12) and Tri 1 (z=0.5, t=1.5 -> Magenta B~228) cover this ray.
    // Closest-hit MUST return Tri 0!
    const uint8_t* overlap_px = pixel_at(151, 123);
    if (overlap_px[0] < 115u || overlap_px[1] < 90u || overlap_px[2] > 30u || overlap_px[3] != 255u) {
        return Fail("Closest-hit depth sorting check failed at overlap pixel (151,123)");
    }

    // Secondary Triangle (Tri 1) visible region check at (196, 124) near s1 (Cyan: R~69, G~193, B~242)
    const uint8_t* sec_px = pixel_at(196, 124);
    if (sec_px[2] < 210u || sec_px[1] < 170u || sec_px[0] > 95u || sec_px[3] != 255u) {
        return Fail("Secondary triangle visible pixel verification failed at (196,124)");
    }

    uint32_t hit_pixels = 0;
    std::unordered_set<uint32_t> unique_rgb;
    unique_rgb.reserve(8192);
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
        if (r != kBgR || g != kBgG || b != kBgB) {
            ++hit_pixels;
        }
        unique_rgb.insert((static_cast<uint32_t>(r) << 16) |
                          (static_cast<uint32_t>(g) << 8) |
                          static_cast<uint32_t>(b));
    }
    if (hit_pixels != kExpectedHitPixels) {
        return Fail("Unexpected raytraced hit pixel count", hit_pixels);
    }
    if (unique_rgb.size() < 2000u) {
        return Fail("Too few unique shaded colors in DXRTriangle",
                    static_cast<int64_t>(unique_rgb.size()));
    }

    // 9. Write T:\dxr_triangle.ppm and T:\metrics.txt
    if (!WritePpmW(L"T:\\dxr_triangle.ppm", kWidth, kHeight, pixels.data())) {
        return Fail("Write T:\\dxr_triangle.ppm", ::GetLastError());
    }

    const char* mode_str = use_hw_dxr ? "hw_dxr" : "sw_bvh_fallback";
    char metrics_buf[768]{};
    const int metrics_len = std::snprintf(
        metrics_buf, sizeof(metrics_buf),
        "sample=DXRTriangle\n"
        "mode=%s\n"
        "dxr_tier=%u\n"
        "adapter=%s\n"
        "is_warp=%u\n"
        "title_id=0x%08X\n"
        "width=%u\n"
        "height=%u\n"
        "fnv1a=0x%016llX\n"
        "hit_pixels=%u\n"
        "unique_colors=%zu\n"
        "frame_ms=%.3f\n",
        mode_str,
        dxr_tier,
        gpu_info.AdapterName,
        gpu_info.IsWarp,
        title_id,
        out_w,
        out_h,
        static_cast<unsigned long long>(frame_hash),
        hit_pixels,
        unique_rgb.size(),
        frame_ms);
    if (metrics_len <= 0 ||
        !WriteWholeFileW(L"T:\\metrics.txt", metrics_buf, static_cast<size_t>(metrics_len))) {
        return Fail("Write T:\\metrics.txt", ::GetLastError());
    }

    // 10. Blank teardown PresentX(0) — standard Xbox GDK DeviceResources teardown pattern
    hr = queue->PresentX(0, nullptr, nullptr);
    if (FAILED(hr)) {
        return Fail("PresentX(0) blank teardown", hr);
    }

    std::printf(
        "[gdk_dxr_triangle] PASS | Mode=%s (DXR=%u) | Adapter=%s (%s) | TitleId=0x%08X | %ux%u | Hits=%u | Colors=%zu | FNV1a=0x%016llX | %.2f ms\n",
        mode_str,
        dxr_tier,
        gpu_info.AdapterName,
        gpu_info.IsWarp ? "WARP" : "HW",
        title_id,
        out_w,
        out_h,
        hit_pixels,
        unique_rgb.size(),
        static_cast<unsigned long long>(frame_hash),
        frame_ms);
    std::fflush(stdout);

    rtpso_props.Reset();
    rtpso.Reset();
    global_rs.Reset();
    uav_heap.Reset();
    sw_upload_buf.Reset();
    uav_tex.Reset();
    sbt_buf.Reset();
    instance_upload.Reset();
    tlas_result.Reset();
    tlas_scratch.Reset();
    blas_result.Reset();
    blas_scratch.Reset();
    vb.Reset();
    present_tex.Reset();
    cmd4.Reset();
    cmd.Reset();
    alloc.Reset();
    queue.Reset();
    device5.Reset();
    device.Reset();
    XGameRuntimeUninitialize();
    return 0;
}
