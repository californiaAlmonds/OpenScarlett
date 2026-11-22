#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/xg_xs_abi.h"
#include "openscarlett/middleware/ffx_gpu_fsr.h"
#include "os_check.h"

namespace {

#if defined(_WIN32)

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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // D3D12 pipeline state stream subobjects require pointer alignment
#endif
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeVal, typename InnerType>
struct alignas(void*) PsoStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = TypeVal;
    InnerType inner{};
};

struct MeshShaderPsoStream {
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_sig;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
    PsoStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool CompileHlslWithDxc(
    const std::string& dxc_exe,
    const std::filesystem::path& scratch_dir,
    const std::string& base_name,
    const char* hlsl_source,
    const char* profile,
    const char* entry_point,
    std::vector<uint8_t>& out_dxil) {
    std::error_code ec;
    std::filesystem::create_directories(scratch_dir, ec);

    const std::filesystem::path hlsl_path = scratch_dir / (base_name + ".hlsl");
    const std::filesystem::path dxil_path = scratch_dir / (base_name + ".dxil");

    {
        std::ofstream ofs(hlsl_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(hlsl_source, static_cast<std::streamsize>(std::strlen(hlsl_source)));
    }

    std::string cmd = "\"" + dxc_exe + "\" -T " + profile + " -E " + entry_point +
                      " -Fo \"" + dxil_path.string() + "\" \"" + hlsl_path.string() + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) return false;

    std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz <= 0) return false;
    out_dxil.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);
    return ifs.good() || ifs.eof();
}

std::string FindDxcExecutable() {
    std::error_code ec;
    char self_buf[MAX_PATH]{};
    const DWORD self_len = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
    if (self_len > 0 && self_len < sizeof(self_buf)) {
        auto beside = std::filesystem::path(self_buf).parent_path() / "dxc.exe";
        if (std::filesystem::exists(beside, ec)) return beside.string();
    }
    char env_dxc[1024]{};
    const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
    if (env_len > 0 && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, ec)) {
        return std::string(env_dxc);
    }
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf);
    }
    const char* candidates[] = {
        R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe)",
        R"(C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe)",
    };
    for (const char* p : candidates) {
        if (std::filesystem::exists(p, ec)) return p;
    }
    const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (std::filesystem::exists(kits_bin, ec)) {
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            auto cand = entry.path() / "x64" / "dxc.exe";
            if (std::filesystem::exists(cand, ec)) found.push_back(cand);
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) return found.back().string();
    }
    return {};
}

// ---------------------------------------------------------------------------
// 1. High-Performance Adapter Selection Test (GTX 1650 Ti + WARP)
// ---------------------------------------------------------------------------
bool TestAdapterSelection(openscarlett::D3D12AdapterInfo& out_hw_info) {
    std::printf("[Test 1] D3D12 High-Performance Adapter Selection...\n");

    openscarlett::D3D12AdapterSelectionOptions hw_opts{};
    hw_opts.prefer_warp = false;
    hw_opts.allow_warp_fallback = false;
    TestComPtr<ID3D12Device> hw_dev;
    TestComPtr<IDXGIAdapter1> hw_adapter;
    openscarlett::Status st = openscarlett::SelectD3D12AdapterAndCreateDevice(
        hw_opts, hw_dev.Put(), hw_adapter.Put(), &out_hw_info);
    const bool has_hw_gpu = st.ok() && hw_dev.Get() != nullptr && !out_hw_info.is_warp;
    if (has_hw_gpu) {
        // On hybrid laptops with both an integrated GPU and a discrete GPU, verify we
        // selected the high-performance discrete GPU rather than falling back to the iGPU.
        std::vector<openscarlett::D3D12AdapterInfo> all_adapters;
        bool has_discrete_non_intel = false;
        if (openscarlett::EnumerateD3D12Adapters(all_adapters).ok()) {
            for (const auto& a : all_adapters) {
                if (!a.is_warp &&
                    a.description.find("Intel") == std::string::npos &&
                    a.dedicated_video_memory_mb >= 1024ull) {
                    has_discrete_non_intel = true;
                }
            }
        }
        if (has_discrete_non_intel) {
            OS_REQUIRE(out_hw_info.dedicated_video_memory_mb >= 1024ull);
            OS_REQUIRE(out_hw_info.description.find("Intel") == std::string::npos);
        }

        std::printf(
            "  HW Selected : %s | Vendor=0x%04X Device=0x%04X | VRAM=%llu MB | SM=0x%02X | MeshTier=%u | DXR=%u | Wave=%u..%u\n",
            out_hw_info.description.c_str(),
            out_hw_info.vendor_id,
            out_hw_info.device_id,
            static_cast<unsigned long long>(out_hw_info.dedicated_video_memory_mb),
            out_hw_info.highest_shader_model,
            out_hw_info.mesh_shader_tier,
            out_hw_info.raytracing_tier,
            out_hw_info.wave_lane_min,
            out_hw_info.wave_lane_max);
    } else {
        std::printf("  HW Selected : none (WARP-only runner; skipping HW-only assertions)\n");
    }

    openscarlett::D3D12AdapterSelectionOptions warp_opts{};
    warp_opts.prefer_warp = true;
    TestComPtr<ID3D12Device> warp_dev;
    TestComPtr<IDXGIAdapter1> warp_adapter;
    openscarlett::D3D12AdapterInfo warp_info{};
    st = openscarlett::SelectD3D12AdapterAndCreateDevice(
        warp_opts, warp_dev.Put(), warp_adapter.Put(), &warp_info);
    OS_REQUIRE(st.ok());
    OS_REQUIRE(warp_dev.Get() != nullptr);
    OS_REQUIRE(warp_info.is_warp);

    std::printf(
        "  WARP Select : %s | SM=0x%02X | MeshTier=%u | Wave=%u..%u\n",
        warp_info.description.c_str(),
        warp_info.highest_shader_model,
        warp_info.mesh_shader_tier,
        warp_info.wave_lane_min,
        warp_info.wave_lane_max);
    return has_hw_gpu;
}

