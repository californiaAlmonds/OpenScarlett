#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "os_check.h"

#if defined(_WIN32)
#if defined(_MSC_VER)
#define TEST_COM_IID(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define TEST_COM_IID(T) __mingw_uuidof<T>()
#else
#define TEST_COM_IID(T) IID_##T
#endif
#endif

namespace {

#if defined(_WIN32)

// Matching COM inspection interfaces exposed by d3d12_xs proxies (no extra DLL exports needed).
enum class D3D12XProxyKind : uint32_t {
    Unknown = 0u,
    Resource = 1u,
    Heap = 2u,
    DescriptorHeap = 3u,
    PipelineState = 4u,
    RootSignature = 5u,
    Fence = 6u,
    CommandAllocator = 7u,
    QueryHeap = 8u,
    CommandSignature = 9u,
    StateObject = 10u,
};

inline constexpr GUID kIID_IOpenScarlettProxyInspect = {
    0x4F534431u, 0x3258u, 0x5052u, {0x58u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u}};

inline constexpr GUID kIID_IOpenScarlettCommandListExtState = {
    0x4F534431u, 0x3258u, 0x434Cu, {0x45u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x11u}};

inline constexpr GUID kIID_ID3D12ResourceX = {
    0x4F534431u, 0x3258u, 0x5245u, {0x53u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x21u}};
inline constexpr GUID kIID_ID3D12HeapX = {
    0x4F534431u, 0x3258u, 0x4845u, {0x41u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x22u}};
inline constexpr GUID kIID_ID3D12DescriptorHeapX = {
    0x4F534431u, 0x3258u, 0x4448u, {0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x23u}};
inline constexpr GUID kIID_ID3D12PipelineStateX = {
    0x4F534431u, 0x3258u, 0x5053u, {0x4Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x24u}};
inline constexpr GUID kIID_ID3D12RootSignatureX = {
    0x4F534431u, 0x3258u, 0x5253u, {0x47u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x25u}};
inline constexpr GUID kIID_ID3D12FenceX = {
    0x4F534431u, 0x3258u, 0x464Eu, {0x43u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x26u}};
inline constexpr GUID kIID_ID3D12CommandAllocatorX = {
    0x4F534431u, 0x3258u, 0x4341u, {0x4Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x27u}};
inline constexpr GUID kIID_ID3D12QueryHeapX = {
    0x4F534431u, 0x3258u, 0x5148u, {0x50u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x28u}};
inline constexpr GUID kIID_ID3D12CommandSignatureX = {
    0x4F534431u, 0x3258u, 0x4353u, {0x47u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x29u}};
inline constexpr GUID kIID_ID3D12StateObjectX = {
    0x4F534431u, 0x3258u, 0x5354u, {0x4Fu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x2Au}};

struct IOpenScarlettProxyInspect : public IUnknown {
    virtual D3D12XProxyKind STDMETHODCALLTYPE GetProxyKind() const noexcept = 0;
    virtual IUnknown* STDMETHODCALLTYPE GetUnderlyingHostObject() const noexcept = 0;
    virtual uint64_t STDMETHODCALLTYPE GetTrackedXboxGpuVa() const noexcept = 0;
};

struct D3D12XBOX_CMDLIST_EXT_STATE {
    float depth_bounds_min = 0.0f;
    float depth_bounds_max = 1.0f;
    uint32_t view_instance_mask = 0u;
    uint32_t sample_positions_per_pixel = 0u;
    uint32_t sample_positions_pixels = 0u;
    uint64_t write_buffer_immediate_calls = 0ull;
    uint32_t host_supports_depth_bounds = 0u;
    uint32_t host_supports_view_instancing = 0u;
    uint32_t host_supports_sample_positions = 0u;
};

struct IOpenScarlettCommandListExtState : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCommandListExtState(
        D3D12XBOX_CMDLIST_EXT_STATE* out_state) const noexcept = 0;
};

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

bool IsHardwareAdapter(IDXGIAdapter1* adapter) noexcept {
    if (!adapter) return false;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) return false;
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return false;
    if (desc.VendorId == 0x1414u &&
        (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0u)) {
        return false;
    }
    return true;
}

bool SelectHardwareAdapter(ComPtr<IDXGIAdapter1>& out_adapter, std::string& out_name) {
    out_adapter.Reset();
    out_name.clear();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0u, TEST_COM_IID(IDXGIFactory4), reinterpret_cast<void**>(factory.Put())))) {
        return false;
    }

    ComPtr<IDXGIFactory6> factory6;
    factory->QueryInterface(TEST_COM_IID(IDXGIFactory6), reinterpret_cast<void**>(factory6.Put()));

    for (UINT i = 0u; ; ++i) {
        ComPtr<IDXGIAdapter1> cand;
        HRESULT hr = E_FAIL;
        if (factory6) {
            hr = factory6->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                TEST_COM_IID(IDXGIAdapter1), reinterpret_cast<void**>(cand.Put()));
        } else {
            hr = factory->EnumAdapters1(i, cand.Put());
        }
        if (FAILED(hr) || !cand) break;
        if (!IsHardwareAdapter(cand.Get())) continue;

        // Verify D3D12 device creation works on this HW adapter.
        ComPtr<ID3D12Device> probe;
        if (SUCCEEDED(D3D12CreateDevice(
                cand.Get(), D3D_FEATURE_LEVEL_12_0,
                TEST_COM_IID(ID3D12Device), reinterpret_cast<void**>(probe.Put()))) &&
            probe) {
            DXGI_ADAPTER_DESC1 desc{};
            cand->GetDesc1(&desc);
            char name_buf[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
            out_name = name_buf;
            out_adapter.Reset(cand.Get());
            out_adapter->AddRef();
            return true;
        }
    }
    return false;
}

void CreateDeviceForMode(
    bool use_warp,
    IDXGIAdapter1* hw_adapter,
    ComPtr<ID3D12DeviceX>& out_dev) {
    OpenScarlettD3D12SetBackendMode(D3D12XBOX_BACKEND_D3D12);
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;

    HRESULT hr = D3D12XboxCreateDevice(
        use_warp ? nullptr : hw_adapter,
        &params,
        IID_ID3D12DeviceX,
        reinterpret_cast<void**>(out_dev.Put()));
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(out_dev.Get() != nullptr);
}

void WaitQueueFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) {
    OS_REQUIRE(SUCCEEDED(queue->Signal(fence, value)));
    if (fence->GetCompletedValue() < value) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(ev != nullptr);
        OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(value, ev)));
        OS_REQUIRE(WaitForSingleObject(ev, 20000u) == WAIT_OBJECT_0);
        CloseHandle(ev);
    }
}

bool CompileCs50(const char* hlsl, const char* entry, ComPtr<ID3DBlob>& out_blob) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
        entry, "cs_5_0", 0u, 0u, out_blob.Put(), err.Put());
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "D3DCompile CS failed: %.*s\n",
                         static_cast<int>(err->GetBufferSize()),
                         static_cast<const char*>(err->GetBufferPointer()));
        }
        return false;
    }
    return out_blob.Get() != nullptr;
}