// ---------------------------------------------------------------------------
// 2. d3d12_xs.dll Clear + PresentX Headless Readback Test (HW & WARP)
// ---------------------------------------------------------------------------
uint64_t RunClearAndPresentXTest(bool use_warp, bool expect_mesh_shaders) {
    const char* mode_label = use_warp ? "WARP" : "HW";
    std::printf("[Test 2 - %s] d3d12_xs ClearRenderTargetView + PresentX...\n", mode_label);

    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);
    OpenScarlettD3D12ResetTelemetry();

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;

    TestComPtr<ID3D12DeviceX> dev;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(dev.Put()));
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(dev.Get() != nullptr);

    // Verify Xbox extensions on DeviceX
    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    OS_REQUIRE(SUCCEEDED(dev->GetGpuHardwareInfoX(&gpu_info)));
    OS_REQUIRE((gpu_info.IsWarp != 0) == use_warp);
    if (!use_warp && expect_mesh_shaders) {
        OS_REQUIRE(gpu_info.MeshShaderTier >= 10u);
    }
    OS_REQUIRE(SUCCEEDED(dev->SetFrameIntervalX(nullptr, 16667u, 2u, 0u)));
    D3D12XBOX_FRAME_PIPELINE_TOKEN frame_token = 0;
    OS_REQUIRE(SUCCEEDED(dev->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, 1000u, nullptr, 0u, &frame_token)));
    OS_REQUIRE(frame_token >= 1ull);
    OS_REQUIRE(SUCCEEDED(dev->SuspendX(0u)));
    OS_REQUIRE(SUCCEEDED(dev->ResumeX()));

    // Verify standard D3D12CreateDevice & D3D12CreateDeviceX exports work end-to-end
    HMODULE d3d12_xs_mod = GetModuleHandleA("d3d12_xs.dll");
    OS_REQUIRE(d3d12_xs_mod != nullptr);
    using PfnCreateDev = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto fn_create_dev = reinterpret_cast<PfnCreateDev>(
        GetProcAddress(d3d12_xs_mod, "D3D12CreateDevice"));
    OS_REQUIRE(fn_create_dev != nullptr);
    OS_REQUIRE(GetProcAddress(d3d12_xs_mod, "D3D12SerializeRootSignature") != nullptr);
    OS_REQUIRE(GetProcAddress(d3d12_xs_mod, "D3D12GetDebugInterface") != nullptr);
    {
        TestComPtr<ID3D12DeviceX> dev_via_std;
        OS_REQUIRE(SUCCEEDED(fn_create_dev(
            nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX,
            reinterpret_cast<void**>(dev_via_std.Put()))));
        OS_REQUIRE(dev_via_std.Get() != nullptr);

        TestComPtr<ID3D12DeviceX> dev_via_x;
        OS_REQUIRE(SUCCEEDED(D3D12CreateDeviceX(
            nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX,
            reinterpret_cast<void**>(dev_via_x.Put()))));
        OS_REQUIRE(dev_via_x.Get() != nullptr);
    }

    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    TestComPtr<ID3D12CommandQueueX> queue;
    hr = dev->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()));
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(queue.Get() != nullptr);

    // Verify GetDevice on CommandQueueX returns wrapped DeviceX
    {
        TestComPtr<ID3D12DeviceX> q_dev;
        OS_REQUIRE(SUCCEEDED(queue->GetDevice(
            IID_ID3D12DeviceX, reinterpret_cast<void**>(q_dev.Put()))));
        OS_REQUIRE(q_dev.Get() == dev.Get());
    }

    TestComPtr<ID3D12CommandAllocator> alloc;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()));
    OS_REQUIRE(SUCCEEDED(hr));

    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    hr = dev->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()));
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(cmd.Get() != nullptr);
    OS_REQUIRE(cmd->GetUnderlyingHostCommandList() != nullptr);

    // Verify CreateCommandList1 & CreatePlacedResourceX
    {
        TestComPtr<ID3D12GraphicsCommandListX> cmd1;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommandList1(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
            IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd1.Put()))));
        OS_REQUIRE(cmd1.Get() != nullptr);

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = 4096;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.Format = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        TestComPtr<ID3D12Resource> placed_x;
        OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
            0x0000000100000000ull, &buf_desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(placed_x.Put()))));
        OS_REQUIRE(placed_x.Get() != nullptr);
    }

    constexpr uint32_t kWidth = 64u;
    constexpr uint32_t kHeight = 64u;

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kWidth;
    rt_desc.Height = kHeight;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear_val.Color[0] = static_cast<float>(0x22) / 255.0f;
    clear_val.Color[1] = static_cast<float>(0x88) / 255.0f;
    clear_val.Color[2] = static_cast<float>(0xEE) / 255.0f;
    clear_val.Color[3] = 1.0f;

    TestComPtr<ID3D12Resource> rt;
    hr = dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, &clear_val, IID_PPV_ARGS(rt.Put()));
    OS_REQUIRE(SUCCEEDED(hr));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    TestComPtr<ID3D12DescriptorHeap> rtv_heap;
    hr = dev->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()));
    OS_REQUIRE(SUCCEEDED(hr));

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_handle);

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = rt.Get();
    to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &to_rt);

    cmd->ClearRenderTargetView(rtv_handle, clear_val.Color, 0, nullptr);

    D3D12_RESOURCE_BARRIER to_present{};
    to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_present.Transition.pResource = rt.Get();
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &to_present);

    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    // Execute via CommandQueueX (must unwrap CommandListX* -> native host ID3D12CommandList*)
    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    // Call PresentX in headless readback mode
    ID3D12Resource* plane_res[] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = frame_token;
    plane.ResourceCount = 1;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;

    D3D12XBOX_PRESENT_PARAMETERS pres_params{};
    pres_params.SyncInterval = 1;

    hr = queue->PresentX(1, &plane, &pres_params);
    OS_REQUIRE(SUCCEEDED(hr));

    uint64_t hash = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    hr = OpenScarlettD3D12GetLastPresentHash(queue.Get(), &hash, &out_w, &out_h);
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(out_w == kWidth);
    OS_REQUIRE(out_h == kHeight);
    OS_REQUIRE(hash != 0ull);

    // Also verify IOpenScarlettPresentCapture COM QI directly on CommandQueueX
    {
        TestComPtr<IOpenScarlettPresentCapture> cap;
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentCapture, reinterpret_cast<void**>(cap.Put()))));
        uint64_t cap_hash = 0;
        uint32_t cap_w = 0;
        uint32_t cap_h = 0;
        uint64_t cap_count = 0;
        OS_REQUIRE(SUCCEEDED(cap->GetLastPresentStats(&cap_hash, &cap_w, &cap_h, &cap_count)));
        OS_REQUIRE(cap_hash == hash && cap_w == kWidth && cap_h == kHeight && cap_count == 1ull);
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    hr = OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied);
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(copied == pixels.size());

    // Verify EVERY single pixel is exact (0x22, 0x88, 0xEE, 0xFF)
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        OS_REQUIRE(pixels[i * 4u + 0u] == 0x22u);
        OS_REQUIRE(pixels[i * 4u + 1u] == 0x88u);
        OS_REQUIRE(pixels[i * 4u + 2u] == 0xEEu);
        OS_REQUIRE(pixels[i * 4u + 3u] == 0xFFu);
    }

    // Verify CallTelemetry logged exact COM vtable slots
    int32_t slot = -1;
    uint64_t calls = 0;
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12CommandQueueX::PresentX", &slot, &calls) == 1);
    OS_REQUIRE(slot == 19);
    OS_REQUIRE(calls >= 1);

    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12CommandQueueX::ExecuteCommandLists", &slot, &calls) == 1);
    OS_REQUIRE(slot == 10);
    OS_REQUIRE(calls >= 1);

    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12GraphicsCommandListX::ClearRenderTargetView", &slot, &calls) == 1);
    OS_REQUIRE(slot == 48);
    OS_REQUIRE(calls >= 1);

    std::printf("  Adapter=%s | %ux%u | Exact RGBA=(0x22,0x88,0xEE,0xFF) | FNV1a=0x%016llX\n",
                gpu_info.AdapterName, out_w, out_h, static_cast<unsigned long long>(hash));

    OpenScarlettD3D12SetPreferWarp(0);
    return hash;
}

// ---------------------------------------------------------------------------
// 3. Live GTX 1650 Ti Hardware Mesh Shader (ms_6_5 + ps_6_5) + DispatchMesh
// ---------------------------------------------------------------------------
void TestLiveHardwareMeshShader() {
    std::printf("[Test 3] Live Hardware Mesh Shader (ms_6_5 + ps_6_5) via CommandListX::DispatchMesh...\n");

    const std::string dxc_exe = FindDxcExecutable();
    OS_REQUIRE(!dxc_exe.empty());

    static constexpr const char* kMeshShaderHlsl = R"(
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void MsMain(out vertices VertexOut verts[3], out indices uint3 tris[1]) {
    SetMeshOutputCounts(3, 1);

    verts[0].position = float4(-0.8, -0.8, 0.0, 1.0);
    verts[0].color    = float4(240.0 / 255.0, 64.0 / 255.0, 144.0 / 255.0, 1.0);

    verts[1].position = float4( 0.0,  0.8, 0.0, 1.0);
    verts[1].color    = float4(240.0 / 255.0, 64.0 / 255.0, 144.0 / 255.0, 1.0);

    verts[2].position = float4( 0.8, -0.8, 0.0, 1.0);
    verts[2].color    = float4(240.0 / 255.0, 64.0 / 255.0, 144.0 / 255.0, 1.0);

    tris[0] = uint3(0, 1, 2);
}
)";

    static constexpr const char* kPixelShaderHlsl = R"(