std::string FindDxcExecutable() {
    std::error_code ec;
    char env_dxc[1024]{};
    const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
    if (env_len > 0u && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, ec)) {
        return std::string(env_dxc);
    }
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0u && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf);
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

bool CompileDxilWithDxc(
    const std::string& dxc_exe,
    const char* hlsl,
    const char* profile,
    std::vector<uint8_t>& out_dxil) {
    if (dxc_exe.empty()) return false;
    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path(ec) / "openscarlett_proxies_test";
    std::filesystem::create_directories(scratch, ec);

    const std::filesystem::path hlsl_path = scratch / "dxr_probe.hlsl";
    const std::filesystem::path dxil_path = scratch / "dxr_probe.dxil";
    {
        std::ofstream ofs(hlsl_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(hlsl, static_cast<std::streamsize>(std::strlen(hlsl)));
    }

    std::string cmd = "\"" + dxc_exe + "\" -T " + profile +
                      " -Fo \"" + dxil_path.string() + "\" \"" + hlsl_path.string() + "\"";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000u);
    DWORD exit_code = 1u;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0u) return false;

    std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz <= 0) return false;
    out_dxil.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);
    return ifs.good() || ifs.eof();
}

void VerifyCommonProxyContract(
    ID3D12DeviceChild* obj,
    ID3D12DeviceX* expected_dev,
    D3D12XProxyKind expected_kind,
    REFIID standard_iid,
    REFIID xbox_iid,
    bool expect_pageable,
    bool require_host_object = true) {
    OS_REQUIRE(obj != nullptr);

    // 1. Proxy inspection interface proves this is a genuine OpenScarlett COM proxy.
    ComPtr<IOpenScarlettProxyInspect> inspect;
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(
        kIID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(inspect.Put()))));
    OS_REQUIRE(inspect.Get() != nullptr);
    OS_REQUIRE(inspect->GetProxyKind() == expected_kind);
    IUnknown* host_obj = inspect->GetUnderlyingHostObject();
    if (require_host_object) {
        OS_REQUIRE(host_obj != nullptr);
    }
    OS_REQUIRE(host_obj != static_cast<IUnknown*>(obj));

    // 2. GetDevice MUST return the wrapped DeviceX, not the raw host ID3D12Device.
    ComPtr<ID3D12DeviceX> child_dev;
    OS_REQUIRE(SUCCEEDED(obj->GetDevice(
        IID_ID3D12DeviceX, reinterpret_cast<void**>(child_dev.Put()))));
    OS_REQUIRE(child_dev.Get() == expected_dev);

    // 3. QueryInterface across base COM hierarchy + standard IID + Xbox IID.
    ComPtr<IUnknown> unk;
    ComPtr<ID3D12Object> d3d_obj;
    ComPtr<ID3D12DeviceChild> dev_child;
    ComPtr<IUnknown> std_iface;
    ComPtr<IUnknown> xbox_iface;
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(TEST_COM_IID(IUnknown), reinterpret_cast<void**>(unk.Put()))));
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(TEST_COM_IID(ID3D12Object), reinterpret_cast<void**>(d3d_obj.Put()))));
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(TEST_COM_IID(ID3D12DeviceChild), reinterpret_cast<void**>(dev_child.Put()))));
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(standard_iid, reinterpret_cast<void**>(std_iface.Put()))));
    OS_REQUIRE(SUCCEEDED(obj->QueryInterface(xbox_iid, reinterpret_cast<void**>(xbox_iface.Put()))));
    OS_REQUIRE(std_iface.Get() == xbox_iface.Get());

    ComPtr<ID3D12Pageable> pageable;
    const HRESULT hr_pageable =
        obj->QueryInterface(TEST_COM_IID(ID3D12Pageable), reinterpret_cast<void**>(pageable.Put()));
    if (expect_pageable) {
        OS_REQUIRE(SUCCEEDED(hr_pageable));
        OS_REQUIRE(pageable.Get() != nullptr);
    } else {
        OS_REQUIRE(FAILED(hr_pageable));
    }

    // 4. AddRef / Release refcount round-trip & ID3D12Object SetName + PrivateData.
    const ULONG refs_up = obj->AddRef();
    const ULONG refs_dn = obj->Release();
    OS_REQUIRE(refs_up == refs_dn + 1u);

    OS_REQUIRE(SUCCEEDED(obj->SetName(L"OpenScarlettProxyTest")));
    const GUID test_guid = {0x12345678u, 0x9ABCu, 0xDEF0u, {1, 2, 3, 4, 5, 6, 7, 8}};
    const uint32_t tag_in = 0x51A7BEEFu;
    OS_REQUIRE(SUCCEEDED(obj->SetPrivateData(test_guid, sizeof(tag_in), &tag_in)));
    uint32_t tag_out = 0u;
    UINT tag_sz = sizeof(tag_out);
    OS_REQUIRE(SUCCEEDED(obj->GetPrivateData(test_guid, &tag_sz, &tag_out)));
    OS_REQUIRE(tag_sz == sizeof(tag_out));
    OS_REQUIRE(tag_out == tag_in);
}

D3D12_RESOURCE_DESC MakeBufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Alignment = 0ull;
    d.Width = bytes;
    d.Height = 1u;
    d.DepthOrArraySize = 1u;
    d.MipLevels = 1u;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1u;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