struct VertexOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

float4 PsMain(VertexOut input) : SV_Target0 {
    return input.color;
}
)";

    std::error_code scratch_ec;
    std::filesystem::path scratch_base = std::filesystem::temp_directory_path(scratch_ec);
    if (scratch_ec || scratch_base.empty()) {
        scratch_base = std::filesystem::path(OPENSCARLETT_BINARY_DIR);
    }
    const std::filesystem::path scratch_dir = scratch_base / "openscarlett_shader_scratch";
    std::vector<uint8_t> ms_dxil;
    std::vector<uint8_t> ps_dxil;
    OS_REQUIRE(CompileHlslWithDxc(
        dxc_exe, scratch_dir, "test_mesh_ms65", kMeshShaderHlsl, "ms_6_5", "MsMain", ms_dxil));
    OS_REQUIRE(CompileHlslWithDxc(
        dxc_exe, scratch_dir, "test_mesh_ps65", kPixelShaderHlsl, "ps_6_5", "PsMain", ps_dxil));
    OS_REQUIRE(!ms_dxil.empty() && !ps_dxil.empty());

    OpenScarlettD3D12SetPreferWarp(0);
    OpenScarlettD3D12ResetTelemetry();

    D3D12XBOX_CREATE_DEVICE_PARAMETERS dev_params{};
    dev_params.Version = 1;

    TestComPtr<ID3D12DeviceX> dev;
    HRESULT hr = D3D12XboxCreateDevice(
        nullptr, &dev_params, IID_ID3D12DeviceX, reinterpret_cast<void**>(dev.Put()));
    OS_REQUIRE(SUCCEEDED(hr));

    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};
    OS_REQUIRE(SUCCEEDED(dev->GetGpuHardwareInfoX(&gpu_info)));
    OS_REQUIRE(gpu_info.IsWarp == 0u);
    OS_REQUIRE(gpu_info.MeshShaderTier >= 10u);

    // Empty root signature
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 0;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    TestComPtr<ID3DBlob> rs_blob;
    TestComPtr<ID3DBlob> err_blob;
    using PfnSerRS = HRESULT(WINAPI*)(
        const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto ser_rs = reinterpret_cast<PfnSerRS>(
        openscarlett::ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    OS_REQUIRE(ser_rs != nullptr);
    hr = ser_rs(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), err_blob.Put());
    OS_REQUIRE(SUCCEEDED(hr));

    TestComPtr<ID3D12RootSignature> root_sig;
    hr = dev->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()));
    OS_REQUIRE(SUCCEEDED(hr));

    // Mesh Shader PSO via ID3D12Device2::CreatePipelineState
    MeshShaderPsoStream stream{};
    stream.root_sig.inner = root_sig.Get();
    stream.ms.inner = {ms_dxil.data(), ms_dxil.size()};
    stream.ps.inner = {ps_dxil.data(), ps_dxil.size()};

    stream.blend.inner.AlphaToCoverageEnable = FALSE;
    stream.blend.inner.IndependentBlendEnable = FALSE;
    stream.blend.inner.RenderTarget[0].BlendEnable = FALSE;
    stream.blend.inner.RenderTarget[0].LogicOpEnable = FALSE;
    stream.blend.inner.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    stream.blend.inner.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    stream.blend.inner.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    stream.blend.inner.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    stream.blend.inner.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    stream.blend.inner.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    stream.blend.inner.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    stream.blend.inner.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    stream.rasterizer.inner.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rasterizer.inner.CullMode = D3D12_CULL_MODE_NONE;
    stream.rasterizer.inner.DepthClipEnable = TRUE;

    stream.depth_stencil.inner.DepthEnable = FALSE;
    stream.depth_stencil.inner.StencilEnable = FALSE;
    stream.sample_mask.inner = UINT_MAX;
    stream.sample_desc.inner = {1, 0};
    stream.rt_formats.inner.NumRenderTargets = 1;
    stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D12_PIPELINE_STATE_STREAM_DESC pso_stream_desc{};
    pso_stream_desc.SizeInBytes = sizeof(stream);
    pso_stream_desc.pPipelineStateSubobjectStream = &stream;

    TestComPtr<ID3D12PipelineState> mesh_pso;
    hr = dev->CreatePipelineState(&pso_stream_desc, IID_PPV_ARGS(mesh_pso.Put()));
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(mesh_pso.Get() != nullptr);

    D3D12_COMMAND_QUEUE_DESC q_desc{};
    q_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    TestComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &q_desc, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()))));

    TestComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));

    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), mesh_pso.Get(),
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));

    constexpr uint32_t kWidth = 64u;
    constexpr uint32_t kHeight = 64u;

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = kWidth;
    rt_desc.Height = kHeight;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE bg_clear{};
    bg_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_clear.Color[0] = static_cast<float>(0x10) / 255.0f;
    bg_clear.Color[1] = static_cast<float>(0x10) / 255.0f;
    bg_clear.Color[2] = static_cast<float>(0x10) / 255.0f;
    bg_clear.Color[3] = 1.0f;

    TestComPtr<ID3D12Resource> rt;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_COMMON, &bg_clear, IID_PPV_ARGS(rt.Put()))));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    TestComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.Put()))));

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_handle);

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = rt.Get();
    to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &to_rt);

    cmd->ClearRenderTargetView(rtv_handle, bg_clear.Color, 0, nullptr);
    cmd->SetGraphicsRootSignature(root_sig.Get());
    cmd->SetPipelineState(mesh_pso.Get());

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    // Hardware Mesh Shader dispatch through CommandListX (vtable slot 79!)
    cmd->DispatchMesh(1, 1, 1);

    D3D12_RESOURCE_BARRIER to_present{};
    to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_present.Transition.pResource = rt.Get();
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &to_present);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Resource* plane_res[] = {rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.ResourceCount = 1;
    plane.ppResources = plane_res;
    plane.ResourceState = D3D12_RESOURCE_STATE_PRESENT;
    D3D12XBOX_PRESENT_PARAMETERS pres_params{};
    pres_params.SyncInterval = 1;
    OS_REQUIRE(SUCCEEDED(queue->PresentX(1, &plane, &pres_params)));

    uint64_t mesh_hash = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetLastPresentHash(queue.Get(), &mesh_hash, &out_w, &out_h)));
    OS_REQUIRE(out_w == kWidth && out_h == kHeight && mesh_hash != 0ull);

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
    uint32_t copied = 0;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CopyLastPresentPixels(
        queue.Get(), pixels.data(), static_cast<uint32_t>(pixels.size()), &copied)));
    OS_REQUIRE(copied == pixels.size());

    // Corner pixel (0,0) must be background clear (0x10, 0x10, 0x10, 0xFF)
    const uint8_t* corner = &pixels[0];
    OS_REQUIRE(corner[0] == 0x10u && corner[1] == 0x10u && corner[2] == 0x10u && corner[3] == 0xFFu);

    // Center pixel (32,32) must be rasterized mesh shader triangle color (0xF0, 0x40, 0x90, 0xFF)
    const uint8_t* center = &pixels[(32u * kWidth + 32u) * 4u];
    OS_REQUIRE(std::abs(static_cast<int>(center[0]) - 0xF0) <= 1);
    OS_REQUIRE(std::abs(static_cast<int>(center[1]) - 0x40) <= 1);
    OS_REQUIRE(std::abs(static_cast<int>(center[2]) - 0x90) <= 1);
    OS_REQUIRE(center[3] == 0xFFu);

    uint32_t tri_pixels = 0;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
        if (pixels[i * 4u + 0u] > 200u && pixels[i * 4u + 2u] > 100u) {
            ++tri_pixels;
        }
    }
    OS_REQUIRE(tri_pixels > 500u && tri_pixels < kWidth * kHeight);

    // Verify CallTelemetry recorded DispatchMesh with exact vtable slot 79
    int32_t slot = -1;
    uint64_t calls = 0;
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry(
                   "ID3D12GraphicsCommandListX::DispatchMesh", &slot, &calls) == 1);
    OS_REQUIRE(slot == 79);
    OS_REQUIRE(calls == 1);

    int32_t pso_slot = -1;
    uint64_t pso_calls = 0;
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry(
                   "ID3D12DeviceX::CreatePipelineState", &pso_slot, &pso_calls) == 1);
    OS_REQUIRE(pso_slot == 47);
    OS_REQUIRE(pso_calls >= 1);

    std::printf(
        "  MeshShader  : %s | ms_6_5=%zu B, ps_6_5=%zu B | TriPixels=%u/%u | Center=(%u,%u,%u,%u) | DispatchMeshSlot=%d | FNV1a=0x%016llX\n",
        gpu_info.AdapterName,
        ms_dxil.size(),
        ps_dxil.size(),
        tri_pixels,
        kWidth * kHeight,
        center[0], center[1], center[2], center[3],
        slot,
        static_cast<unsigned long long>(mesh_hash));
}

// ---------------------------------------------------------------------------
// 4. xg_xs.dll Swizzle / Deswizzle Bit-Exact Round-Trip Test
// ---------------------------------------------------------------------------
void VerifyXgRoundTrip(
    const char* label,
    uint32_t dxgi_format,
    uint32_t width,
    uint32_t height,
    XG_TILE_MODE tile_mode) {
    XG_FORMAT_BLOCK_INFO fmt{};
    OS_REQUIRE(XGGetFormatBlockInfo(dxgi_format, &fmt) == 0);

    XG_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = dxgi_format;
    desc.SampleCount = 1;
    desc.TileMode = tile_mode;

    XG_RESOURCE_LAYOUT layout{};
    OS_REQUIRE(XGComputeMipLayout(&desc, &layout) == 0);
    OS_REQUIRE(layout.SizeBytes > 0ull);

    const uint32_t elems_x = (width + fmt.block_width - 1u) / fmt.block_width;
    const uint32_t elems_y = (height + fmt.block_height - 1u) / fmt.block_height;
    const uint32_t row_pitch = elems_x * fmt.bytes_per_block;
    const size_t linear_bytes = static_cast<size_t>(row_pitch) * elems_y;

    std::vector<uint8_t> src_linear(linear_bytes);
    for (size_t i = 0; i < linear_bytes; ++i) {
        src_linear[i] = static_cast<uint8_t>((i * 131u + (i >> 5) * 37u + 19u) & 0xFFu);
    }

    std::vector<uint8_t> tiled(static_cast<size_t>(layout.SizeBytes), 0xAAu);
    OS_REQUIRE(XGTileSurface2D(&desc, 0, 0, src_linear.data(), row_pitch, tiled.data(), tiled.size()) == 0);

    // Prove genuine swizzling occurred (tiled bytes differ from linear bytes!)
    if (tile_mode != XG_TILE_MODE_LINEAR) {
        size_t diff_bytes = 0;
        const size_t cmp_len = std::min(src_linear.size(), tiled.size());
        for (size_t i = 0; i < cmp_len; ++i) {
            if (src_linear[i] != tiled[i]) ++diff_bytes;
        }
        OS_REQUIRE(diff_bytes > cmp_len / 4u);
    }

    std::vector<uint8_t> dst_linear(linear_bytes, 0x55u);
    OS_REQUIRE(XGUntileSurface2D(&desc, 0, 0, tiled.data(), tiled.size(), dst_linear.data(), row_pitch) == 0);
    OS_REQUIRE(std::memcmp(src_linear.data(), dst_linear.data(), linear_bytes) == 0);

    // Also verify IXGTextureAddressComputer COM object matches bit-for-bit
    IXGTextureAddressComputer* computer = nullptr;
    OS_REQUIRE(XGCreateTexture2DComputer(&desc, &computer) == 0);
    OS_REQUIRE(computer != nullptr);
    std::vector<uint8_t> tiled2(static_cast<size_t>(layout.SizeBytes), 0x00u);
    std::vector<uint8_t> dst_linear2(linear_bytes, 0x00u);
    OS_REQUIRE(computer->CopyIntoSubresource(tiled2.data(), 0, 0, src_linear.data(), row_pitch, 0) == 0);
    OS_REQUIRE(std::memcmp(tiled.data(), tiled2.data(), tiled.size()) == 0);
    OS_REQUIRE(computer->CopyFromSubresource(dst_linear2.data(), 0, 0, tiled2.data(), row_pitch, 0) == 0);
    OS_REQUIRE(std::memcmp(src_linear.data(), dst_linear2.data(), linear_bytes) == 0);
    computer->Release();

    std::printf("  XG %-28s | %ux%u | linear=%zu B -> tiled=%llu B | BIT-EXACT\n",
                label, width, height, linear_bytes,
                static_cast<unsigned long long>(layout.SizeBytes));
}