// ---------------------------------------------------------------------------
// Suite 1: All 10 COM Object Proxies + ExecuteIndirect + DXR StateObjectX
// ---------------------------------------------------------------------------
void RunAllTenProxiesTest(bool use_warp, IDXGIAdapter1* hw_adapter, const char* label) {
    std::printf("[Suite 1 - %s] Testing all 10 Xbox-ABI COM object proxies...\n", label);

    ComPtr<ID3D12DeviceX> dev;
    CreateDeviceForMode(use_warp, hw_adapter, dev);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()))));

    // (1) CommandAllocatorX
    ComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        TEST_COM_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(alloc.Put()))));
    VerifyCommonProxyContract(
        alloc.Get(), dev.Get(), D3D12XProxyKind::CommandAllocator,
        TEST_COM_IID(ID3D12CommandAllocator), kIID_ID3D12CommandAllocatorX, true);

    // (2) HeapX
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 256ull * 1024ull;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateHeap(
        &hd, TEST_COM_IID(ID3D12Heap), reinterpret_cast<void**>(heap.Put()))));
    VerifyCommonProxyContract(
        heap.Get(), dev.Get(), D3D12XProxyKind::Heap,
        TEST_COM_IID(ID3D12Heap), kIID_ID3D12HeapX, true);
    OS_REQUIRE(heap->GetDesc().SizeInBytes == hd.SizeInBytes);

    // (3) ResourceX (committed + placed on HeapX + CreatePlacedResourceX with Xbox GPU VA)
    const D3D12_RESOURCE_DESC uav_desc = MakeBufferDesc(256ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> placed_uav;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResource(
        heap.Get(), 0ull, &uav_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(placed_uav.Put()))));
    VerifyCommonProxyContract(
        placed_uav.Get(), dev.Get(), D3D12XProxyKind::Resource,
        TEST_COM_IID(ID3D12Resource), kIID_ID3D12ResourceX, true);
    OS_REQUIRE(placed_uav->GetDesc().Width == 256ull);
    OS_REQUIRE(placed_uav->GetGPUVirtualAddress() != 0ull);
    D3D12_HEAP_PROPERTIES out_hp{};
    D3D12_HEAP_FLAGS out_hf = D3D12_HEAP_FLAG_NONE;
    OS_REQUIRE(SUCCEEDED(placed_uav->GetHeapProperties(&out_hp, &out_hf)));
    OS_REQUIRE(out_hp.Type == D3D12_HEAP_TYPE_DEFAULT);

    // Verify CreatePlacedResourceX tracks the requested Xbox GPU VA on the proxy!
    const D3D12_GPU_VIRTUAL_ADDRESS kXboxTestVa = 0x0000004000010000ull;
    const D3D12_RESOURCE_DESC xbox_buf_desc = MakeBufferDesc(4096ull);
    ComPtr<ID3D12Resource> placed_x;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
        kXboxTestVa, &xbox_buf_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(placed_x.Put()))));
    {
        ComPtr<IOpenScarlettProxyInspect> insp;
        OS_REQUIRE(SUCCEEDED(placed_x->QueryInterface(
            kIID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(insp.Put()))));
        OS_REQUIRE(insp->GetTrackedXboxGpuVa() == kXboxTestVa);
    }

    // (4) DescriptorHeapX
    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.NumDescriptors = 8u;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> desc_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(
        &dhd, TEST_COM_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(desc_heap.Put()))));
    VerifyCommonProxyContract(
        desc_heap.Get(), dev.Get(), D3D12XProxyKind::DescriptorHeap,
        TEST_COM_IID(ID3D12DescriptorHeap), kIID_ID3D12DescriptorHeapX, true);
    OS_REQUIRE(desc_heap->GetDesc().NumDescriptors == 8u);
    OS_REQUIRE(desc_heap->GetCPUDescriptorHandleForHeapStart().ptr != 0u);
    OS_REQUIRE(desc_heap->GetGPUDescriptorHandleForHeapStart().ptr != 0ull);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view{};
    uav_view.Format = DXGI_FORMAT_R32_TYPELESS;
    uav_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_view.Buffer.NumElements = 64u;
    uav_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(
        placed_uav.Get(), nullptr, &uav_view, desc_heap->GetCPUDescriptorHandleForHeapStart());

    // (5) RootSignatureX
    D3D12_ROOT_PARAMETER root_params[1]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_params[0].Descriptor.ShaderRegister = 0u;
    root_params[0].Descriptor.RegisterSpace = 0u;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1u;
    rs_desc.pParameters = root_params;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3DBlob> rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    ComPtr<ID3D12RootSignature> root_sig;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        TEST_COM_IID(ID3D12RootSignature), reinterpret_cast<void**>(root_sig.Put()))));
    VerifyCommonProxyContract(
        root_sig.Get(), dev.Get(), D3D12XProxyKind::RootSignature,
        TEST_COM_IID(ID3D12RootSignature), kIID_ID3D12RootSignatureX, /*expect_pageable=*/false);

    // (6) PipelineStateX (compute shader that writes 0x51A70000 | tid via ExecuteIndirect)
    static const char* kIndirectCs = R"(
        RWByteAddressBuffer gOut : register(u0);
        [numthreads(8, 1, 1)]
        void CSMain(uint3 tid : SV_DispatchThreadID) {
            gOut.Store(tid.x * 4, 0x51A70000u | tid.x);
        }
    )";
    ComPtr<ID3DBlob> cs_blob;
    OS_REQUIRE(CompileCs50(kIndirectCs, "CSMain", cs_blob));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso_desc{};
    cpso_desc.pRootSignature = root_sig.Get();
    cpso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
    cpso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(
        &cpso_desc, TEST_COM_IID(ID3D12PipelineState), reinterpret_cast<void**>(pso.Put()))));
    VerifyCommonProxyContract(
        pso.Get(), dev.Get(), D3D12XProxyKind::PipelineState,
        TEST_COM_IID(ID3D12PipelineState), kIID_ID3D12PipelineStateX, true);

    // (7) CommandSignatureX (Dispatch indirect arguments)
    D3D12_INDIRECT_ARGUMENT_DESC arg_desc{};
    arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sig_desc.NumArgumentDescs = 1u;
    sig_desc.pArgumentDescs = &arg_desc;
    ComPtr<ID3D12CommandSignature> cmd_sig;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandSignature(
        &sig_desc, nullptr,
        TEST_COM_IID(ID3D12CommandSignature), reinterpret_cast<void**>(cmd_sig.Put()))));
    VerifyCommonProxyContract(
        cmd_sig.Get(), dev.Get(), D3D12XProxyKind::CommandSignature,
        TEST_COM_IID(ID3D12CommandSignature), kIID_ID3D12CommandSignatureX, true);

    // (8) QueryHeapX (Timestamp queries)
    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2u;
    ComPtr<ID3D12QueryHeap> query_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateQueryHeap(
        &qhd, TEST_COM_IID(ID3D12QueryHeap), reinterpret_cast<void**>(query_heap.Put()))));
    VerifyCommonProxyContract(
        query_heap.Get(), dev.Get(), D3D12XProxyKind::QueryHeap,
        TEST_COM_IID(ID3D12QueryHeap), kIID_ID3D12QueryHeapX, true);

    // (9) FenceX
    ComPtr<ID3D12Fence> fence;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(
        0ull, D3D12_FENCE_FLAG_NONE,
        TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(fence.Put()))));
    VerifyCommonProxyContract(
        fence.Get(), dev.Get(), D3D12XProxyKind::Fence,
        TEST_COM_IID(ID3D12Fence), kIID_ID3D12FenceX, true);
    OS_REQUIRE(SUCCEEDED(fence->Signal(5ull)));
    OS_REQUIRE(fence->GetCompletedValue() == 5ull);

    // Create argument buffer + readback buffer + timestamp readback buffer (all ResourceX proxies)
    D3D12_HEAP_PROPERTIES up_hp{};
    up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rb_hp{};
    rb_hp.Type = D3D12_HEAP_TYPE_READBACK;

    const D3D12_RESOURCE_DESC arg_buf_desc = MakeBufferDesc(256ull);
    ComPtr<ID3D12Resource> arg_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &up_hp, D3D12_HEAP_FLAG_NONE, &arg_buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(arg_buf.Put()))));
    {
        void* mapped = nullptr;
        D3D12_RANGE rnone{0u, 0u};
        OS_REQUIRE(SUCCEEDED(arg_buf->Map(0u, &rnone, &mapped)));
        D3D12_DISPATCH_ARGUMENTS dargs{2u, 1u, 1u};  // 16 threads
        std::memcpy(mapped, &dargs, sizeof(dargs));
        arg_buf->Unmap(0u, nullptr);
    }

    const D3D12_RESOURCE_DESC rb_desc = MakeBufferDesc(256ull);
    ComPtr<ID3D12Resource> rb_buf;
    ComPtr<ID3D12Resource> ts_rb_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(rb_buf.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(ts_rb_buf.Put()))));

    // Also verify MakeResident / Evict on mixed proxy pageables (HeapX + DescriptorHeapX)
    ID3D12Pageable* pageables[] = {heap.Get(), desc_heap.Get()};
    OS_REQUIRE(SUCCEEDED(dev->MakeResident(2u, pageables)));

    // Execute live GPU ExecuteIndirect + Timestamp Query using ALL wrapped proxies!
    ComPtr<ID3D12GraphicsCommandListX> cl;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(),
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cl.Put()))));

    ID3D12DescriptorHeap* heaps[] = {desc_heap.Get()};
    cl->SetDescriptorHeaps(1u, heaps);
    cl->SetComputeRootSignature(root_sig.Get());
    cl->SetComputeRootUnorderedAccessView(0u, placed_uav->GetGPUVirtualAddress());
    cl->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u);
    cl->ExecuteIndirect(cmd_sig.Get(), 1u, arg_buf.Get(), 0ull, nullptr, 0ull);
    cl->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1u);

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = placed_uav.Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1u, &bar);

    cl->CopyBufferRegion(rb_buf.Get(), 0ull, placed_uav.Get(), 0ull, 64ull);
    cl->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0u, 2u, ts_rb_buf.Get(), 0ull);
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1u, lists);
    WaitQueueFence(queue.Get(), fence.Get(), 10ull);
    OS_REQUIRE(fence->GetCompletedValue() >= 10ull);

    // Verify ExecuteIndirect GPU output!
    {
        void* mapped = nullptr;
        D3D12_RANGE rread{0u, 64u};
        OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rread, &mapped)));
        const auto* out_u32 = static_cast<const uint32_t*>(mapped);
        for (uint32_t i = 0u; i < 16u; ++i) {
            OS_REQUIRE(out_u32[i] == (0x51A70000u | i));
        }
        D3D12_RANGE wnone{0u, 0u};
        rb_buf->Unmap(0u, &wnone);
    }

    // Verify SetEventOnMultipleFenceCompletion with two wrapped FenceX proxies!
    ComPtr<ID3D12Fence> fence2;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(
        0ull, D3D12_FENCE_FLAG_NONE,
        TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(fence2.Put()))));
    OS_REQUIRE(SUCCEEDED(fence2->Signal(42ull)));
    {
        HANDLE multi_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(multi_ev != nullptr);
        ID3D12Fence* fences[2] = {fence.Get(), fence2.Get()};
        UINT64 vals[2] = {10ull, 42ull};
        OS_REQUIRE(SUCCEEDED(dev->SetEventOnMultipleFenceCompletion(
            fences, vals, 2u, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, multi_ev)));
        OS_REQUIRE(WaitForSingleObject(multi_ev, 5000u) == WAIT_OBJECT_0);
        CloseHandle(multi_ev);
    }

    // Verify seamless mixed raw-host + wrapped ResourceX interop in CopyBufferRegion!
    {
        ComPtr<IOpenScarlettProxyInspect> insp;
        OS_REQUIRE(SUCCEEDED(rb_buf->QueryInterface(
            kIID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(insp.Put()))));
        auto* raw_host_rb = static_cast<ID3D12Resource*>(insp->GetUnderlyingHostObject());
        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), nullptr)));
        // Pass wrapped source + raw host destination!
        cl->CopyBufferRegion(raw_host_rb, 64ull, placed_uav.Get(), 0ull, 64ull);
        OS_REQUIRE(SUCCEEDED(cl->Close()));
        ID3D12CommandList* mix_lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1u, mix_lists);
        WaitQueueFence(queue.Get(), fence.Get(), 11ull);

        void* mapped = nullptr;
        D3D12_RANGE rread{64u, 128u};
        OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rread, &mapped)));
        const auto* out_u32 = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + 64u);
        for (uint32_t i = 0u; i < 16u; ++i) {
            OS_REQUIRE(out_u32[i] == (0x51A70000u | i));
        }
        D3D12_RANGE wnone{0u, 0u};
        rb_buf->Unmap(0u, &wnone);
    }

    // (10) StateObjectX (+ ID3D12StateObjectProperties QI)
    // Verified on BOTH HW DXR adapters (real host ID3D12StateObject) AND non-DXR adapters
    // (GTX 1650 Ti / Win10 WARP via software StateObjectXProxy fallback)!
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    const bool has_hw_dxr =
        SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))) &&
        opts5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    const std::string dxc_exe = FindDxcExecutable();
    std::vector<uint8_t> dxil;
    if (!dxc_exe.empty()) {
        static const char* kDxrLib = R"(
            RWByteAddressBuffer gOut : register(u0);
            [shader("raygeneration")]
            void MyRaygen() {
                gOut.Store(0, 0xD3D12010u);
            }
        )";
        CompileDxilWithDxc(dxc_exe, kDxrLib, "lib_6_3", dxil);
    }
    if (dxil.empty()) {
        // Minimal non-empty placeholder bytecode for software-fallback StateObjectXProxy verification
        // when dxc.exe is not present in a minimal CI image.
        dxil.assign(64u, 0x42u);
    }

    D3D12_EXPORT_DESC exp_desc{};
    exp_desc.Name = L"MyRaygen";
    D3D12_DXIL_LIBRARY_DESC lib_desc{};
    lib_desc.DXILLibrary.pShaderBytecode = dxil.data();
    lib_desc.DXILLibrary.BytecodeLength = dxil.size();
    lib_desc.NumExports = 1u;
    lib_desc.pExports = &exp_desc;

    D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
    shader_cfg.MaxPayloadSizeInBytes = 16u;
    shader_cfg.MaxAttributeSizeInBytes = 8u;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipe_cfg{};
    pipe_cfg.MaxTraceRecursionDepth = 1u;

    D3D12_GLOBAL_ROOT_SIGNATURE global_rs{};
    global_rs.pGlobalRootSignature = root_sig.Get();  // Wrapped RootSignatureX!

    D3D12_STATE_SUBOBJECT subs[4]{};
    subs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subs[0].pDesc = &lib_desc;
    subs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subs[1].pDesc = &shader_cfg;
    subs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subs[2].pDesc = &pipe_cfg;
    subs[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subs[3].pDesc = &global_rs;

    D3D12_STATE_OBJECT_DESC so_desc{};
    so_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    so_desc.NumSubobjects = 4u;
    so_desc.pSubobjects = subs;

    ComPtr<ID3D12StateObject> state_obj;
    const HRESULT so_hr = dev->CreateStateObject(
        &so_desc, TEST_COM_IID(ID3D12StateObject), reinterpret_cast<void**>(state_obj.Put()));
    OS_REQUIRE(SUCCEEDED(so_hr));
    OS_REQUIRE(state_obj.Get() != nullptr);

    VerifyCommonProxyContract(
        state_obj.Get(), dev.Get(), D3D12XProxyKind::StateObject,
        TEST_COM_IID(ID3D12StateObject), kIID_ID3D12StateObjectX,
        /*expect_pageable=*/true,
        /*require_host_object=*/has_hw_dxr);

    // Verify ID3D12StateObjectProperties QI + GetShaderIdentifier + GetShaderStackSize + PipelineStackSize!
    ComPtr<ID3D12StateObjectProperties> props;
    OS_REQUIRE(SUCCEEDED(state_obj->QueryInterface(
        TEST_COM_IID(ID3D12StateObjectProperties),
        reinterpret_cast<void**>(props.Put()))));
    OS_REQUIRE(props.Get() != nullptr);
    void* shader_id = props->GetShaderIdentifier(L"MyRaygen");
    OS_REQUIRE(shader_id != nullptr);
    const UINT64 raygen_stack = props->GetShaderStackSize(L"MyRaygen");
    // Host DXR (e.g. Windows Server WARP Tier 1.1) validly returns 0 for an empty
    // non-recursive raygen shader, and UINT64_MAX only for unknown exports; our SW proxy returns 256.
    if (has_hw_dxr) {
        OS_REQUIRE(raygen_stack != UINT64_MAX);
    } else {
        OS_REQUIRE(raygen_stack > 0ull);
    }
    const UINT64 orig_stack = props->GetPipelineStackSize();
    props->SetPipelineStackSize(orig_stack + 256ull);
    OS_REQUIRE(props->GetPipelineStackSize() >= orig_stack);

    // Also verify CommandListX::SetPipelineState1 unwraps StateObjectX cleanly!
    OS_REQUIRE(SUCCEEDED(alloc->Reset()));
    OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), nullptr)));
    cl->SetPipelineState1(state_obj.Get());
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    std::printf(
        "  [PASS - %s] All 10 proxies verified (ExecuteIndirect=OK, StateObjectX+Props=%s)\n",
        label, has_hw_dxr ? "HOST-DXR" : "SW-DXR-PROXY");
}