void TestXgSwizzle() {
    std::printf("[Test 4] xg_xs.dll RDNA2/Scarlett Texture Swizzle & Deswizzle...\n");
    OpenScarlettXgResetTelemetry();

    VerifyXgRoundTrip("RGBA8 4KB_S NPOT", DXGI_FORMAT_R8G8B8A8_UNORM, 137, 93, XG_TILE_MODE_2D_THIN_4KB_S);
    VerifyXgRoundTrip("RGBA8 64KB_S NPOT", DXGI_FORMAT_R8G8B8A8_UNORM, 137, 93, XG_TILE_MODE_2D_THIN_64KB_S);
    VerifyXgRoundTrip("RGBA8 4KB_S 256", DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, XG_TILE_MODE_2D_THIN_4KB_S);
    VerifyXgRoundTrip("RGBA8 64KB_S 256", DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, XG_TILE_MODE_2D_THIN_64KB_S);
    VerifyXgRoundTrip("RGBA8 64KB_D 256", DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, XG_TILE_MODE_2D_THIN_64KB_D);
    VerifyXgRoundTrip("RGBA8 64KB_R_X 256", DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, XG_TILE_MODE_2D_THIN_64KB_R_X);
    VerifyXgRoundTrip("BC7 4KB_S 128", DXGI_FORMAT_BC7_UNORM, 128, 128, XG_TILE_MODE_2D_THIN_4KB_S);
    VerifyXgRoundTrip("BC7 64KB_S 128", DXGI_FORMAT_BC7_UNORM, 128, 128, XG_TILE_MODE_2D_THIN_64KB_S);

    OS_REQUIRE(XGComputeOptimalTileMode(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080, XG_BIND_DISPLAYABLE) ==
               XG_TILE_MODE_2D_THIN_64KB_D);
    OS_REQUIRE(XGComputeOptimalTileMode(DXGI_FORMAT_R8G8B8A8_UNORM, 1920, 1080, XG_BIND_RENDER_TARGET) ==
               XG_TILE_MODE_2D_THIN_64KB_R_X);
    OS_REQUIRE(XGComputeOptimalTileMode(DXGI_FORMAT_BC7_UNORM, 512, 512, XG_BIND_SHADER_RESOURCE) ==
               XG_TILE_MODE_2D_THIN_64KB_S);

    uint64_t xg_calls = 0;
    OS_REQUIRE(OpenScarlettXgGetTelemetryEntry("XGTileSurface2D", &xg_calls) == 1 && xg_calls >= 8);
    OS_REQUIRE(OpenScarlettXgGetTelemetryEntry("XGUntileSurface2D", &xg_calls) == 1 && xg_calls >= 8);
}