// ---------------------------------------------------------------------------
// Suite 2: Xbox D3D12 Extensions
//   WriteBufferImmediate, SetViewInstanceMask, OMSetDepthBounds,
//   SetSamplePositions, GetCopyableFootprints (standard + host-refusal parity)
// ---------------------------------------------------------------------------
void RunXboxExtensionsTest(bool use_warp, IDXGIAdapter1* hw_adapter, const char* label) {
    std::printf("[Suite 2 - %s] Testing Xbox D3D12 extensions...\n", label);

    OpenScarlettD3D12ResetTelemetry();
    ComPtr<ID3D12DeviceX> dev;
    CreateDeviceForMode(use_warp, hw_adapter, dev);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueueX> queue;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(queue.Put()))));

    ComPtr<ID3D12CommandAllocator> alloc;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        TEST_COM_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(alloc.Put()))));
    ComPtr<ID3D12GraphicsCommandListX> cl;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cl.Put()))));
    ComPtr<ID3D12Fence> fence;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(
        0ull, D3D12_FENCE_FLAG_NONE,
        TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(fence.Put()))));

    // 2A. WriteBufferImmediate live GPU verification
    D3D12_HEAP_PROPERTIES def_hp{};
    def_hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES rb_hp{};
    rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC buf_desc = MakeBufferDesc(256ull);

    ComPtr<ID3D12Resource> gpu_buf;
    ComPtr<ID3D12Resource> rb_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_hp, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(gpu_buf.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_hp, D3D12_HEAP_FLAG_NONE, &buf_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(rb_buf.Put()))));

    const D3D12_GPU_VIRTUAL_ADDRESS base_va = gpu_buf->GetGPUVirtualAddress();
    const uint32_t kExpectedWbi[4] = {0xDEADBEEFu, 0xCAFEBABEu, 0x1337C0DEu, 0xA5A55A5Au};
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wbi_params[4]{};
    D3D12_WRITEBUFFERIMMEDIATE_MODE wbi_modes[4] = {
        D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
    };
    for (UINT i = 0u; i < 4u; ++i) {
        wbi_params[i].Dest = base_va + i * 4ull;
        wbi_params[i].Value = kExpectedWbi[i];
    }
    cl->WriteBufferImmediate(4u, wbi_params, wbi_modes);

    // 2B. OMSetDepthBounds, SetViewInstanceMask, SetSamplePositions
    cl->OMSetDepthBounds(0.15f, 0.85f);
    cl->SetViewInstanceMask(0x3u);
    D3D12_SAMPLE_POSITION sp{};
    sp.X = 2;
    sp.Y = -2;
    cl->SetSamplePositions(1u, 1u, &sp);

    // Inspect tracked state via COM extension interface before Reset
    {
        ComPtr<IOpenScarlettCommandListExtState> ext;
        OS_REQUIRE(SUCCEEDED(cl->QueryInterface(
            kIID_IOpenScarlettCommandListExtState, reinterpret_cast<void**>(ext.Put()))));
        D3D12XBOX_CMDLIST_EXT_STATE st{};
        OS_REQUIRE(SUCCEEDED(ext->GetCommandListExtState(&st)));
        OS_REQUIRE(std::fabs(st.depth_bounds_min - 0.15f) < 1e-5f);
        OS_REQUIRE(std::fabs(st.depth_bounds_max - 0.85f) < 1e-5f);
        OS_REQUIRE(st.view_instance_mask == 0x3u);
        OS_REQUIRE(st.sample_positions_per_pixel == 1u);
        OS_REQUIRE(st.sample_positions_pixels == 1u);
        OS_REQUIRE(st.write_buffer_immediate_calls == 1ull);
    }

    // Reset sample positions back to default (0,0,nullptr) before copy + submit
    cl->SetSamplePositions(0u, 0u, nullptr);

    D3D12_RESOURCE_BARRIER to_src{};
    to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_src.Transition.pResource = gpu_buf.Get();
    to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1u, &to_src);

    cl->CopyBufferRegion(rb_buf.Get(), 0ull, gpu_buf.Get(), 0ull, 16ull);
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1u, lists);
    WaitQueueFence(queue.Get(), fence.Get(), 1ull);

    {
        void* mapped = nullptr;
        D3D12_RANGE rread{0u, 16u};
        OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rread, &mapped)));
        const auto* got = static_cast<const uint32_t*>(mapped);
        for (UINT i = 0u; i < 4u; ++i) {
            OS_REQUIRE(got[i] == kExpectedWbi[i]);
        }
        D3D12_RANGE wnone{0u, 0u};
        rb_buf->Unmap(0u, &wnone);
    }

    // Verify telemetry recorded zero errors for all 4 extension methods!
    const char* ext_syms[4] = {
        "ID3D12GraphicsCommandListX::WriteBufferImmediate",
        "ID3D12GraphicsCommandListX::OMSetDepthBounds",
        "ID3D12GraphicsCommandListX::SetViewInstanceMask",
        "ID3D12GraphicsCommandListX::SetSamplePositions",
    };
    for (const char* sym : ext_syms) {
        int32_t slot = -1;
        uint64_t total = 0ull, ok = 0ull, not_impl = 0ull, errs = 0ull;
        int32_t last_hr = -1;
        OS_REQUIRE(OpenScarlettD3D12GetTelemetryDetail(
                       sym, &slot, &total, &ok, &not_impl, &errs, &last_hr) == 1);
        OS_REQUIRE(total >= 1ull);
        OS_REQUIRE(ok >= 1ull);
        OS_REQUIRE(errs == 0ull);
        OS_REQUIRE(last_hr == S_OK);
    }

    // Verify Reset clears tracked extension state cleanly.
    OS_REQUIRE(SUCCEEDED(alloc->Reset()));
    OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), nullptr)));
    {
        ComPtr<IOpenScarlettCommandListExtState> ext;
        OS_REQUIRE(SUCCEEDED(cl->QueryInterface(
            kIID_IOpenScarlettCommandListExtState, reinterpret_cast<void**>(ext.Put()))));
        D3D12XBOX_CMDLIST_EXT_STATE st{};
        OS_REQUIRE(SUCCEEDED(ext->GetCommandListExtState(&st)));
        OS_REQUIRE(std::fabs(st.depth_bounds_min - 0.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(st.depth_bounds_max - 1.0f) < 1e-5f);
        OS_REQUIRE(st.view_instance_mask == 0u);
    }
    OS_REQUIRE(SUCCEEDED(cl->Close()));

    // 2C. GetCopyableFootprints standard byte-for-byte parity vs raw host device + Xbox host-refusal parity!
    ComPtr<ID3D12Device> raw_host_dev;
    {
        ComPtr<IOpenScarlettProxyInspect> insp;
        OS_REQUIRE(SUCCEEDED(gpu_buf->QueryInterface(
            kIID_IOpenScarlettProxyInspect, reinterpret_cast<void**>(insp.Put()))));
        auto* raw_res = static_cast<ID3D12Resource*>(insp->GetUnderlyingHostObject());
        OS_REQUIRE(SUCCEEDED(raw_res->GetDevice(
            TEST_COM_IID(ID3D12Device), reinterpret_cast<void**>(raw_host_dev.Put()))));
    }

    auto verify_standard_parity = [&](const D3D12_RESOURCE_DESC& d, UINT num_subs, UINT64 base_off) {
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp_x(num_subs), fp_h(num_subs);
        std::vector<UINT> rows_x(num_subs), rows_h(num_subs);
        std::vector<UINT64> rsz_x(num_subs), rsz_h(num_subs);
        UINT64 tot_x = 0ull, tot_h = 0ull;

        dev->GetCopyableFootprints(&d, 0u, num_subs, base_off, fp_x.data(), rows_x.data(), rsz_x.data(), &tot_x);
        raw_host_dev->GetCopyableFootprints(&d, 0u, num_subs, base_off, fp_h.data(), rows_h.data(), rsz_h.data(), &tot_h);

        OS_REQUIRE(tot_x == tot_h);
        OS_REQUIRE(tot_x > 0ull && tot_x != UINT64_MAX);
        OS_REQUIRE(std::memcmp(fp_x.data(), fp_h.data(), sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) * num_subs) == 0);
        OS_REQUIRE(std::memcmp(rows_x.data(), rows_h.data(), sizeof(UINT) * num_subs) == 0);
        OS_REQUIRE(std::memcmp(rsz_x.data(), rsz_h.data(), sizeof(UINT64) * num_subs) == 0);
    };

    D3D12_RESOURCE_DESC tex2d{};
    tex2d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex2d.Width = 256ull;
    tex2d.Height = 128u;
    tex2d.DepthOrArraySize = 1u;
    tex2d.MipLevels = 4u;
    tex2d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex2d.SampleDesc.Count = 1u;
    verify_standard_parity(tex2d, 4u, 512ull);

    D3D12_RESOURCE_DESC bc7 = tex2d;
    bc7.Width = 512ull;
    bc7.Height = 256u;
    bc7.MipLevels = 3u;
    bc7.Format = DXGI_FORMAT_BC7_UNORM;
    verify_standard_parity(bc7, 3u, 0ull);

    D3D12_RESOURCE_DESC tex3d{};
    tex3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    tex3d.Width = 32ull;
    tex3d.Height = 16u;
    tex3d.DepthOrArraySize = 8u;
    tex3d.MipLevels = 2u;
    tex3d.Format = DXGI_FORMAT_R32_FLOAT;
    tex3d.SampleDesc.Count = 1u;
    verify_standard_parity(tex3d, 2u, 0ull);

    verify_standard_parity(MakeBufferDesc(4096ull), 1u, 0ull);

    // Xbox host-refusal Case 1: Xbox DCC upper flag (0x80000000) on 2D texture
    {
        D3D12_RESOURCE_DESC xbox_flagged = tex2d;
        xbox_flagged.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x80000000u);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_x[4]{}, fp_clean[4]{};
        UINT rows_x[4]{}, rows_clean[4]{};
        UINT64 rsz_x[4]{}, rsz_clean[4]{};
        UINT64 tot_x = 0ull, tot_clean = 0ull;
        dev->GetCopyableFootprints(&xbox_flagged, 0u, 4u, 0ull, fp_x, rows_x, rsz_x, &tot_x);
        raw_host_dev->GetCopyableFootprints(&tex2d, 0u, 4u, 0ull, fp_clean, rows_clean, rsz_clean, &tot_clean);
        OS_REQUIRE(tot_x == tot_clean);
        OS_REQUIRE(std::memcmp(fp_x, fp_clean, sizeof(fp_x)) == 0);
    }

    // Xbox host-refusal Case 2: Exotic/Xbox-only DXGI_FORMAT (200) refused by host with UINT64_MAX
    {
        D3D12_RESOURCE_DESC exotic{};
        exotic.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        exotic.Width = 128ull;
        exotic.Height = 64u;
        exotic.DepthOrArraySize = 1u;
        exotic.MipLevels = 2u;
        exotic.Format = static_cast<DXGI_FORMAT>(200);
        exotic.SampleDesc.Count = 1u;

        // Verify host refuses it with UINT64_MAX
        UINT64 host_refused = 0ull;
        raw_host_dev->GetCopyableFootprints(&exotic, 0u, 2u, 0ull, nullptr, nullptr, nullptr, &host_refused);
        OS_REQUIRE(host_refused == UINT64_MAX);

        // Prefill outputs with 0xCC to prove zero uninitialized bytes remain!
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];
        UINT rows[2];
        UINT64 rsz[2];
        UINT64 tot = 0ull;
        std::memset(fp, 0xCC, sizeof(fp));
        std::memset(rows, 0xCC, sizeof(rows));
        std::memset(rsz, 0xCC, sizeof(rsz));

        dev->GetCopyableFootprints(&exotic, 0u, 2u, 512ull, fp, rows, rsz, &tot);
        OS_REQUIRE(tot != 0ull && tot != UINT64_MAX);
        OS_REQUIRE(fp[0].Offset == 512ull);
        OS_REQUIRE((fp[0].Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0u);
        OS_REQUIRE(fp[0].Footprint.Width == 128u);
        OS_REQUIRE(fp[0].Footprint.Height == 64u);
        OS_REQUIRE(fp[0].Footprint.Depth == 1u);
        OS_REQUIRE(rows[0] == 64u);
        OS_REQUIRE(rsz[0] > 0ull);
        OS_REQUIRE(fp[1].Offset > fp[0].Offset);
        OS_REQUIRE(((fp[1].Offset - 512ull) % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) == 0ull);
        OS_REQUIRE(fp[1].Footprint.Width == 64u);
        OS_REQUIRE(fp[1].Footprint.Height == 32u);
        OS_REQUIRE(rows[1] == 32u);
        OS_REQUIRE(rsz[1] > 0ull);
    }

    std::printf("  [PASS - %s] WriteBufferImmediate + DepthBounds + ViewInstanceMask + SamplePositions + GetCopyableFootprints verified\n", label);
}