// ---------------------------------------------------------------------------
// 5. Live GPU FSR 1.0 (EASU + RCAS) on BOTH GTX 1650 Ti AND WARP
// ---------------------------------------------------------------------------
void TestLiveGpuFsr1(bool has_hw_gpu, bool expect_non_intel) {
    std::printf("[Test 5] Live GPU FSR 1.0 (EASU + RCAS) 64x64 -> 128x128 on HW & WARP...\n");

    constexpr uint32_t kInW = 64u;
    constexpr uint32_t kInH = 64u;
    constexpr uint32_t kOutW = 128u;
    constexpr uint32_t kOutH = 128u;

    // Build sharp checkerboard + diagonal edge test pattern
    std::vector<uint8_t> input(static_cast<size_t>(kInW) * kInH * 4u);
    for (uint32_t y = 0; y < kInH; ++y) {
        for (uint32_t x = 0; x < kInW; ++x) {
            uint8_t* p = &input[(static_cast<size_t>(y) * kInW + x) * 4u];
            const bool checker = (((x / 8u) + (y / 8u)) & 1u) != 0u;
            const bool diag = (x + y) >= kInW;
            if (diag) {
                p[0] = checker ? 240u : 20u;
                p[1] = checker ? 240u : 30u;
                p[2] = checker ? 240u : 40u;
            } else {
                p[0] = checker ? 230u : 15u;
                p[1] = checker ? 60u : 220u;
                p[2] = checker ? 30u : 230u;
            }
            p[3] = 255u;
        }
    }

    openscarlett::Fsr1DispatchParams params{};
    params.input_rgba8 = input.data();
    params.input_width = kInW;
    params.input_height = kInH;
    params.output_width = kOutW;
    params.output_height = kOutH;
    params.sharpness = 0.8f;
    params.enable_rcas = true;

    openscarlett::FfxGpuFsrPipeline warp_fsr;
    OS_REQUIRE(warp_fsr.Initialize(/*prefer_warp=*/true).ok());
    OS_REQUIRE(warp_fsr.AdapterInfo().is_warp);

    openscarlett::FrameBufferRgba8 warp_out;
    openscarlett::Fsr1DispatchStats warp_stats{};
    OS_REQUIRE(warp_fsr.Dispatch(params, warp_out, &warp_stats).ok());
    OS_REQUIRE(warp_out.width == kOutW && warp_out.height == kOutH);
    OS_REQUIRE(warp_stats.output_fnv1a_hash != 0ull);

    openscarlett::FrameBufferRgba8 ref_gpu_out = warp_out;
    int max_abs_diff = 0;

    if (has_hw_gpu) {
        openscarlett::FfxGpuFsrPipeline hw_fsr;
        OS_REQUIRE(hw_fsr.Initialize(/*prefer_warp=*/false).ok());
        OS_REQUIRE(!hw_fsr.AdapterInfo().is_warp);
        if (expect_non_intel) {
            OS_REQUIRE(hw_fsr.AdapterInfo().description.find("Intel") == std::string::npos);
        }

        openscarlett::FrameBufferRgba8 hw_out;
        openscarlett::Fsr1DispatchStats hw_stats{};
        OS_REQUIRE(hw_fsr.Dispatch(params, hw_out, &hw_stats).ok());
        OS_REQUIRE(hw_out.width == kOutW && hw_out.height == kOutH);
        OS_REQUIRE(hw_out.pixels.size() == static_cast<size_t>(kOutW) * kOutH * 4u);
        OS_REQUIRE(hw_stats.output_fnv1a_hash != 0ull);

        for (size_t i = 0; i < hw_out.pixels.size(); ++i) {
            const int d = std::abs(static_cast<int>(hw_out.pixels[i]) - static_cast<int>(warp_out.pixels[i]));
            if (d > max_abs_diff) max_abs_diff = d;
        }
        OS_REQUIRE(max_abs_diff <= 2);
        ref_gpu_out = hw_out;

        std::printf(
            "  FSR1 HW     : %s | EASU=%zu B RCAS=%zu B | FNV1a=0x%016llX\n",
            hw_stats.adapter_description.c_str(),
            hw_stats.easu_dxbc_bytes,
            hw_stats.rcas_dxbc_bytes,
            static_cast<unsigned long long>(hw_stats.output_fnv1a_hash));
    }

    // Verify edge contrast is preserved and stronger than or comparable to bilinear
    openscarlett::FrameBufferRgba8 bilinear_out;
    OS_REQUIRE(openscarlett::FfxGpuFsrPipeline::UpscaleBilinearCpu(
        input.data(), kInW, kInH, kOutW, kOutH, bilinear_out).ok());

    auto compute_mean_edge_gradient = [](const openscarlett::FrameBufferRgba8& fb) -> double {
        double sum = 0.0;
        uint64_t count = 0;
        for (uint32_t y = 1; y + 1 < fb.height; ++y) {
            for (uint32_t x = 1; x + 1 < fb.width; ++x) {
                const size_t c = (static_cast<size_t>(y) * fb.width + x) * 4u;
                const size_t r = (static_cast<size_t>(y) * fb.width + (x + 1u)) * 4u;
                const size_t b = (static_cast<size_t>(y + 1u) * fb.width + x) * 4u;
                const int gx = std::abs(static_cast<int>(fb.pixels[r]) - static_cast<int>(fb.pixels[c]));
                const int gy = std::abs(static_cast<int>(fb.pixels[b]) - static_cast<int>(fb.pixels[c]));
                sum += static_cast<double>(gx + gy);
                ++count;
            }
        }
        return sum / static_cast<double>(count);
    };

    const double fsr_grad = compute_mean_edge_gradient(ref_gpu_out);
    const double bil_grad = compute_mean_edge_gradient(bilinear_out);
    OS_REQUIRE(fsr_grad > bil_grad);

    std::printf(
        "  FSR1 WARP   : %s | FNV1a=0x%016llX | EdgeGrad=%.2f (Bilinear=%.2f) | MaxDiff=%d LSB\n",
        warp_stats.adapter_description.c_str(),
        static_cast<unsigned long long>(warp_stats.output_fnv1a_hash),
        fsr_grad,
        bil_grad,
        max_abs_diff);
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    openscarlett::D3D12AdapterInfo hw_info{};
    const bool has_hw_gpu = TestAdapterSelection(hw_info);
    const bool has_hw_mesh = has_hw_gpu && hw_info.mesh_shader_tier >= 10u;
    const bool expect_non_intel =
        has_hw_gpu && hw_info.description.find("Intel") == std::string::npos;

    const uint64_t warp_clear_hash = RunClearAndPresentXTest(/*use_warp=*/true, false);
    if (has_hw_gpu) {
        const uint64_t hw_clear_hash = RunClearAndPresentXTest(/*use_warp=*/false, has_hw_mesh);
        OS_REQUIRE(hw_clear_hash == warp_clear_hash);
        std::printf("  Clear+PresentX HW vs WARP hash match: 0x%016llX\n",
                    static_cast<unsigned long long>(hw_clear_hash));
        if (has_hw_mesh) {
            TestLiveHardwareMeshShader();
        } else {
            std::printf("  [SKIP] Hardware Mesh Shader test (HW adapter %s has MeshTier=%u < 10)\n",
                        hw_info.description.c_str(), hw_info.mesh_shader_tier);
        }
    } else {
        std::printf("  [SKIP] Hardware Mesh Shader test (no HW D3D12 GPU on runner)\n");
    }

    TestXgSwizzle();
    TestLiveGpuFsr1(has_hw_gpu, expect_non_intel);

    std::printf("ALL d3d12_xs / xg_xs / MeshShader / FSR1 LIVE GPU TESTS PASSED!\n");
    return 0;
#else
    std::printf("Skipping Windows D3D12 live GPU tests on non-Windows host.\n");
    return 0;
#endif
}