// ---------------------------------------------------------------------------
// Suite 3: 6-Thread Concurrent Recording + Resource Churn + Multi-Queue Fence Sync
//          (Copy Queue -> Async Compute Queue -> Direct Queue)
// ---------------------------------------------------------------------------
void RunMultiThreadMultiQueueStressTest(bool use_warp, IDXGIAdapter1* hw_adapter, const char* label) {
    constexpr uint32_t kWorkerCount = 6u;
    constexpr uint32_t kItersPerWorker = 10u;
    constexpr uint32_t kElemCount = 64u;
    constexpr uint64_t kBufBytes = kElemCount * sizeof(uint32_t);

    std::printf(
        "[Suite 3 - %s] Running %u-thread x %u-iter Direct + Async Compute + Copy multi-queue stress test...\n",
        label, kWorkerCount, kItersPerWorker);

    ComPtr<ID3D12DeviceX> dev;
    CreateDeviceForMode(use_warp, hw_adapter, dev);

    D3D12_COMMAND_QUEUE_DESC direct_qd{};
    direct_qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_COMMAND_QUEUE_DESC compute_qd{};
    compute_qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    D3D12_COMMAND_QUEUE_DESC copy_qd{};
    copy_qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;

    ComPtr<ID3D12CommandQueueX> direct_q;
    ComPtr<ID3D12CommandQueueX> compute_q;
    ComPtr<ID3D12CommandQueueX> copy_q;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &direct_qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(direct_q.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &compute_qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(compute_q.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandQueue(
        &copy_qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(copy_q.Put()))));

    // Shared compute pipeline: out[i] = in[i] * 3 + 7 + salt
    D3D12_ROOT_PARAMETER rparams[3]{};
    rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rparams[0].Constants.ShaderRegister = 0u;
    rparams[0].Constants.RegisterSpace = 0u;
    rparams[0].Constants.Num32BitValues = 1u;
    rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rparams[1].Descriptor.ShaderRegister = 0u;
    rparams[1].Descriptor.RegisterSpace = 0u;
    rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rparams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rparams[2].Descriptor.ShaderRegister = 0u;
    rparams[2].Descriptor.RegisterSpace = 0u;
    rparams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3u;
    rs_desc.pParameters = rparams;

    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3DBlob> rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    ComPtr<ID3D12RootSignature> root_sig;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
        TEST_COM_IID(ID3D12RootSignature), reinterpret_cast<void**>(root_sig.Put()))));

    static const char* kTransformCs = R"(
        cbuffer Params : register(b0) { uint gSalt; };
        ByteAddressBuffer gIn : register(t0);
        RWByteAddressBuffer gOut : register(u0);
        [numthreads(64, 1, 1)]
        void CSMain(uint3 tid : SV_DispatchThreadID) {
            uint v = gIn.Load(tid.x * 4);
            gOut.Store(tid.x * 4, v * 3u + 7u + gSalt);
        }
    )";
    ComPtr<ID3DBlob> cs_blob;
    OS_REQUIRE(CompileCs50(kTransformCs, "CSMain", cs_blob));

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso_desc{};
    cpso_desc.pRootSignature = root_sig.Get();
    cpso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
    cpso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
    ComPtr<ID3D12PipelineState> compute_pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(
        &cpso_desc, TEST_COM_IID(ID3D12PipelineState), reinterpret_cast<void**>(compute_pso.Put()))));

    std::atomic<uint32_t> failures{0u};
    std::atomic<uint32_t> completed_jobs{0u};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);

    for (uint32_t tid = 0u; tid < kWorkerCount; ++tid) {
        workers.emplace_back([&, tid]() {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!ev) {
                failures.fetch_add(1u, std::memory_order_relaxed);
                return;
            }

            D3D12_HEAP_PROPERTIES up_hp{};
            up_hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_HEAP_PROPERTIES rb_hp{};
            rb_hp.Type = D3D12_HEAP_TYPE_READBACK;

            for (uint32_t iter = 0u; iter < kItersPerWorker; ++iter) {
                const uint32_t salt = (tid + 1u) * 1000u + iter * 17u;

                // Churn HeapX + placed default input/output buffers + committed upload/readback + DescriptorHeapX + 3 FenceX + 3 AllocatorX + 3 CommandListX
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = 256ull * 1024ull;
                hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

                ComPtr<ID3D12Heap> def_heap;
                ComPtr<ID3D12Resource> upload_buf;
                ComPtr<ID3D12Resource> gpu_in;
                ComPtr<ID3D12Resource> gpu_out;
                ComPtr<ID3D12Resource> readback_buf;
                ComPtr<ID3D12DescriptorHeap> churn_dh;
                ComPtr<ID3D12Fence> copy_fence;
                ComPtr<ID3D12Fence> compute_fence;
                ComPtr<ID3D12Fence> direct_fence;
                ComPtr<ID3D12CommandAllocator> copy_alloc;
                ComPtr<ID3D12CommandAllocator> compute_alloc;
                ComPtr<ID3D12CommandAllocator> direct_alloc;
                ComPtr<ID3D12GraphicsCommandListX> copy_cl;
                ComPtr<ID3D12GraphicsCommandListX> compute_cl;
                ComPtr<ID3D12GraphicsCommandListX> direct_cl;

                const D3D12_RESOURCE_DESC plain_desc = MakeBufferDesc(kBufBytes);
                const D3D12_RESOURCE_DESC uav_desc = MakeBufferDesc(kBufBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                D3D12_DESCRIPTOR_HEAP_DESC dhd{};
                dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                dhd.NumDescriptors = 4u;
                dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

                bool ok = true;
                ok = ok && SUCCEEDED(dev->CreateHeap(&hd, TEST_COM_IID(ID3D12Heap), reinterpret_cast<void**>(def_heap.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommittedResource(&up_hp, D3D12_HEAP_FLAG_NONE, &plain_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(upload_buf.Put())));
                ok = ok && SUCCEEDED(dev->CreatePlacedResource(def_heap.Get(), 0ull, &plain_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(gpu_in.Put())));
                ok = ok && SUCCEEDED(dev->CreatePlacedResource(def_heap.Get(), 65536ull, &uav_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(gpu_out.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &plain_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, TEST_COM_IID(ID3D12Resource), reinterpret_cast<void**>(readback_buf.Put())));
                ok = ok && SUCCEEDED(dev->CreateDescriptorHeap(&dhd, TEST_COM_IID(ID3D12DescriptorHeap), reinterpret_cast<void**>(churn_dh.Put())));
                ok = ok && SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(copy_fence.Put())));
                ok = ok && SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(compute_fence.Put())));
                ok = ok && SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, TEST_COM_IID(ID3D12Fence), reinterpret_cast<void**>(direct_fence.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, TEST_COM_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(copy_alloc.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, TEST_COM_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(compute_alloc.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, TEST_COM_IID(ID3D12CommandAllocator), reinterpret_cast<void**>(direct_alloc.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COPY, copy_alloc.Get(), nullptr, IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(copy_cl.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_alloc.Get(), compute_pso.Get(), IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(compute_cl.Put())));
                ok = ok && SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_alloc.Get(), nullptr, IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(direct_cl.Put())));
                if (!ok) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }

                // Populate input buffer
                void* up_ptr = nullptr;
                D3D12_RANGE rnone{0u, 0u};
                if (FAILED(upload_buf->Map(0u, &rnone, &up_ptr)) || !up_ptr) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }
                auto* in_u32 = static_cast<uint32_t*>(up_ptr);
                for (uint32_t i = 0u; i < kElemCount; ++i) {
                    in_u32[i] = (tid * 257u) + (iter * 31u) + i + 1u;
                }
                upload_buf->Unmap(0u, nullptr);

                // Stage 1 (Copy Queue): upload_buf -> gpu_in
                copy_cl->CopyBufferRegion(gpu_in.Get(), 0ull, upload_buf.Get(), 0ull, kBufBytes);
                ok = ok && SUCCEEDED(copy_cl->Close());

                // Stage 2 (Async Compute Queue): compute transform gpu_in -> gpu_out
                compute_cl->SetComputeRootSignature(root_sig.Get());
                compute_cl->SetComputeRoot32BitConstant(0u, salt, 0u);
                compute_cl->SetComputeRootShaderResourceView(1u, gpu_in->GetGPUVirtualAddress());
                compute_cl->SetComputeRootUnorderedAccessView(2u, gpu_out->GetGPUVirtualAddress());
                compute_cl->Dispatch(1u, 1u, 1u);
                ok = ok && SUCCEEDED(compute_cl->Close());

                // Stage 3 (Direct Queue): gpu_out -> readback_buf
                direct_cl->CopyBufferRegion(readback_buf.Get(), 0ull, gpu_out.Get(), 0ull, kBufBytes);
                ok = ok && SUCCEEDED(direct_cl->Close());

                if (!ok) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }

                // Cross-queue concurrent execution + FenceX Signal/Wait chain!
                ID3D12CommandList* copy_list[] = {copy_cl.Get()};
                copy_q->ExecuteCommandLists(1u, copy_list);
                ok = ok && SUCCEEDED(copy_q->Signal(copy_fence.Get(), 1ull));

                ok = ok && SUCCEEDED(compute_q->Wait(copy_fence.Get(), 1ull));
                ID3D12CommandList* comp_list[] = {compute_cl.Get()};
                compute_q->ExecuteCommandLists(1u, comp_list);
                ok = ok && SUCCEEDED(compute_q->Signal(compute_fence.Get(), 1ull));

                ok = ok && SUCCEEDED(direct_q->Wait(compute_fence.Get(), 1ull));
                ID3D12CommandList* dir_list[] = {direct_cl.Get()};
                direct_q->ExecuteCommandLists(1u, dir_list);
                ok = ok && SUCCEEDED(direct_q->Signal(direct_fence.Get(), 1ull));

                if (!ok) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }

                if (direct_fence->GetCompletedValue() < 1ull) {
                    if (FAILED(direct_fence->SetEventOnCompletion(1ull, ev)) ||
                        WaitForSingleObject(ev, 30000u) != WAIT_OBJECT_0) {
                        failures.fetch_add(1u, std::memory_order_relaxed);
                        break;
                    }
                }

                // Readback & verify every single uint32 element!
                void* rb_ptr = nullptr;
                D3D12_RANGE rread{0u, static_cast<SIZE_T>(kBufBytes)};
                if (FAILED(readback_buf->Map(0u, &rread, &rb_ptr)) || !rb_ptr) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }
                const auto* out_u32 = static_cast<const uint32_t*>(rb_ptr);
                bool match = true;
                for (uint32_t i = 0u; i < kElemCount; ++i) {
                    const uint32_t in_val = (tid * 257u) + (iter * 31u) + i + 1u;
                    const uint32_t expected = in_val * 3u + 7u + salt;
                    if (out_u32[i] != expected) {
                        std::fprintf(
                            stderr,
                            "[%s] Mismatch tid=%u iter=%u elem=%u got=%u expected=%u\n",
                            label, tid, iter, i, out_u32[i], expected);
                        match = false;
                        break;
                    }
                }
                D3D12_RANGE wnone{0u, 0u};
                readback_buf->Unmap(0u, &wnone);
                if (!match) {
                    failures.fetch_add(1u, std::memory_order_relaxed);
                    break;
                }
                completed_jobs.fetch_add(1u, std::memory_order_relaxed);
            }

            CloseHandle(ev);
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    OS_REQUIRE(failures.load(std::memory_order_acquire) == 0u);
    OS_REQUIRE(completed_jobs.load(std::memory_order_acquire) == kWorkerCount * kItersPerWorker);

    std::printf(
        "  [PASS - %s] %u/%u concurrent multi-queue Copy->Compute->Direct GPU jobs verified bit-exact!\n",
        label,
        completed_jobs.load(std::memory_order_acquire),
        kWorkerCount * kItersPerWorker);
}

#endif  // _WIN32

}  // namespace

int main() {
#if !defined(_WIN32)
    std::printf("test_d3d12_xs_proxies_multiqueue: skipped on non-Windows\n");
    return 0;
#else
    std::printf("=== OpenScarlett Sprint 9+10: d3d12_xs COM Proxies & Multi-Queue Stress ===\n");

    ComPtr<IDXGIAdapter1> hw_adapter;
    std::string hw_name;
    const bool has_hw = SelectHardwareAdapter(hw_adapter, hw_name);
    if (has_hw) {
        std::printf("Detected HW GPU: %s\n", hw_name.c_str());
        RunAllTenProxiesTest(/*use_warp=*/false, hw_adapter.Get(), "HW-GPU");
        RunXboxExtensionsTest(/*use_warp=*/false, hw_adapter.Get(), "HW-GPU");
        RunMultiThreadMultiQueueStressTest(/*use_warp=*/false, hw_adapter.Get(), "HW-GPU");
    } else {
        std::printf("No discrete/HW D3D12 adapter detected (headless CI); running WARP validation.\n");
    }

    RunAllTenProxiesTest(/*use_warp=*/true, nullptr, "WARP");
    RunXboxExtensionsTest(/*use_warp=*/true, nullptr, "WARP");
    RunMultiThreadMultiQueueStressTest(/*use_warp=*/true, nullptr, "WARP");

    std::printf("=== ALL d3d12_xs PROXIES & MULTI-QUEUE TESTS PASSED ===\n");
    return 0;
#endif
}
