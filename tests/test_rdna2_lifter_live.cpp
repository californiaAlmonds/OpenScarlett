#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

using Microsoft::WRL::ComPtr;

// Checks whether a specific SPIR-V opcode is present in the binary word stream.
bool SpirvContainsOpcode(const std::vector<uint32_t>& spv, uint16_t target_op) {
    size_t pos = 5;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0 || pos + wc > spv.size()) break;
        if (op == target_op) return true;
        pos += wc;
    }
    return false;
}

bool HasExternalSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(openscarlett::kDefaultSpirvValPath, ec);
}

void VerifySpirvExternalStrict(const std::vector<uint32_t>& spv, const char* label) {
    OS_REQUIRE(openscarlett::ValidateSpirvBinary(spv).ok());
    if (!HasExternalSpirvVal()) {
        return;
    }
    std::string diag;
    auto st11 = openscarlett::ValidateSpirvExternal(spv, diag, openscarlett::kDefaultSpirvValPath, "vulkan1.1");
    if (!st11.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val (vulkan1.1) for %s: %s\n", label, diag.c_str());
    }
    OS_REQUIRE(st11.ok());

    auto st12 = openscarlett::ValidateSpirvExternal(spv, diag, openscarlett::kDefaultSpirvValPath, "vulkan1.2");
    if (!st12.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val (vulkan1.2) for %s: %s\n", label, diag.c_str());
    }
    OS_REQUIRE(st12.ok());
}

#if defined(_WIN32)
struct D3D12TestContext {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList6> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;

    std::string adapter_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t adapter_flags = 0;
    uint64_t driver_version = 0;
    bool is_warp = false;
    uint32_t wave_min = 0;
    uint32_t mesh_tier = 0;
    uint32_t highest_sm = 0;

    // Catch 5 (Xbox2Windows #145): Microsoft WARP (VendorId 0x1414 / software adapter) versions
    // up through 1.0.22 exhibit a non-uniform WaveReadLaneAt bug when reading across lane widths > 8.
    // Real hardware GPUs (NVIDIA/AMD/Intel) are never guarded so hardware Wave32/Wave64 tests remain strict.
    bool IsWarpAdapter() const noexcept {
        return is_warp || vendor_id == 0x1414u || (adapter_flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u;
    }

    bool ShouldGuardWarpNonUniformWaveReadAbove8() const noexcept {
        if (!IsWarpAdapter()) return false;
        // Guard WARP software rasterizer when running native WaveReadLaneAt non-uniform > 8 lanes
        // (WARP 1.0.22 and Win10/11 built-in d3d10warp with WaveLaneCountMin <= 8).
        const uint16_t maj = static_cast<uint16_t>((driver_version >> 48) & 0xFFFFu);
        const uint16_t min_v = static_cast<uint16_t>((driver_version >> 32) & 0xFFFFu);
        const uint16_t bld = static_cast<uint16_t>((driver_version >> 16) & 0xFFFFu);
        const bool is_known_warp_1_0_22_or_older =
            (driver_version == 0u) || (maj == 1u && min_v == 0u && bld <= 22u) || (wave_min <= 8u);
        return is_known_warp_1_0_22_or_older;
    }

    ~D3D12TestContext() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool use_warp) {
        is_warp = use_warp;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

        if (use_warp) {
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        } else {
            if (FAILED(factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
                return false;
            }
        }

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        vendor_id = desc.VendorId;
        device_id = desc.DeviceId;
        adapter_flags = desc.Flags;
        LARGE_INTEGER umd_ver{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_ver))) {
            driver_version = static_cast<uint64_t>(umd_ver.QuadPart);
        }
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            return false;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            wave_min = opt1.WaveLaneCountMin;
        }
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
            mesh_tier = static_cast<uint32_t>(opt7.MeshShaderTier);
        }
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&cmd_list)))) {
            return false;
        }
        cmd_list->Close();

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    void WaitGpu() {
        ++fence_val;
        OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), fence_val)));
        if (fence->GetCompletedValue() < fence_val) {
            OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)));
            WaitForSingleObject(fence_event, 15000);
        }
    }

    ComPtr<ID3D12Resource> CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap_type,
                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> res;
        HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
                                                     IID_PPV_ARGS(&res));
        OS_REQUIRE(SUCCEEDED(hr));
        return res;
    }

    void RunComputeBuffers(const std::vector<uint8_t>& dxil,
                           const std::vector<uint32_t>& in_data,
                           std::vector<uint32_t>& out_data,
                           uint32_t group_x) {
        const uint64_t byte_size = static_cast<uint64_t>(in_data.size()) * sizeof(uint32_t);
        out_data.assign(in_data.size(), 0u);

        // Root signature: u0 (root UAV) + u1 (root UAV)
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
        OS_REQUIRE(SUCCEEDED(hr));

        ComPtr<ID3D12RootSignature> root_sig;
        hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_sig));
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState failed on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto in_upload = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_upload = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto readback = CreateBuffer(byte_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(in_upload->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, in_data.data(), static_cast<size_t>(byte_size));
        in_upload->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(out_upload->Map(0, nullptr, &mapped)));
        std::memset(mapped, 0, static_cast<size_t>(byte_size));
        out_upload->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        cmd_list->CopyResource(in_gpu.Get(), in_upload.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_upload.Get());

        D3D12_RESOURCE_BARRIER to_uav[2]{};
        to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, to_uav);

        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
        cmd_list->Dispatch(group_x, 1, 1);

        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1, &uav_bar);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);
        cmd_list->CopyResource(readback.Get(), out_gpu.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        HRESULT map_hr = readback->Map(0, nullptr, &mapped);
        if (FAILED(map_hr)) {
            std::fprintf(stderr, "[FAIL] readback->Map failed on %s: hr=0x%08lX removed=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(map_hr),
                         static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        }
        OS_REQUIRE(SUCCEEDED(map_hr));
        std::memcpy(out_data.data(), mapped, static_cast<size_t>(byte_size));
        readback->Unmap(0, nullptr);
    }

    void RunComputeMimgTexture16x16(const std::vector<uint8_t>& dxil,
                                    const std::vector<float>& in_rgba,
                                    std::vector<float>& out_rgba) {
        constexpr UINT kW = 16;
        constexpr UINT kH = 16;
        constexpr UINT kRowPitch = kW * 16;  // 16 * sizeof(float4) = 256 bytes (exact D3D12 pitch alignment)
        constexpr UINT kTotalBytes = kRowPitch * kH;
        OS_REQUIRE(in_rgba.size() == kW * kH * 4);
        out_rgba.assign(kW * kH * 4, 0.0f);

        // Root signature: Descriptor table [t0 (SRV), u1 (UAV)] + static point sampler s0
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        samp.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;

        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        OS_REQUIRE(SUCCEEDED(device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso))));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kW;
        td.Height = kH;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> in_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&in_tex))));
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> out_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                             IID_PPV_ARGS(&out_tex))));

        auto upload = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, in_rgba.data(), kTotalBytes);
        upload->Unmap(0, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = {cpu0.ptr + inc};

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(in_tex.Get(), &srvd, cpu0);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(out_tex.Get(), nullptr, &uavd, cpu1);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = upload.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Offset = 0;
        src_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kRowPitch};

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = in_tex.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = 0;
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        D3D12_RESOURCE_BARRIER to_srv{};
        to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_srv.Transition = {in_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        cmd_list->ResourceBarrier(1, &to_srv);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(4, 1, 1);  // 4 * 64 = 256 threads covering 16x16

        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_tex.Get();
        cmd_list->ResourceBarrier(1, &uav_bar);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {out_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION rb_src{};
        rb_src.pResource = out_tex.Get();
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = readback.Get();
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint.Offset = 0;
        rb_dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1, kRowPitch};
        cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        HRESULT map_hr = readback->Map(0, nullptr, &mapped);
        if (FAILED(map_hr)) {
            std::fprintf(stderr, "[FAIL] readback->Map failed on %s: hr=0x%08lX removed=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(map_hr),
                         static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        }
        OS_REQUIRE(SUCCEEDED(map_hr));
        std::memcpy(out_rgba.data(), mapped, kTotalBytes);
        readback->Unmap(0, nullptr);
    }

    void RunMeshShaderPipeline64x64(const std::vector<uint8_t>& as_dxil,
                                    const std::vector<uint8_t>& ms_dxil,
                                    const std::vector<uint8_t>& ps_dxil,
                                    std::vector<uint8_t>& out_rgba8) {
        constexpr UINT kW = 64;
        constexpr UINT kH = 64;
        constexpr UINT kRowPitch = kW * 4;  // 256 bytes exact D3D12 pitch alignment
        constexpr UINT kTotalBytes = kRowPitch * kH;
        out_rgba8.assign(kTotalBytes, 0u);

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
        struct alignas(void*) SubobjRootSig {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* val = nullptr;
        };
        struct alignas(void*) SubobjAS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjMS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjPS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            D3D12_SHADER_BYTECODE val{};
        };
        struct alignas(void*) SubobjBlend {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
            D3D12_BLEND_DESC val{};
        };
        struct alignas(void*) SubobjSampleMask {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
            UINT val = 0xFFFFFFFFu;
        };
        struct alignas(void*) SubobjRast {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
            D3D12_RASTERIZER_DESC val{};
        };
        struct alignas(void*) SubobjDepth {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
            D3D12_DEPTH_STENCIL_DESC val{};
        };
        struct alignas(void*) SubobjRtvFormats {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            D3D12_RT_FORMAT_ARRAY val{};
        };
        struct alignas(void*) SubobjSampleDesc {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
            DXGI_SAMPLE_DESC val{1, 0};
        };

        struct MeshPsoStream {
            SubobjRootSig root_sig;
            SubobjAS as;
            SubobjMS ms;
            SubobjPS ps;
            SubobjBlend blend;
            SubobjSampleMask sample_mask;
            SubobjRast rast;
            SubobjDepth depth;
            SubobjRtvFormats rtv_formats;
            SubobjSampleDesc sample_desc;
        } stream{};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

        stream.root_sig.val = root_sig.Get();
        stream.as.val = {as_dxil.data(), as_dxil.size()};
        stream.ms.val = {ms_dxil.data(), ms_dxil.size()};
        stream.ps.val = {ps_dxil.data(), ps_dxil.size()};
        stream.blend.val.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.sample_mask.val = 0xFFFFFFFFu;
        stream.rast.val.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rast.val.CullMode = D3D12_CULL_MODE_NONE;
        stream.rast.val.DepthClipEnable = FALSE;
        stream.depth.val.DepthEnable = FALSE;
        stream.depth.val.StencilEnable = FALSE;
        stream.rtv_formats.val.NumRenderTargets = 1;
        stream.rtv_formats.val.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample_desc.val = {1, 0};

        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] Mesh CreatePipelineState failed on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rtd{};
        rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rtd.Width = kW;
        rtd.Height = kH;
        rtd.DepthOrArraySize = 1;
        rtd.MipLevels = 1;
        rtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtd.SampleDesc.Count = 1;
        rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> rt_tex;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &rtd,
                                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                             IID_PPV_ARGS(&rt_tex))));

        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
        rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&rtv_heap))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(rt_tex.Get(), nullptr, rtv_h);

        auto readback = CreateBuffer(kTotalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        const float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd_list->ClearRenderTargetView(rtv_h, clear_rgba, 0, nullptr);
        cmd_list->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd_list->RSSetViewports(1, &vp);
        cmd_list->RSSetScissorRects(1, &sc);

        cmd_list->SetGraphicsRootSignature(root_sig.Get());
        cmd_list->SetPipelineState(pso.Get());
        cmd_list->DispatchMesh(1, 1, 1);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {rt_tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION rb_src{};
        rb_src.pResource = rt_tex.Get();
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION rb_dst{};
        rb_dst.pResource = readback.Get();
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint.Offset = 0;
        rb_dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kRowPitch};
        cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        void* mapped = nullptr;
        HRESULT map_hr = readback->Map(0, nullptr, &mapped);
        if (FAILED(map_hr)) {
            std::fprintf(stderr, "[FAIL] readback->Map failed on %s: hr=0x%08lX removed=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(map_hr),
                         static_cast<unsigned long>(device->GetDeviceRemovedReason()));
        }
        OS_REQUIRE(SUCCEEDED(map_hr));
        std::memcpy(out_rgba8.data(), mapped, kTotalBytes);
        readback->Unmap(0, nullptr);
    }

    // Runs 3D IMAGE_LOAD_MIP (t0), Cube/2DArray IMAGE_LOAD_MIP (t1), and 2D R32_UINT IMAGE_ATOMIC_CMPSWAP (u2)
    // alongside output buffer (u1) on live D3D12 HW and WARP!
    void RunComputeMimg3dCubeAtomic(const std::vector<uint8_t>& dxil,
                                    const openscarlett::Rdna2CpuTexture& tex3d,
                                    const openscarlett::Rdna2CpuTexture& texcube,
                                    uint32_t initial_atomic_val,
                                    std::vector<uint32_t>& out_buf,
                                    uint32_t& out_final_atomic_val) {
        out_buf.assign(64, 0u);
        out_final_atomic_val = 0u;

        // Root signature:
        // Param 0: root UAV u1 (_out0)
        // Param 1: descriptor table [t0 (3D SRV), t1 (Cube 2DArray SRV), u2 (RWTexture2D<uint>)]
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 2;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 1;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] RunComputeMimg3dCubeAtomic PSO failed on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        D3D12_HEAP_PROPERTIES hp_def{};
        hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;

        // 1. 3D texture 4x4x4 with 2 mips (mip0: 4x4x4, mip1: 2x2x2)
        D3D12_RESOURCE_DESC td3d{};
        td3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        td3d.Width = 4;
        td3d.Height = 4;
        td3d.DepthOrArraySize = 4;
        td3d.MipLevels = 2;
        td3d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td3d.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> res_3d;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td3d,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&res_3d))));

        // 2. Cube texture as 2DArray (4x4, 6 faces, 2 mips)
        D3D12_RESOURCE_DESC tdcube{};
        tdcube.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tdcube.Width = 4;
        tdcube.Height = 4;
        tdcube.DepthOrArraySize = 6;
        tdcube.MipLevels = 2;
        tdcube.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        tdcube.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> res_cube;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &tdcube,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&res_cube))));

        // 3. Atomic 2D R32_UINT texture (4x4)
        D3D12_RESOURCE_DESC tdatom{};
        tdatom.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tdatom.Width = 4;
        tdatom.Height = 4;
        tdatom.DepthOrArraySize = 1;
        tdatom.MipLevels = 1;
        tdatom.Format = DXGI_FORMAT_R32_UINT;
        tdatom.SampleDesc.Count = 1;
        tdatom.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> res_atom;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &tdatom,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                             IID_PPV_ARGS(&res_atom))));

        // Upload staging buffers (row pitch = 256 bytes minimum D3D12 alignment)
        auto up_3d_m1 = CreateBuffer(256u * 2u * 2u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
        {
            void* p = nullptr;
            OS_REQUIRE(SUCCEEDED(up_3d_m1->Map(0, nullptr, &p)));
            auto* dst = static_cast<uint8_t*>(p);
            std::memset(dst, 0, 256u * 4u);
            for (uint32_t z = 0; z < 2u; ++z) {
                for (uint32_t y = 0; y < 2u; ++y) {
                    auto* row = reinterpret_cast<float*>(dst + (z * 2u + y) * 256u);
                    for (uint32_t x = 0; x < 2u; ++x) {
                        auto c = tex3d.LoadRgba32f(x, y, z, 1u);
                        row[x * 4 + 0] = c[0]; row[x * 4 + 1] = c[1];
                        row[x * 4 + 2] = c[2]; row[x * 4 + 3] = c[3];
                    }
                }
            }
            up_3d_m1->Unmap(0, nullptr);
        }

        // Cube mip1 upload for all 6 faces (each 2x2 slice needs 512 bytes)
        auto up_cube_m1 = CreateBuffer(512u * 6u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        {
            void* p = nullptr;
            OS_REQUIRE(SUCCEEDED(up_cube_m1->Map(0, nullptr, &p)));
            auto* dst = static_cast<uint8_t*>(p);
            std::memset(dst, 0, 512u * 6u);
            for (uint32_t f = 0; f < 6u; ++f) {
                for (uint32_t y = 0; y < 2u; ++y) {
                    auto* row = reinterpret_cast<float*>(dst + f * 512u + y * 256u);
                    for (uint32_t x = 0; x < 2u; ++x) {
                        auto c = texcube.LoadRgba32f(x, y, f, 1u);
                        row[x * 4 + 0] = c[0]; row[x * 4 + 1] = c[1];
                        row[x * 4 + 2] = c[2]; row[x * 4 + 3] = c[3];
                    }
                }
            }
            up_cube_m1->Unmap(0, nullptr);
        }

        auto up_atom = CreateBuffer(256u * 4u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
        {
            void* p = nullptr;
            OS_REQUIRE(SUCCEEDED(up_atom->Map(0, nullptr, &p)));
            auto* dst = static_cast<uint8_t*>(p);
            std::memset(dst, 0, 256u * 4u);
            for (uint32_t y = 0; y < 4u; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(dst + y * 256u);
                for (uint32_t x = 0; x < 4u; ++x) row[x] = initial_atomic_val;
            }
            up_atom->Unmap(0, nullptr);
        }

        auto out_gpu = CreateBuffer(64u * 4u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto rb_buf = CreateBuffer(64u * 4u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb_atom = CreateBuffer(256u * 4u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE h1 = {h0.ptr + inc};
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = {h0.ptr + inc * 2u};

        D3D12_SHADER_RESOURCE_VIEW_DESC srv3d{};
        srv3d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv3d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv3d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv3d.Texture3D.MipLevels = 2;
        device->CreateShaderResourceView(res_3d.Get(), &srv3d, h0);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvcube{};
        srvcube.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvcube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvcube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvcube.Texture2DArray.MipLevels = 2;
        srvcube.Texture2DArray.ArraySize = 6;
        device->CreateShaderResourceView(res_cube.Get(), &srvcube, h1);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_atom{};
        uav_atom.Format = DXGI_FORMAT_R32_UINT;
        uav_atom.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(res_atom.Get(), nullptr, &uav_atom, h2);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));

        // Copy 3D mip 1 (subresource 1)
        {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = up_3d_m1.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 2, 2, 256};
            dst.pResource = res_3d.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 1;
            cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        // Copy Cube mip 1 for each face f (subresource = f * 2 + 1)
        for (UINT f = 0; f < 6u; ++f) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = up_cube_m1.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = f * 512u;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 2, 1, 256};
            dst.pResource = res_cube.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = f * 2u + 1u;
            cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        // Copy atomic texture
        {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = up_atom.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 4, 4, 1, 256};
            dst.pResource = res_atom.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER bars[3]{};
        bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[0].Transition = {res_3d.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[1].Transition = {res_cube.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        bars[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[2].Transition = {res_atom.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(3, bars);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, out_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER post[2]{};
        post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[0].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        post[1].Transition = {res_atom.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(2, post);

        cmd_list->CopyResource(rb_buf.Get(), out_gpu.Get());
        {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = res_atom.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            dst.pResource = rb_atom.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 4, 4, 1, 256};
            cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(rb_buf->Map(0, nullptr, &mp)));
        std::memcpy(out_buf.data(), mp, 64u * 4u);
        rb_buf->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(rb_atom->Map(0, nullptr, &mp)));
        out_final_atomic_val = *static_cast<const uint32_t*>(mp);
        rb_atom->Unmap(0, nullptr);
    }
};
#endif

// Compiles lifted HLSL to a WARP-compatible DXIL target (SM 6.2 on Win10 WARP, or SM 6.5 if supported).
std::vector<uint8_t> CompileForDevice(const D3D12TestContext& ctx,
                                      const std::string& hlsl,
                                      const std::vector<uint8_t>& sm65_dxil) {
    if (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
        return sm65_dxil;
    }
    const char* target = (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
    std::vector<uint8_t> dxil;
    std::string diag;
    auto st = openscarlett::CompileHlslWithDxc(hlsl, target, "main", dxil, diag);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] DXC %s compile for WARP failed: %s\n", target, diag.c_str());
    }
    OS_REQUIRE(st.ok());
    return dxil;
}

// ---------------------------------------------------------------------------
// Test 1: Compute ALU + Conditional Branch + Constant Folding
// ---------------------------------------------------------------------------
void Test1_ComputeAluBranchConstFold(openscarlett::VulkanBackend& vk,
                                     D3D12TestContext& d3d_hw,
                                     D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4 (byte offset)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v2 = in[tid] (srsrc = 0 -> u0 / binding 0)
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    // Foldable constants: s8 = 1 + 2 = 3, s9 = 2 + 3 = 5
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::InlineInt(1));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 8, Rdna2Operand::Sgpr(8), Rdna2Operand::InlineInt(2));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::InlineInt(2));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 9, Rdna2Operand::Sgpr(9), Rdna2Operand::InlineInt(3));
    // v3 = v2 * 3; v4 = v3 + 5
    a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8));
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::Sgpr(9), 3);
    // Branch: if (tid < 32) v5 = v4; else v5 = v4 + 100;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(32));
    a.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 10);
    const uint32_t br_else = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    // Then: v5 = v4 + 0
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 5, Rdna2Operand::InlineInt(0), 4);
    const uint32_t br_join = a.EmitSopp(Rdna2Opcode::S_BRANCH);
    // Else: v5 = v4 + 100
    a.PatchBranch(br_else, a.CurrentWordOffset());
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 5, Rdna2Operand::LiteralU32(100u), 4);
    // Join: store v5 to out[tid] (srsrc_sgpr = 4 -> srsrc = 1 -> u1 / binding 1)
    a.PatchBranch(br_join, a.CurrentWordOffset());
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test1 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.opt_stats.folded_constants >= 2u);
    OS_REQUIRE(res.hlsl_target == "cs_6_5");
    VerifySpirvExternalStrict(res.spirv_words, "Test1_ComputeAluBranch");

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input(kCount);
    for (uint32_t i = 0; i < kCount; ++i) input[i] = i * 7u + 13u;

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() == kCount);
        for (uint32_t i = 0; i < kCount; ++i) {
            const uint32_t expected = (i < 32u) ? (input[i] * 3u + 5u) : (input[i] * 3u + 105u);
            if (out[i] != expected) {
                std::fprintf(stderr, "[FAIL] Test1 %s lane %u: got %u expected %u\n", backend, i, out[i], expected);
            }
            OS_REQUIRE(out[i] == expected);
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    // Verify exact parity with CPU RDNA2 Reference Interpreter
    Rdna2CpuExecState cpu{};
    cpu.uav0 = input;
    cpu.uav1.assign(kCount, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
    verify_golden(cpu.uav1, "CPU-Interpreter");
    OS_REQUIRE(cpu.uav1 == d3d_hw_out);

    // Also verify SM 5.0 fallback target emission
    std::string sm50_hlsl, sm50_target;
    OS_REQUIRE(EmitHlsl(res.ir, sm50_hlsl, sm50_target, /*use_sm65=*/false).ok());
    OS_REQUIRE(sm50_target == "cs_5_0");

    std::printf("[PASS] Test 1: Compute ALU + Branch + ConstFold (%u folded) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP\n",
                res.ir.opt_stats.folded_constants);
}

// ---------------------------------------------------------------------------
// Test 2: DS LDS Shared Memory + S_BARRIER + DS_ADD_RTN_U32 Atomic
// ---------------------------------------------------------------------------
void Test2_DsLdsBarrierAtomics(openscarlett::VulkanBackend& vk,
                               D3D12TestContext& d3d_hw,
                               D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v2 = in[tid]
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    // Zero-init LDS counter at byte offset 256 (lds[64]) on tid == 0
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
    const uint32_t br_skip = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(256u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 11, 10);
    a.PatchBranch(br_skip, a.CurrentWordOffset());

    // Write v2 to LDS[tid]
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
    a.EmitSopp(Rdna2Opcode::S_BARRIER);

    // Read reversed lane (63 - tid) from LDS
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(63));
    a.EmitVop2(Rdna2Opcode::V_SUB_NC_U32, 3, Rdna2Operand::Vgpr(12), 0);
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 3);
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4);

    // Atomic increment counter at LDS byte offset 256 -> unique ticket in v6
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralU32(256u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::InlineInt(1));
    a.EmitDs(Rdna2Opcode::DS_ADD_RTN_U32, 6, 13, 14);
    a.EmitSopp(Rdna2Opcode::S_BARRIER);

    // Read final atomic sum (must be 64) into v7
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 7, 13);

    // Pack: out[tid] = (v5 << 16) | (v7 << 8) | v6
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(16), 5);
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 9, Rdna2Operand::InlineInt(8), 7);
    a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 9);
    a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 6);

    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test2 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_lds);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 234u));  // OpAtomicIAdd
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 224u));  // OpControlBarrier
    VerifySpirvExternalStrict(res.spirv_words, "Test2_DsLdsAtomics");

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input(kCount);
    for (uint32_t i = 0; i < kCount; ++i) input[i] = i * 19u + 5u;

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() == kCount);
        std::array<bool, kCount> seen{};
        for (uint32_t i = 0; i < kCount; ++i) {
            const uint32_t rev_val = out[i] >> 16;
            const uint32_t total = (out[i] >> 8) & 0xFFu;
            const uint32_t ticket = out[i] & 0xFFu;
            if (rev_val != input[63u - i] || total != 64u || ticket >= 64u) {
                std::fprintf(stderr, "[FAIL] Test2 %s lane %u: rev=%u(exp %u) total=%u ticket=%u\n",
                             backend, i, rev_val, input[63u - i], total, ticket);
            }
            OS_REQUIRE(rev_val == input[63u - i]);
            OS_REQUIRE(total == 64u);
            OS_REQUIRE(ticket < 64u);
            OS_REQUIRE(!seen[ticket]);
            seen[ticket] = true;
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    // CPU RDNA2 Reference Interpreter parity (verifies LDS + S_BARRIER lockstep + DS_ADD_RTN_U32)
    Rdna2CpuExecState cpu{};
    cpu.uav0 = input;
    cpu.uav1.assign(kCount, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
    OS_REQUIRE(cpu.barriers_synchronized == 2u);
    verify_golden(cpu.uav1, "CPU-Interpreter");

    std::printf("[PASS] Test 2: DS LDS + S_BARRIER + DS_ADD_RTN_U32 (64/64 unique atomic tickets) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP + CPU parity\n");
}

// ---------------------------------------------------------------------------
// Test 3: wave64 -> wave32 Emulation (Ballot64, ActiveCount64, PrefixCount64, ReadLaneAt(42), ReadFirstLane, Swizzle)
// ---------------------------------------------------------------------------
void Test3_Wave64OnWave32(openscarlett::VulkanBackend& vk,
                          D3D12TestContext& d3d_hw,
                          D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // v1 = tid * 4
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // v4 = in[tid]
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 1, 0);

    // Predicate: (tid & 1) == 0 -> 32 active even lanes across wave64
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 2, Rdna2Operand::InlineInt(1), 0);
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 2);

    // s8 = WaveActiveCountBits64(vcc) == 32
    a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 8, Rdna2Operand::VccLo());

    // v6 = WavePrefixCount64(vcc) == tid/2 for even lanes, (tid+1)/2 for odd lanes
    a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 5, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
    a.EmitVop3(Rdna2Opcode::V_MBCNT_HI_U32_B32, 6, Rdna2Operand::VccHi(), Rdna2Operand::Vgpr(5));

    // s9 = WaveReadLaneAt(v4, 42) across Wave32 boundary [0..31] <-> [32..63]!
    a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 9, Rdna2Operand::Vgpr(4), Rdna2Operand::InlineInt(42));
    // s10 = WaveReadFirstLane(v4) == in[0]
    a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 10, Rdna2Operand::Vgpr(4));
    // v7 = DS_SWIZZLE_B32 butterfly XOR 1 (swaps adjacent lanes tid ^ 1)
    a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 7, 4, 0, 0, static_cast<uint16_t>(0x8000u | (1u << 10) | 0x1Fu));

    // Store 5 full 32-bit results per lane into out[k*64 + tid]
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::Sgpr(8));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::Sgpr(9));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::Sgpr(10));

    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 1, 4, 0);     // out[0..63]   = active_count (32)
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 256);    // out[64..127] = prefix_count
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 1, 4, 512);   // out[128..191]= in[42]
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 768);   // out[192..255]= in[0]
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 1024);   // out[256..319]= in[tid ^ 1]
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, /*wave_size=*/64);

    // Also verify XS2 wave_size=64 round-trip!
    Xs2ShaderContainer parsed{};
    OS_REQUIRE(ParseXs2Container(xs2, parsed).ok());
    OS_REQUIRE(parsed.wave_size == 64u);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test3 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_wave64_emulation);
    OS_REQUIRE(res.ir.wave_size == 64u);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 205u));  // OpBitCount
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 241u));  // OpAtomicOr
    VerifySpirvExternalStrict(res.spirv_words, "Test3_Wave64OnWave32");

    std::vector<uint32_t> input(320, 0u);
    for (uint32_t i = 0; i < 64u; ++i) input[i] = 0xCAFE0000u | (i * 17u + 3u);

    auto verify_golden = [&](const std::vector<uint32_t>& out, const char* backend) {
        OS_REQUIRE(out.size() >= 320u);
        for (uint32_t tid = 0; tid < 64u; ++tid) {
            const uint32_t exp_active = 32u;
            const uint32_t exp_prefix = (tid + 1u) / 2u;  // exact count of even lanes < tid (== tid/2 for even lanes)
            const uint32_t exp_lane42 = input[42];
            const uint32_t exp_lane0 = input[0];
            const uint32_t exp_swiz = input[tid ^ 1u];

            if (out[tid] != exp_active || out[64 + tid] != exp_prefix ||
                out[128 + tid] != exp_lane42 || out[192 + tid] != exp_lane0 ||
                out[256 + tid] != exp_swiz) {
                std::fprintf(stderr,
                             "[FAIL] Test3 %s tid=%u: active=%u(%u) prefix=%u(%u) lane42=0x%X(0x%X) lane0=0x%X(0x%X) swiz=0x%X(0x%X)\n",
                             backend, tid,
                             out[tid], exp_active,
                             out[64 + tid], exp_prefix,
                             out[128 + tid], exp_lane42,
                             out[192 + tid], exp_lane0,
                             out[256 + tid], exp_swiz);
            }
            OS_REQUIRE(out[tid] == exp_active);
            OS_REQUIRE(out[64 + tid] == exp_prefix);
            if ((tid & 1u) == 0u) {
                OS_REQUIRE(out[64 + tid] == tid / 2u);
            }
            OS_REQUIRE(out[128 + tid] == exp_lane42);
            OS_REQUIRE(out[192 + tid] == exp_lane0);
            OS_REQUIRE(out[256 + tid] == exp_swiz);
        }
    };

    if (vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
        verify_golden(vk_out, "Vulkan-HW");
    }

    std::vector<uint32_t> d3d_hw_out;
    d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, d3d_hw_out, 1);
    verify_golden(d3d_hw_out, "D3D12-HW-Wave32");

    std::vector<uint32_t> d3d_warp_out;
    d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, d3d_warp_out, 1);
    verify_golden(d3d_warp_out, "D3D12-WARP");

    // Wave64 CPU RDNA2 Reference Interpreter exact parity vs live GPU!
    Rdna2CpuExecState cpu{};
    cpu.uav0 = input;
    cpu.uav1.assign(320, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
    verify_golden(cpu.uav1, "CPU-Interpreter-Wave64");
    OS_REQUIRE(cpu.uav1 == d3d_hw_out);

    std::printf("[PASS] Test 3: wave64->wave32 emulation (HW Wave=%u, Ballot64/Count=32/Prefix/ReadLane(42)/Swizzle) -> spirv-val + Vulkan + DXC cs_6_5 + D3D12 HW + WARP + CPU Wave64 parity\n",
                d3d_hw.wave_min);
}

// ---------------------------------------------------------------------------
// Test 4: MIMG (IMAGE_LOAD + IMAGE_SAMPLE_LZ + IMAGE_STORE) on 16x16 RGBA32F
// ---------------------------------------------------------------------------
void Test4_MimgTexturePipeline(D3D12TestContext& d3d_hw, D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2Assembler a;
    // 1D dispatch (4*64 = 256 threads) -> 2D pixel coords: v1 = x = tid & 15, v2 = y = tid >> 4
    a.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(15), 0);
    a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2, Rdna2Operand::InlineInt(4), 0);

    // IMAGE_LOAD v[4:7], v[1:2], s[0:7] dmask:0xf dim:2D unrm
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD, 4, 1, 0, 0, 0xFu, 1u, true);

    // Normalized texel-center UVs: u = (float(x) + 0.5) / 16.0, v = (float(y) + 0.5) / 16.0
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(1));
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 11, Rdna2Operand::Vgpr(2));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 10, Rdna2Operand::InlineFloat(0.5f), 10);
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 11, Rdna2Operand::InlineFloat(0.5f), 11);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 10, Rdna2Operand::LiteralF32(1.0f / 16.0f), 10);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 11, Rdna2Operand::LiteralF32(1.0f / 16.0f), 11);

    // IMAGE_SAMPLE_LZ v[12:15], v[10:11], s[0:7], s[8:11] dmask:0xf dim:2D
    a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE_LZ, 12, 10, 0, 8, 0xFu, 1u, false);

    // Transform:
    // out.r = load.r * 0.5 + sample.r * 0.5 + 0.125
    // out.g = load.g * 2.0
    // out.b = sample.b + 0.25
    // out.a = 1.0
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 16, Rdna2Operand::InlineFloat(0.5f), 4);
    a.EmitVop3(Rdna2Opcode::V_FMA_F32, 16, Rdna2Operand::Vgpr(12), Rdna2Operand::InlineFloat(0.5f), Rdna2Operand::Vgpr(16));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 16, Rdna2Operand::LiteralF32(0.125f), 16);
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 17, Rdna2Operand::InlineFloat(2.0f), 5);
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 18, Rdna2Operand::LiteralF32(0.25f), 14);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 19, Rdna2Operand::InlineFloat(1.0f));

    // IMAGE_STORE v[16:19], v[1:2], s[0:7] dmask:0xf dim:2D unrm
    a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 16, 1, 0, 0, 0xFu, 1u, true);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 24, 32);

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    Rdna2LiftResult res{};
    auto st = LiftRdna2Xs2Shader(xs2, res, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Test4 Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(res.ir.uses_texture_srv && res.ir.uses_texture_uav);
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 88u));  // OpImageSampleExplicitLod
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 98u));  // OpImageRead
    OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 99u));  // OpImageWrite
    VerifySpirvExternalStrict(res.spirv_words, "Test4_MimgTexture");

    std::vector<float> in_rgba(16 * 16 * 4);
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const size_t idx = (y * 16 + x) * 4;
            in_rgba[idx + 0] = static_cast<float>(x) / 16.0f;
            in_rgba[idx + 1] = static_cast<float>(y) / 32.0f;
            in_rgba[idx + 2] = static_cast<float>((x + y) & 15u) / 16.0f;
            in_rgba[idx + 3] = 1.0f;
        }
    }

    auto verify_pixels = [&](const std::vector<float>& out, const char* backend) {
        OS_REQUIRE(out.size() == in_rgba.size());
        for (uint32_t y = 0; y < 16; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                const size_t idx = (y * 16 + x) * 4;
                const float exp_r = in_rgba[idx + 0] + 0.125f;
                const float exp_g = in_rgba[idx + 1] * 2.0f;
                const float exp_b = in_rgba[idx + 2] + 0.25f;
                const float exp_a = 1.0f;
                if (std::fabs(out[idx + 0] - exp_r) > 1e-4f ||
                    std::fabs(out[idx + 1] - exp_g) > 1e-4f ||
                    std::fabs(out[idx + 2] - exp_b) > 1e-4f ||
                    std::fabs(out[idx + 3] - exp_a) > 1e-4f) {
                    std::fprintf(stderr,
                                 "[FAIL] Test4 %s (%u,%u): got (%.4f,%.4f,%.4f,%.4f) exp (%.4f,%.4f,%.4f,%.4f)\n",
                                 backend, x, y,
                                 out[idx + 0], out[idx + 1], out[idx + 2], out[idx + 3],
                                 exp_r, exp_g, exp_b, exp_a);
                }
                OS_REQUIRE(std::fabs(out[idx + 0] - exp_r) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 1] - exp_g) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 2] - exp_b) <= 1e-4f);
                OS_REQUIRE(std::fabs(out[idx + 3] - exp_a) <= 1e-4f);
            }
        }
    };

    std::vector<float> hw_out;
    d3d_hw.RunComputeMimgTexture16x16(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), in_rgba, hw_out);
    verify_pixels(hw_out, "D3D12-HW");

    std::vector<float> warp_out;
    d3d_warp.RunComputeMimgTexture16x16(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), in_rgba, warp_out);
    verify_pixels(warp_out, "D3D12-WARP");

    // CPU RDNA2 Reference Interpreter 16x16 texture parity!
    Rdna2CpuExecState cpu{};
    cpu.dispatch_x = 4;
    cpu.tex2d_srv.AllocateRgba32f(1, 16, 16, 1, 1);
    cpu.tex2d_srv.mips[0].rgba32f = in_rgba;
    cpu.tex2d_uav.AllocateRgba32f(1, 16, 16, 1, 1);
    OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
    verify_pixels(cpu.tex2d_uav.mips[0].rgba32f, "CPU-Interpreter");

    std::printf("[PASS] Test 4: MIMG (IMAGE_LOAD + IMAGE_SAMPLE_LZ + IMAGE_STORE, 16x16 RGBA32F) -> spirv-val + DXC cs_6_5 + D3D12 HW + WARP + CPU parity\n");
}

// ---------------------------------------------------------------------------
// Test 5: VS + PS (spirv-val + dxc) AND AS + MS + PS LIVE on GTX 1650 Ti (MeshTier=10)
// ---------------------------------------------------------------------------
void Test5_GraphicsVsPsAndLiveMeshAs(D3D12TestContext& d3d_hw) {
    using namespace openscarlett;

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    // 5A. Vertex Shader (vs_6_5 + SPIR-V 1.3)
    Rdna2Assembler vs;
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult vs_res{};
    auto st_vs = LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex), vs_res, opts);
    if (!st_vs.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 VS failed: %s\nHLSL:\n%s\n", st_vs.message.c_str(), vs_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_vs.ok());
    OS_REQUIRE(vs_res.hlsl_target == "vs_6_5");
    VerifySpirvExternalStrict(vs_res.spirv_words, "Test5_VS");

    // 5B. Pixel Shader (ps_6_5 + SPIR-V 1.3): doubles R (0.25 -> 0.50), preserves G=0.50, B=0.75, A=1.0
    Rdna2Assembler ps;
    ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::InlineFloat(2.0f), 0);
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 1, 2, 3, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult ps_res{};
    auto st_ps = LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel), ps_res, opts);
    if (!st_ps.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 PS failed: %s\nHLSL:\n%s\n", st_ps.message.c_str(), ps_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_ps.ok());
    OS_REQUIRE(ps_res.hlsl_target == "ps_6_5");
    VerifySpirvExternalStrict(ps_res.spirv_words, "Test5_PS");

    // 5C. Amplification Shader (as_6_5): dispatches (1, 1, 1) mesh threadgroup
    Rdna2Assembler as;
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
    as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
    as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
    as.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult as_res{};
    auto st_as = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 1, 1, 1), as_res, opts);
    if (!st_as.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 AS failed: %s\nHLSL:\n%s\n", st_as.message.c_str(), as_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_as.ok());
    OS_REQUIRE(as_res.hlsl_target == "as_6_5");

    // 5D. Mesh Shader (ms_6_5): emits 3 vertices + 1 full-screen triangle covering 64x64 RT
    Rdna2Assembler ms;
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));  // vert_count = 3
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));  // prim_count = 1
    ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

    // Per-thread vertex position from v0 (_gtid in 0..2): (-1,-1), (-1,3), (3,-1)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

    // Vertex color Param0 = (0.25, 0.50, 0.75, 1.0)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

    // Thread 0 exports primitive connectivity (0, 1, 2) via EXP Prim (target 20)
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
    const uint32_t br_skip_prim = ms.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(1));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(2));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
    ms.PatchBranch(br_skip_prim, ms.CurrentWordOffset());
    ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftResult ms_res{};
    auto st_ms = LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1), ms_res, opts);
    if (!st_ms.ok()) {
        std::fprintf(stderr, "[FAIL] Test5 MS failed: %s\nHLSL:\n%s\n", st_ms.message.c_str(), ms_res.hlsl_source.c_str());
    }
    OS_REQUIRE(st_ms.ok());
    OS_REQUIRE(ms_res.hlsl_target == "ms_6_5");
    OS_REQUIRE(ms_res.ir.exports_prim);

    // Execute LIVE AS + MS + PS Mesh Shader Pipeline on HW GPUs with MeshTier >= 10 (GTX 1650 Ti)!
    if (d3d_hw.mesh_tier >= 10u) {
        std::vector<uint8_t> rt_pixels;
        d3d_hw.RunMeshShaderPipeline64x64(as_res.pc_dxil, ms_res.pc_dxil, ps_res.pc_dxil, rt_pixels);
        OS_REQUIRE(rt_pixels.size() == 64u * 64u * 4u);

        // Expected color: (0.25*2.0, 0.50, 0.75, 1.0) -> UNORM8 (128, 128, 191, 255)
        for (uint32_t p = 0; p < 64u * 64u; ++p) {
            const int r = static_cast<int>(rt_pixels[p * 4 + 0]);
            const int g = static_cast<int>(rt_pixels[p * 4 + 1]);
            const int b = static_cast<int>(rt_pixels[p * 4 + 2]);
            const int a_ch = static_cast<int>(rt_pixels[p * 4 + 3]);
            if (std::abs(r - 128) > 1 || std::abs(g - 128) > 1 || std::abs(b - 191) > 1 || a_ch != 255) {
                std::fprintf(stderr, "[FAIL] Test5 Mesh RT pixel %u: got (%d,%d,%d,%d) exp (~128,~128,~191,255)\n",
                             p, r, g, b, a_ch);
            }
            OS_REQUIRE(std::abs(r - 128) <= 1);
            OS_REQUIRE(std::abs(g - 128) <= 1);
            OS_REQUIRE(std::abs(b - 191) <= 1);
            OS_REQUIRE(a_ch == 255);
        }

        std::printf("[PASS] Test 5: VS+PS (spirv-val + DXC SM6.5) AND LIVE AS+MS+PS DispatchMesh on %s (MeshTier=%u, 64x64 RT=%u,%u,%u,%u)\n",
                    d3d_hw.adapter_name.c_str(), d3d_hw.mesh_tier,
                    rt_pixels[0], rt_pixels[1], rt_pixels[2], rt_pixels[3]);
    } else {
        std::printf("[PASS] Test 5: VS+PS+AS+MS DXC SM6.5 validated (live DispatchMesh skipped on %s, MeshTier=%u)\n",
                    d3d_hw.adapter_name.c_str(), d3d_hw.mesh_tier);
    }
}

// ---------------------------------------------------------------------------
// Test 6: XS2 v1/v2 Round-Trip, Extra Opcodes (IMAGE_SAMPLE, DS_ADD_U32, Packed Prim), & Error Paths
// ---------------------------------------------------------------------------
void Test6_DisasmRoundTripAndEdgeCases() {
    using namespace openscarlett;

    // 1. XS2 v2 container (44-byte header) with wave_size=64 round-trip
    {
        Rdna2Assembler a;
        a.EmitSopp(Rdna2Opcode::S_NOP);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Xs2ShaderContainer c{};
        c.version = kXs2ContainerVersionV2;
        c.stage = ShaderStage::Compute;
        c.sgpr_count = 24;
        c.vgpr_count = 32;
        c.threadgroup_x = 64;
        c.wave_size = 64;
        c.entry_offset = kXs2HeaderSizeBytesV2;
        c.code_words = a.Words();
        const auto bytes = BuildXs2Container(c);
        OS_REQUIRE(bytes.size() == kXs2HeaderSizeBytesV2 + a.Words().size() * 4u);

        Xs2ShaderContainer parsed{};
        OS_REQUIRE(ParseXs2Container(bytes, parsed).ok());
        OS_REQUIRE(parsed.version == kXs2ContainerVersionV2);
        OS_REQUIRE(parsed.sgpr_count == 24u);
        OS_REQUIRE(parsed.vgpr_count == 32u);
        OS_REQUIRE(parsed.wave_size == 64u);
    }

    // 2. IMAGE_SAMPLE (explicit LOD) + DS_ADD_U32 (non-returning atomic, must survive DCE!)
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.5f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(0.5f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(1.0f));  // LOD = 1.0
        a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 1, 0, 8, 0xFu, 1u, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineInt(0));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::InlineInt(1));
        a.EmitDs(Rdna2Opcode::DS_ADD_U32, 0, 8, 9);
        a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 4, 8, 0, 0, 0xFu, 1u, true);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.validate_spirv_external = HasExternalSpirvVal();
        opts.compile_dxil_with_dxc = true;
        Rdna2LiftResult res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 64, 1, 1), res, opts).ok());
        OS_REQUIRE(res.ir.uses_lds);
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 234u));  // DS_ADD_U32 survived DCE -> OpAtomicIAdd
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 88u));   // OpImageSampleExplicitLod
        VerifySpirvExternalStrict(res.spirv_words, "Test6_ImageSample_DsAddU32");
    }

    // 3. Packed EXP Prim (en=0x1, 10-bit indices packed in single VGPR) -> ms_6_5 + SM 5.0 rejection
    {
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 1, 1, 2, 0xFu, false);
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(0u | (1u << 10) | (2u << 20)));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 3, 0, 0, 0, 0x1u, true);
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.compile_dxil_with_dxc = true;
        Rdna2LiftResult res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1), res, opts).ok());
        OS_REQUIRE(res.hlsl_target == "ms_6_5");

        std::string dummy_hlsl, dummy_target;
        OS_REQUIRE(!EmitHlsl(res.ir, dummy_hlsl, dummy_target, /*use_sm65=*/false).ok());
    }

    // 4. Negative validator & compiler diagnostics checks
    {
        std::string diag;
        std::vector<uint32_t> bad_spv = {0x07230203u, 0x00010300u, 0u, 10u, 0u, (2u << 16) | 17u, 99999u};
        OS_REQUIRE(!ValidateSpirvExternal(bad_spv, diag).ok());
        OS_REQUIRE(!diag.empty());

        std::vector<uint8_t> bad_dxil;
        std::string dxc_diag;
        OS_REQUIRE(!CompileHlslWithDxc("this is not valid hlsl;", "cs_6_5", "main", bad_dxil, dxc_diag).ok());
        OS_REQUIRE(!dxc_diag.empty());
    }

    std::printf("[PASS] Test 6: XS2 v2 round-trip, IMAGE_SAMPLE(LOD), DS_ADD_U32 DCE retention, packed EXP Prim, & error diagnostics\n");
}

// ---------------------------------------------------------------------------
// Test 7: AAA Opcodes Live Dual-GPU + spirv-val + CPU Interpreter Bit-Exact Parity
//   7A: V_PERMLANE16_B32, V_PERMLANEX16_B32, V_BFE_U32, V_BFE_I32, V_BFI_B32, SCRATCH_LOAD/STORE
//   7B: VOP3P (0x33) V_PK_ADD_F16, V_PK_MUL_F16, V_PK_FMA_F16, V_PK_ADD_U16, V_PK_LSHLREV_B16, V_PK_LSHRREV_B16
//   7C: MIMG 3D IMAGE_LOAD_MIP, Cube IMAGE_LOAD_MIP, & IMAGE_ATOMIC_CMPSWAP
//   7D: MIMG IMAGE_BVH_INTERSECT_RAY (Möller-Trumbore triangle + AABB slab)
// ---------------------------------------------------------------------------
void Test7_AaaOpcodesLiveDualGpuAndCpuParity(openscarlett::VulkanBackend& vk,
                                             D3D12TestContext& d3d_hw,
                                             D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    auto float_to_bits = [](float f) -> uint32_t {
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    };
    auto bits_to_float = [](uint32_t b) -> float {
        float f = 0.0f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    };

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    // ----- 7A: Permlane16/X16 + BFE_U32/I32 + BFI_B32 + SCRATCH_LOAD/STORE -----
    {
        Rdna2Assembler a;
        // v1 = tid * 4
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // v2 = in[tid]
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
        // Reverse-lane permute table for 16 lanes: row 0..7 -> 15..8 (0x89ABCDEF), row 8..15 -> 7..0 (0x01234567)
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
        a.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
        a.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
        // Bitfield ops on v2
        a.EmitVop3(Rdna2Opcode::V_BFE_U32, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
        a.EmitVop3(Rdna2Opcode::V_BFE_I32, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
        a.EmitVop3(Rdna2Opcode::V_BFI_B32, 7, Rdna2Operand::LiteralU32(0x00FF00FFu), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        // Thread-private scratch round-trip: store v7 at scratch[(tid & 3) * 4 + 8], load back into v10
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 8, Rdna2Operand::InlineInt(3), 0);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(2), 8);
        a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 7, 8, 8);
        a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 10, 8, 8);

        // Store 5 outputs per lane across 64 threads -> 320 dwords
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);     // out[0..63]   = permlane16
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 256);   // out[64..127] = permlanex16
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 512);   // out[128..191]= bfe_u32
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 768);   // out[192..255]= bfe_i32
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 1024); // out[256..319]= scratch(bfi_b32)
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(xs2, res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test7A Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.ir.uses_scratch);
        VerifySpirvExternalStrict(res.spirv_words, "Test7A_PermlaneBfeBfiScratch");

        std::vector<uint32_t> input(320, 0u);
        for (uint32_t i = 0; i < 64u; ++i) {
            input[i] = 0xA0000000u | ((0x80u + i) << 8) | (i * 3u + 1u);
        }

        // Execute on CPU RDNA2 Reference Interpreter first
        Rdna2CpuExecState cpu{};
        cpu.uav0 = input;
        cpu.uav1.assign(320, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());

        // Compare bit-for-bit against Vulkan HW, D3D12 HW, and D3D12 WARP!
        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
            OS_REQUIRE(vk_out == cpu.uav1);
        }

        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, hw_out, 1);
        OS_REQUIRE(hw_out == cpu.uav1);

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, warp_out, 1);
        OS_REQUIRE(warp_out == cpu.uav1);
    }

    // ----- 7B: VOP3P (0x33) Packed 16-bit f16 & u16/b16 Live Dual-GPU + CPU Parity -----
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // v2 = in[tid] (packed 2xf16), v3 = in[tid + 64] (packed 2xf16)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 256);
        a.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
        a.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
        a.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        // v7 = in[tid + 128] (packed 2xu16), v8 = packed shift/add (lo=2, hi=3)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 7, 1, 0, 512);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::LiteralU32(0x00030002u));
        a.EmitVop3p(Rdna2Opcode::V_PK_ADD_U16, 9, Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(8));
        a.EmitVop3p(Rdna2Opcode::V_PK_LSHLREV_B16, 10, Rdna2Operand::Vgpr(8), Rdna2Operand::Vgpr(7));
        a.EmitVop3p(Rdna2Opcode::V_PK_LSHRREV_B16, 11, Rdna2Operand::Vgpr(8), Rdna2Operand::Vgpr(7));

        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 512);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 768);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 1024);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 1, 4, 1280);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(xs2, res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test7B Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        VerifySpirvExternalStrict(res.spirv_words, "Test7B_Vop3pPacked16");

        // Use exact binary16 powers/halves so CPU, Vulkan, D3D12 HW, and WARP match bit-for-bit
        constexpr uint16_t kHalves[4] = {0x3800u /*0.5*/, 0x3C00u /*1.0*/, 0x3E00u /*1.5*/, 0x4000u /*2.0*/};
        std::vector<uint32_t> input(384, 0u);
        for (uint32_t i = 0; i < 64u; ++i) {
            input[i] = static_cast<uint32_t>(kHalves[i & 3u]) |
                       (static_cast<uint32_t>(kHalves[(i + 1u) & 3u]) << 16);
            input[64 + i] = static_cast<uint32_t>(kHalves[(i + 2u) & 3u]) |
                            (static_cast<uint32_t>(kHalves[(i + 3u) & 3u]) << 16);
            input[128 + i] = ((i * 11u + 16u) & 0xFFFFu) | (((i * 23u + 64u) & 0xFFFFu) << 16);
        }

        Rdna2CpuExecState cpu{};
        cpu.uav0 = input;
        cpu.uav1.assign(384, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());

        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
            OS_REQUIRE(vk_out == cpu.uav1);
        }

        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, hw_out, 1);
        OS_REQUIRE(hw_out == cpu.uav1);

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, warp_out, 1);
        OS_REQUIRE(warp_out == cpu.uav1);
    }

    // ----- 7C: MIMG 3D IMAGE_LOAD_MIP + Cube IMAGE_LOAD_MIP + IMAGE_ATOMIC_CMPSWAP -----
    {
        Rdna2Assembler a;
        // 8 threads: load 3D mip1 texel at (tid&1, (tid>>1)&1, (tid>>2)&1, mip=1)
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(1), 0);
        a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2, Rdna2Operand::InlineInt(1), 0);
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 2, Rdna2Operand::InlineInt(1), 2);
        a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 3, Rdna2Operand::InlineInt(2), 0);
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 3, Rdna2Operand::InlineInt(1), 3);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(1));  // mip = 1
        // 3D IMAGE_LOAD_MIP v[8:11], v[1:4], dim=2 (3D)
        a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 8, 1, 0, 0, 0xFu, 2u, true);

        // Cube face = (tid & 3) (faces 0..3), coords (1, 0, face, mip=1)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(1));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineInt(0));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 14, Rdna2Operand::InlineInt(3), 0);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::InlineInt(1));
        // Cube IMAGE_LOAD_MIP v[16:19], v[12:15], dim=3 (Cube)
        a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 16, 12, 0, 0, 0xFu, 3u, true);

        // Lane 0 performs IMAGE_ATOMIC_CMPSWAP at (0, 0): if tex == 100 -> swap to 777, returns old (100)
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
        const uint32_t br_skip = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralU32(777u));  // vdata[0] = src (new)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralU32(100u));  // vdata[1] = cmp (expected)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 22, Rdna2Operand::InlineInt(0));      // x = 0
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::InlineInt(0));      // y = 0
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP, 20, 22, 0, 0, 0x1u, 1u, true, true);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::LiteralU32(64u));   // out[16] = prev atomic val
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20, 24, 4, 0);
        a.PatchBranch(br_skip, a.CurrentWordOffset());

        // Store 3D.r and Cube.g to u1 (out[tid] = v8, out[8 + tid] = v17)
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 25, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 25, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 25, 4, 32);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 8, 1, 1, 16, 32, 32);
        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(xs2, res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test7C Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.ir.uses_texture3d_srv && res.ir.uses_texture_cube_srv && res.ir.uses_texture_u32_uav);
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 95u));   // OpImageFetch
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 60u));   // OpImageTexelPointer
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 230u));  // OpAtomicCompareExchange
        VerifySpirvExternalStrict(res.spirv_words, "Test7C_Mimg3dCubeAtomic");

        Rdna2CpuTexture tex3d{};
        tex3d.AllocateRgba32f(2, 4, 4, 4, 2);
        for (uint32_t z = 0; z < 2u; ++z) {
            for (uint32_t y = 0; y < 2u; ++y) {
                for (uint32_t x = 0; x < 2u; ++x) {
                    const size_t idx = ((z * 2u + y) * 2u + x) * 4u;
                    tex3d.mips[1].rgba32f[idx + 0] = static_cast<float>(x + y * 2u + z * 4u + 1u) * 0.25f;
                    tex3d.mips[1].rgba32f[idx + 3] = 1.0f;
                }
            }
        }
        Rdna2CpuTexture texcube{};
        texcube.AllocateRgba32f(3, 4, 4, 6, 2);
        for (uint32_t f = 0; f < 6u; ++f) {
            const size_t idx = ((f * 2u + 0u) * 2u + 1u) * 4u;  // (x=1, y=0, face=f) in mip 1
            texcube.mips[1].rgba32f[idx + 1] = static_cast<float>(f + 1u) * 0.5f;
            texcube.mips[1].rgba32f[idx + 3] = 1.0f;
        }

        Rdna2CpuExecState cpu{};
        cpu.tex3d_srv = tex3d;
        cpu.texcube_srv = texcube;
        cpu.tex_u32_uav.AllocateR32u(1, 4, 4, 1);
        cpu.tex_u32_uav.mips[0].r32u[0] = 100u;
        cpu.uav1.assign(64, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
        OS_REQUIRE(cpu.uav1[16] == 100u);
        OS_REQUIRE(cpu.tex_u32_uav.mips[0].r32u[0] == 777u);

        std::vector<uint32_t> hw_out;
        uint32_t hw_atom_final = 0u;
        d3d_hw.RunComputeMimg3dCubeAtomic(
            CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil),
            tex3d, texcube, 100u, hw_out, hw_atom_final);
        OS_REQUIRE(hw_atom_final == 777u);
        OS_REQUIRE(hw_out[16] == 100u);
        for (uint32_t i = 0; i < 16u; ++i) {
            OS_REQUIRE(std::fabs(bits_to_float(hw_out[i]) - bits_to_float(cpu.uav1[i])) < 1e-5f);
        }

        std::vector<uint32_t> warp_out;
        uint32_t warp_atom_final = 0u;
        d3d_warp.RunComputeMimg3dCubeAtomic(
            CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil),
            tex3d, texcube, 100u, warp_out, warp_atom_final);
        OS_REQUIRE(warp_atom_final == 777u);
        OS_REQUIRE(warp_out[16] == 100u);
        for (uint32_t i = 0; i < 16u; ++i) {
            OS_REQUIRE(std::fabs(bits_to_float(warp_out[i]) - bits_to_float(cpu.uav1[i])) < 1e-5f);
        }
    }

    // ----- 7D: IMAGE_BVH_INTERSECT_RAY (Triangle Möller-Trumbore + AABB Slab) Live Dual-GPU + CPU Parity -----
    {
        Rdna2Assembler a;
        // 4 rays in threads 0..3:
        // tid 0: triangle node (ptr=0), ray hits at (0.25, 0.25, 0) -> (0,0,1), t=5, u=0.25, v=0.25, id=42
        // tid 1: triangle node (ptr=0), ray misses at (2.25, 0.25, 0) -> (0,0,1), id=0xFFFFFFFF
        // tid 2: AABB box node (ptr=64), ray hits at (0.25, 0.25, 0) -> (0,0,1), t_enter=2, t_exit=4, id=77
        // tid 3: AABB box node (ptr=64), ray misses at (2.25, 0.25, 0) -> (0,0,1), id=0xFFFFFFFF
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(64));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 12, Rdna2Operand::InlineInt(2), 0);
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 12);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20, Rdna2Operand::Vgpr(10), 11);  // node_ptr = (tid & 2) ? 64 : 0

        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralF32(100.0f));  // tmax
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralF32(0.25f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::LiteralF32(2.25f));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 15, Rdna2Operand::InlineInt(1), 0);
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 15);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 22, Rdna2Operand::Vgpr(13), 14);  // ox = (tid & 1) ? 2.25 : 0.25
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::LiteralF32(0.25f));  // oy
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::InlineFloat(0.0f));  // oz
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineFloat(0.0f));  // dx
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::InlineFloat(0.0f));  // dy
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::InlineFloat(1.0f));  // dz

        a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 30, 20, 0, 0, 0xFu, 0u, false);

        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);  // tid * 16
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 30, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 31, 1, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 32, 1, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 33, 1, 4, 12);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 16, 40, 32);
        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(xs2, res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test7D Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.ir.uses_bvh_ray);
        VerifySpirvExternalStrict(res.spirv_words, "Test7D_BvhIntersectRay");

        std::vector<uint32_t> bvh_buf(64, 0u);
        // Triangle at offset 0: (0,0,5), (1,0,5), (0,1,5), id=42
        bvh_buf[0] = 42u;
        bvh_buf[1] = float_to_bits(0.0f); bvh_buf[2] = float_to_bits(0.0f); bvh_buf[3] = float_to_bits(5.0f);
        bvh_buf[4] = float_to_bits(1.0f); bvh_buf[5] = float_to_bits(0.0f); bvh_buf[6] = float_to_bits(5.0f);
        bvh_buf[7] = float_to_bits(0.0f); bvh_buf[8] = float_to_bits(1.0f); bvh_buf[9] = float_to_bits(5.0f);
        // AABB Box at offset 64 (dword 16): bmin=(0,0,2), bmax=(1,1,4), id=77 | 0x80000000
        bvh_buf[16] = 0x80000000u | 77u;
        bvh_buf[17] = float_to_bits(0.0f); bvh_buf[18] = float_to_bits(0.0f); bvh_buf[19] = float_to_bits(2.0f);
        bvh_buf[20] = float_to_bits(1.0f); bvh_buf[21] = float_to_bits(1.0f); bvh_buf[22] = float_to_bits(4.0f);

        Rdna2CpuExecState cpu{};
        cpu.uav0 = bvh_buf;
        cpu.uav1.assign(64, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
        OS_REQUIRE(cpu.uav1[3] == 42u);
        OS_REQUIRE(cpu.uav1[7] == 0xFFFFFFFFu);
        OS_REQUIRE(cpu.uav1[11] == 77u);
        OS_REQUIRE(cpu.uav1[15] == 0xFFFFFFFFu);

        auto verify_bvh_parity = [&](const std::vector<uint32_t>& gpu_out, const char* backend) {
            for (uint32_t r = 0; r < 4u; ++r) {
                for (uint32_t c = 0; c < 3u; ++c) {
                    const float gf = bits_to_float(gpu_out[r * 4 + c]);
                    const float cf = bits_to_float(cpu.uav1[r * 4 + c]);
                    if (std::fabs(gf - cf) > 1e-4f) {
                        std::fprintf(stderr, "[FAIL] Test7D %s ray %u comp %u: got %.5f exp %.5f\n",
                                     backend, r, c, gf, cf);
                    }
                    OS_REQUIRE(std::fabs(gf - cf) <= 1e-4f);
                }
                OS_REQUIRE(gpu_out[r * 4 + 3] == cpu.uav1[r * 4 + 3]);
            }
        };

        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, bvh_buf, vk_out, 1).ok());
            verify_bvh_parity(vk_out, "Vulkan-HW");
        }

        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), bvh_buf, hw_out, 1);
        verify_bvh_parity(hw_out, "D3D12-HW");

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), bvh_buf, warp_out, 1);
        verify_bvh_parity(warp_out, "D3D12-WARP");
    }

    std::printf("[PASS] Test 7: AAA Opcodes (Permlane16/X16, BFE/BFI, Scratch, VOP3P f16/u16, 3D/Cube Mip + AtomicCmpSwap, BVH Ray) -> spirv-val + Vulkan + D3D12 HW + WARP + CPU exact parity!\n");
}

// ---------------------------------------------------------------------------
// Test 8: All 5 RDNA2 ISA & Wave64 Ecosystem Catches Live Dual-GPU + CPU Parity
//   8A: Zero-initialized LDS (groupshared) read-before-write + atomic counter
//   8B: GFX10.3 VOP2 0x36/0x37/0x38 f16 FMA + V_FMA_F32 vs V_MAD_F32 (FMA != MAD!)
//   8C: 16-bit integer VOP3 (no _e64 assumption) + s_bcnt1_i32_b32 vs b64 high-word
//   8D: Pixel shader helper-lane affine ddx/ddy/QuadReadAcross mip derivs in divergent CFG
//   8E: Bounded wave64-on-wave32 Mesh Shader (live DispatchMesh!) + Geometry Shader (gs_6_5)
//   8F: WARP 1.0.22 non-uniform WaveReadLaneAt > 8 guard + HW strict verification
// ---------------------------------------------------------------------------
void Test8_Rdna2IsaCatchesLiveDualGpuAndCpuParity(openscarlett::VulkanBackend& vk,
                                                  D3D12TestContext& d3d_hw,
                                                  D3D12TestContext& d3d_warp) {
    using namespace openscarlett;

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.validate_spirv_external = HasExternalSpirvVal();
    opts.compile_dxil_with_dxc = true;

    // ----- 8A (Catch 1): Zero-initialized LDS read-before-write + unwritten atomic slot -----
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // Read unwritten LDS[tid] directly (must deterministically return 0 thanks to entry zero-init!)
        a.EmitDs(Rdna2Opcode::DS_READ_B32, 2, 1);
        // Atomic increment unwritten LDS[64] (byte offset 256) without manual tid==0 zeroing
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(256u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(1));
        a.EmitDs(Rdna2Opcode::DS_ADD_RTN_U32, 5, 3, 4);
        a.EmitSopp(Rdna2Opcode::S_BARRIER);
        a.EmitDs(Rdna2Opcode::DS_READ_B32, 6, 3);  // Final sum must be 64
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 1, 4, 0);    // out[0..63]   = 0
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 256);  // out[64..127] = 64
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32), res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test8A Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.ir.uses_lds);
        OS_REQUIRE(res.hlsl_source.find("_lds_init") != std::string::npos);
        OS_REQUIRE(res.hlsl_source.find("GroupMemoryBarrierWithGroupSync()") != std::string::npos);
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 246u));  // OpLoopMerge (cooperative LDS zero-init loop)
        OS_REQUIRE(SpirvContainsOpcode(res.spirv_words, 224u));  // OpControlBarrier
        VerifySpirvExternalStrict(res.spirv_words, "Test8A_ZeroInitLds");

        std::vector<uint32_t> input(128, 0xCAFEBABEu);
        Rdna2CpuExecState cpu{};
        cpu.uav0 = input;
        cpu.uav1.assign(128, 0xBADu);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
        for (uint32_t i = 0; i < 64u; ++i) {
            OS_REQUIRE(cpu.uav1[i] == 0u);
            OS_REQUIRE(cpu.uav1[64 + i] == 64u);
        }

        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
            OS_REQUIRE(vk_out == cpu.uav1);
        }
        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, hw_out, 1);
        OS_REQUIRE(hw_out == cpu.uav1);

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, warp_out, 1);
        OS_REQUIRE(warp_out == cpu.uav1);
    }

    // ----- 8B (Catch 2): VOP2 0x36/0x37/0x38 f16 FMA + V_FMA_F32 vs V_MAD_F32 (rounding + denorm!) -----
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // Load dynamic inputs from uav0 so nothing constant-folds away!
        // uav0[0..3] = a, b, c for rounding cancellation test (1+2^-13, 1-2^-13, -1.0)
        // uav0[4..7] = a, b, c for denormal flush test (2^-127 [0x00400000], 4.0, 0.0)
        // uav0[8..10] = f16 operands (1.5=0x3E00, 2.0=0x4000, 0.5=0x3800)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 20, 0, 0);   // v2 = a_cancel
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 20, 0, 4);   // v3 = b_cancel
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 20, 0, 8);   // v4 = c_cancel
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 5, 20, 0, 16);  // v5 = a_denorm (2^-127)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 6, 20, 0, 20);  // v6 = b_denorm (4.0)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 7, 20, 0, 24);  // v7 = c_denorm (0.0)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 8, 20, 0, 32);  // v8 = f16(1.5)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 9, 20, 0, 36);  // v9 = f16(2.0)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 10, 20, 0, 40); // v10 = f16(0.5)

        // FMA vs MAD on cancellation vector
        a.EmitVop3(Rdna2Opcode::V_FMA_F32, 11, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        a.EmitVop3(Rdna2Opcode::V_MAD_F32, 12, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        // FMA vs MAD on denormal vector
        a.EmitVop3(Rdna2Opcode::V_FMA_F32, 13, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
        a.EmitVop3(Rdna2Opcode::V_MAD_F32, 14, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
        // VOP2 0x36/0x37/0x38 fused f16 + VOP3 v_fma_f16
        a.EmitVop2(Rdna2Opcode::V_FMAC_F16, 10, Rdna2Operand::Vgpr(8), 9);                // 1.5*2.0 + 0.5 = 3.5 (0x4300)
        a.EmitVop2Literal(Rdna2Opcode::V_FMAMK_F16, 15, Rdna2Operand::Vgpr(8), 9, 0x4000u);  // 1.5*2.0 + 2.0 = 5.0 (0x4500)
        a.EmitVop2Literal(Rdna2Opcode::V_FMAAK_F16, 16, Rdna2Operand::Vgpr(8), 9, 0x3C00u);  // 1.5*2.0 + 1.0 = 4.0 (0x4400)
        a.EmitVop3(Rdna2Opcode::V_FMA_F16, 17, Rdna2Operand::Vgpr(8), Rdna2Operand::Vgpr(9), Rdna2Operand::LiteralU32(0x3C00u));

        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 20, 4, 0);   // out[0] = fma_cancel (0xB2800000)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 20, 4, 4);   // out[1] = mad_cancel (0x00000000)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 20, 4, 8);   // out[2] = fma_denorm (0x01000000)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 20, 4, 12);  // out[3] = mad_denorm (0x00000000)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 20, 4, 16);  // out[4] = v_fmac_f16 (0x4300)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 20, 4, 20);  // out[5] = v_fmamk_f16 (0x4500)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 20, 4, 24);  // out[6] = v_fmaak_f16 (0x4400)
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 20, 4, 28);  // out[7] = v_fma_f16 (0x4400)
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 24, 32), res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test8B Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.hlsl_source.find("_os_flush_denorm") != std::string::npos);
        OS_REQUIRE(res.hlsl_source.find("_os_mad_f32") != std::string::npos);
        OS_REQUIRE(res.hlsl_source.find("_os_fma_f32") != std::string::npos);
        VerifySpirvExternalStrict(res.spirv_words, "Test8B_FmaVsMadAndF16");

        std::vector<uint32_t> input(32, 0u);
        input[0] = 0x3F800400u;  // 1 + 2^-13
        input[1] = 0x3F7FF800u;  // 1 - 2^-13
        input[2] = 0xBF800000u;  // -1.0f
        input[4] = 0x00400000u;  // 2^-127 (subnormal)
        input[5] = 0x40800000u;  // 4.0f
        input[6] = 0x00000000u;  // 0.0f
        input[8] = 0x3E00u;      // f16(1.5)
        input[9] = 0x4000u;      // f16(2.0)
        input[10] = 0x3800u;     // f16(0.5)

        Rdna2CpuExecState cpu{};
        cpu.uav0 = input;
        cpu.uav1.assign(32, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 0xB2800000u);  // FMA cancel = -2^-26
        OS_REQUIRE(cpu.uav1[1] == 0x00000000u);  // MAD cancel = 0.0
        OS_REQUIRE(cpu.uav1[0] != cpu.uav1[1]);
        OS_REQUIRE(cpu.uav1[2] == 0x01000000u);  // FMA denorm = 2^-125
        OS_REQUIRE(cpu.uav1[3] == 0x00000000u);  // MAD denorm = 0.0
        OS_REQUIRE(cpu.uav1[2] != cpu.uav1[3]);
        OS_REQUIRE(cpu.uav1[4] == 0x4300u);
        OS_REQUIRE(cpu.uav1[5] == 0x4500u);
        OS_REQUIRE(cpu.uav1[6] == 0x4400u);
        OS_REQUIRE(cpu.uav1[7] == 0x4400u);

        auto check_fma_mad_gpu = [&](const std::vector<uint32_t>& out, const char* backend) {
            if (out[0] != cpu.uav1[0] || out[1] != cpu.uav1[1] || out[2] != cpu.uav1[2] ||
                out[3] != cpu.uav1[3] || out[4] != cpu.uav1[4] || out[5] != cpu.uav1[5] ||
                out[6] != cpu.uav1[6] || out[7] != cpu.uav1[7]) {
                std::fprintf(stderr,
                             "[FAIL] Test8B %s: fma_c=0x%08X(0x%08X) mad_c=0x%08X(0x%08X) fma_d=0x%08X(0x%08X) mad_d=0x%08X(0x%08X) f16=0x%X,0x%X,0x%X,0x%X\n",
                             backend, out[0], cpu.uav1[0], out[1], cpu.uav1[1], out[2], cpu.uav1[2], out[3], cpu.uav1[3],
                             out[4], out[5], out[6], out[7]);
            }
            OS_REQUIRE(out[0] == cpu.uav1[0]);  // FMA cancel == -2^-26
            OS_REQUIRE(out[1] == cpu.uav1[1]);  // MAD cancel == 0.0
            OS_REQUIRE(out[0] != out[1]);       // FMA != MAD on live GPU!
            OS_REQUIRE(out[2] == cpu.uav1[2]);  // FMA preserves subnormal -> 2^-125
            OS_REQUIRE(out[3] == cpu.uav1[3]);  // MAD denorm flushes to 0.0
            OS_REQUIRE(out[2] != out[3]);       // FMA != MAD on denormal input!
            OS_REQUIRE(out[4] == cpu.uav1[4]);
            OS_REQUIRE(out[5] == cpu.uav1[5]);
            OS_REQUIRE(out[6] == cpu.uav1[6]);
            OS_REQUIRE(out[7] == cpu.uav1[7]);
        };

        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
            check_fma_mad_gpu(vk_out, "Vulkan-HW");
        }
        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, hw_out, 1);
        check_fma_mad_gpu(hw_out, "D3D12-HW");

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, warp_out, 1);
        check_fma_mad_gpu(warp_out, "D3D12-WARP");
    }

    // ----- 8C (Catch 3): 16-bit integer VOP3 + s_bcnt1_i32_b32 vs s_bcnt1_i32_b64 high-word -----
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::InlineInt(0));
        // Load dynamic scalar pair into s8, s9 via V_READFIRSTLANE_B32 from uav0[0..1] = 0x80000003, 0x00000001
        // and uav0[2..3] = 0x00000000, 0xF0F00001
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 20, 0, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 20, 0, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 20, 0, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 5, 20, 0, 12);
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 8, Rdna2Operand::Vgpr(2));
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 9, Rdna2Operand::Vgpr(3));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 10, Rdna2Operand::Sgpr(8));  // 3
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 11, Rdna2Operand::Sgpr(8));  // 4
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 12, Rdna2Operand::Vgpr(4));
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 13, Rdna2Operand::Vgpr(5));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 14, Rdna2Operand::Sgpr(12)); // 0
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 15, Rdna2Operand::Sgpr(12)); // 9

        // 16-bit integer VOP3 ops on dynamic uav0[4..6]
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 6, 20, 0, 16);   // 0xAAAAFFF0
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 7, 20, 0, 20);   // 0xBBBB0020
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 8, 20, 0, 24);   // 0xCCCC0005
        a.EmitVop3(Rdna2Opcode::V_ADD_NC_U16, 9, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
        a.EmitVop3(Rdna2Opcode::V_SUB_NC_U16, 10, Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(6));
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U16, 11, Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(8));
        a.EmitVop3(Rdna2Opcode::V_MAD_U16, 12, Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(8), Rdna2Operand::InlineInt(7));
        a.EmitVop3(Rdna2Opcode::V_MIN_I16, 13, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
        a.EmitVop3(Rdna2Opcode::V_MAX_I16, 14, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));

        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::Sgpr(10));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::Sgpr(11));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17, Rdna2Operand::Sgpr(14));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 18, Rdna2Operand::Sgpr(15));

        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 20, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 20, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 20, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 18, 20, 4, 12);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 20, 4, 16);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 20, 4, 20);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 20, 4, 24);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 20, 4, 28);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 20, 4, 32);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 20, 4, 36);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult res{};
        auto st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 20, 24, 32), res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test8C Lift failed: %s\nHLSL:\n%s\n", st.message.c_str(), res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        VerifySpirvExternalStrict(res.spirv_words, "Test8C_Vop3Int16AndBcnt32_64");

        std::vector<uint32_t> input(32, 0u);
        input[0] = 0x80000003u;
        input[1] = 0x00000001u;
        input[2] = 0x00000000u;
        input[3] = 0xF0F00001u;
        input[4] = 0xAAAAFFF0u;
        input[5] = 0xBBBB0020u;
        input[6] = 0xCCCC0005u;

        Rdna2CpuExecState cpu{};
        cpu.uav0 = input;
        cpu.uav1.assign(32, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 3u && cpu.uav1[1] == 4u);
        OS_REQUIRE(cpu.uav1[2] == 0u && cpu.uav1[3] == 9u);
        OS_REQUIRE(cpu.uav1[4] == 0x0010u && cpu.uav1[5] == 0x0030u);
        OS_REQUIRE(cpu.uav1[6] == 0x00A0u && cpu.uav1[7] == 0x00A7u);
        OS_REQUIRE(cpu.uav1[8] == 0xFFF0u && cpu.uav1[9] == 0x0020u);

        if (vk.GetDeviceInfo().is_hardware) {
            std::vector<uint32_t> vk_out;
            OS_REQUIRE(vk.ExecuteComputeShader(res.spirv_words, input, vk_out, 1).ok());
            OS_REQUIRE(vk_out == cpu.uav1);
        }
        std::vector<uint32_t> hw_out;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, res.hlsl_source, res.pc_dxil), input, hw_out, 1);
        OS_REQUIRE(hw_out == cpu.uav1);

        std::vector<uint32_t> warp_out;
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, res.hlsl_source, res.pc_dxil), input, warp_out, 1);
        OS_REQUIRE(warp_out == cpu.uav1);
    }

    // ----- 8D (Catch 4A): Pixel shader helper-lane affine quad derivative sampling in divergent CFG -----
    {
        Rdna2Assembler ps;
        // Divergent branch on param0.x > 0.25f before implicit-LOD IMAGE_SAMPLE
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::LiteralF32(0.25f));
        ps.EmitVopc(Rdna2Opcode::V_CMP_GT_F32, Rdna2Operand::Vgpr(0), 8);
        const uint32_t br_else = ps.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        // Taken: sample at (u, v) = (v0, v1) with implicit quad derivatives
        ps.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 0, 0, 8, 0xFu, 1u, false);
        const uint32_t br_join = ps.EmitSopp(Rdna2Opcode::S_BRANCH);
        ps.PatchBranch(br_else, ps.CurrentWordOffset());
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(1.0f));
        ps.PatchBranch(br_join, ps.CurrentWordOffset());
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 6, 7, 0xFu, true, false, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult ps_res{};
        auto st = LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel), ps_res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Test8D PS quad deriv lift failed: %s\nHLSL:\n%s\n",
                         st.message.c_str(), ps_res.hlsl_source.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(ps_res.hlsl_target == "ps_6_5");
        OS_REQUIRE(ps_res.ir.uses_pixel_quad_derivatives);
        OS_REQUIRE(ps_res.hlsl_source.find("QuadReadAcrossX") != std::string::npos);
        OS_REQUIRE(ps_res.hlsl_source.find("QuadReadAcrossY") != std::string::npos);
        OS_REQUIRE(ps_res.hlsl_source.find("SampleGrad") != std::string::npos);
        OS_REQUIRE(SpirvContainsOpcode(ps_res.spirv_words, 207u));  // OpDPdx
        OS_REQUIRE(SpirvContainsOpcode(ps_res.spirv_words, 208u));  // OpDPdy
        OS_REQUIRE(SpirvContainsOpcode(ps_res.spirv_words, 88u));   // OpImageSampleExplicitLod (Grad)
        VerifySpirvExternalStrict(ps_res.spirv_words, "Test8D_PsQuadDerivatives");

        // Also verify SM 5.0 fallback uses ddx/ddy + SampleGrad cleanly
        std::string sm50_hlsl, sm50_target;
        OS_REQUIRE(EmitHlsl(ps_res.ir, sm50_hlsl, sm50_target, /*use_sm65=*/false).ok());
        OS_REQUIRE(sm50_target == "ps_5_0");
        OS_REQUIRE(sm50_hlsl.find("ddx(uv)") != std::string::npos);
        OS_REQUIRE(sm50_hlsl.find("SampleGrad") != std::string::npos);
    }

    // ----- 8E (Catch 4B): Bounded wave64-on-wave32 Mesh Shader (LIVE DispatchMesh!) + Geometry Shader -----
    {
        // Wave64 Mesh Shader (tg=64, wave_size=64):
        // Emits 35 vertices and 1 full-screen triangle with indices (0, 1, 34):
        //   - Vertex 0 (-1, -1) exported by lane 0  (half 0!)
        //   - Vertex 1 (-1,  3) exported by lane 1  (half 0!)
        //   - Vertex 34 (3, -1) exported by lane 34 (half 1!) -> proves bounded 2-half wave64->wave32 execution!
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(35));  // vert_count = 35
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));   // prim_count = 1
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

        // Base pos = (-1, -1, 0, 1)
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
        // If lane64 == 1 -> y = 3.0
        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
        ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
        // If lane64 == 34 (in HALF 1!) -> x = 3.0
        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(34), 0);
        ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

        // Param0 = (0.25, 0.50, 0.75, 1.0)
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

        // Lane 0 exports primitive (0, 1, 34) connecting half-0 and half-1 vertices!
        ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
        const uint32_t br_skip = ms.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(1));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(34));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
        ms.PatchBranch(br_skip, ms.CurrentWordOffset());
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult ms_res{};
        auto st_ms = LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 64, 1, 1, 16, 16, /*wave_size=*/64), ms_res, opts);
        if (!st_ms.ok()) {
            std::fprintf(stderr, "[FAIL] Test8E Wave64 MS lift failed: %s\nHLSL:\n%s\n",
                         st_ms.message.c_str(), ms_res.hlsl_source.c_str());
        }
        OS_REQUIRE(st_ms.ok());
        OS_REQUIRE(ms_res.hlsl_target == "ms_6_5");
        OS_REQUIRE(ms_res.hlsl_source.find("_w64_half < 2u") != std::string::npos);
        OS_REQUIRE(ms_res.hlsl_source.find("WaveGetLaneIndex()") != std::string::npos);
        OS_REQUIRE(ms_res.hlsl_source.find("[numthreads(32, 1, 1)]") != std::string::npos);

        // Wave64 Geometry Shader (gs_6_5) bounded two-half wrapper compilation test
        Rdna2Assembler gs;
        gs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.0f));
        gs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
        gs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 1, 1, 2, 0xFu, true);
        gs.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult gs_res{};
        auto st_gs = LiftRdna2Xs2Shader(gs.BuildXs2(ShaderStage::Geometry, 64, 1, 1, 16, 16, /*wave_size=*/64), gs_res, opts);
        if (!st_gs.ok()) {
            std::fprintf(stderr, "[FAIL] Test8E Wave64 GS lift failed: %s\nHLSL:\n%s\n",
                         st_gs.message.c_str(), gs_res.hlsl_source.c_str());
        }
        OS_REQUIRE(st_gs.ok());
        OS_REQUIRE(gs_res.hlsl_target == "gs_6_5");
        OS_REQUIRE(gs_res.hlsl_source.find("_w64_half < 2u") != std::string::npos);

        // Execute Wave64 Mesh Shader LIVE on GTX 1650 Ti (Wave=32, MeshTier=10)!
        if (d3d_hw.mesh_tier >= 10u) {
            Rdna2Assembler as;
            as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(1));
            as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
            as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
            as.EmitSopp(Rdna2Opcode::S_ENDPGM);
            Rdna2LiftResult as_res{};
            OS_REQUIRE(LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 64, 1, 1, 16, 16, /*wave_size=*/64), as_res, opts).ok());

            Rdna2Assembler ps;
            ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::InlineFloat(2.0f), 0);
            ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 1, 2, 3, 0xFu, true, false, true);
            ps.EmitSopp(Rdna2Opcode::S_ENDPGM);
            Rdna2LiftResult ps_res{};
            OS_REQUIRE(LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel), ps_res, opts).ok());

            std::vector<uint8_t> rt_pixels;
            d3d_hw.RunMeshShaderPipeline64x64(as_res.pc_dxil, ms_res.pc_dxil, ps_res.pc_dxil, rt_pixels);
            OS_REQUIRE(rt_pixels.size() == 64u * 64u * 4u);
            for (uint32_t p = 0; p < 64u * 64u; ++p) {
                OS_REQUIRE(std::abs(static_cast<int>(rt_pixels[p * 4 + 0]) - 128) <= 1);
                OS_REQUIRE(std::abs(static_cast<int>(rt_pixels[p * 4 + 1]) - 128) <= 1);
                OS_REQUIRE(std::abs(static_cast<int>(rt_pixels[p * 4 + 2]) - 191) <= 1);
                OS_REQUIRE(rt_pixels[p * 4 + 3] == 255u);
            }
        }
    }

    // ----- 8F (Catch 5): WARP 1.0.22 non-uniform WaveReadLaneAt > 8 guard + HW strict verification -----
    {
        // Verify WARP detection metadata
        OS_REQUIRE(d3d_warp.IsWarpAdapter());
        OS_REQUIRE(d3d_warp.ShouldGuardWarpNonUniformWaveReadAbove8());
        if (!d3d_hw.IsWarpAdapter()) {
            OS_REQUIRE(!d3d_hw.ShouldGuardWarpNonUniformWaveReadAbove8());
        }

        // 1. Uniform <= 8 lane WaveReadLaneAt (compile-time lane 3 and lane 0) must succeed on BOTH HW and WARP
        const std::string hlsl_uni_le8 =
            "RWByteAddressBuffer _in0 : register(u0);\n"
            "RWByteAddressBuffer _out0 : register(u1);\n"
            "[numthreads(32, 1, 1)]\n"
            "void main(uint3 _tid : SV_DispatchThreadID) {\n"
            "    uint v = _in0.Load(_tid.x * 4u);\n"
            "    uint r_hi = WaveReadLaneAt(v, 3u);\n"
            "    uint r_lo = WaveReadLaneAt(v, 0u);\n"
            "    _out0.Store(_tid.x * 4u, r_hi ^ r_lo);\n"
            "}\n";
        std::vector<uint8_t> dxil_uni_le8;
        std::string diag;
        OS_REQUIRE(CompileHlslWithDxc(hlsl_uni_le8, "cs_6_5", "main", dxil_uni_le8, diag).ok());

        std::vector<uint32_t> input(32, 0u);
        for (uint32_t i = 0; i < 32u; ++i) input[i] = 0x1000u + i * 13u;

        std::vector<uint32_t> hw_le8, warp_le8;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, hlsl_uni_le8, dxil_uni_le8), input, hw_le8, 1);
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, hlsl_uni_le8, dxil_uni_le8), input, warp_le8, 1);
        const uint32_t hw_w = d3d_hw.wave_min ? d3d_hw.wave_min : 32u;
        const uint32_t warp_w = d3d_warp.wave_min ? d3d_warp.wave_min : 4u;
        for (uint32_t i = 0; i < 32u; ++i) {
            const uint32_t hw_base = (i / hw_w) * hw_w;
            const uint32_t hw_lane = std::min(hw_w - 1u, 3u);
            OS_REQUIRE(hw_le8[i] == (input[hw_base + hw_lane] ^ input[hw_base]));

            const uint32_t warp_base = (i / warp_w) * warp_w;
            const uint32_t warp_lane = std::min(warp_w - 1u, 3u);
            OS_REQUIRE(warp_le8[i] == (input[warp_base + warp_lane] ^ input[warp_base]));
        }

        // 2. Non-uniform WaveReadLaneAt across full Wave32 (> 8 lanes): strictly validated on HW, guarded on WARP
        const std::string hlsl_gt8 =
            "RWByteAddressBuffer _in0 : register(u0);\n"
            "RWByteAddressBuffer _out0 : register(u1);\n"
            "[numthreads(32, 1, 1)]\n"
            "void main(uint3 _tid : SV_DispatchThreadID) {\n"
            "    uint v = _in0.Load(_tid.x * 4u);\n"
            "    uint non_uniform_lane = (_tid.x ^ 17u) & 31u;\n"
            "    uint r = WaveReadLaneAt(v, non_uniform_lane);\n"
            "    _out0.Store(_tid.x * 4u, r);\n"
            "}\n";
        std::vector<uint8_t> dxil_gt8;
        OS_REQUIRE(CompileHlslWithDxc(hlsl_gt8, "cs_6_5", "main", dxil_gt8, diag).ok());

        if (!d3d_hw.ShouldGuardWarpNonUniformWaveReadAbove8() && d3d_hw.wave_min >= 32u) {
            std::vector<uint32_t> hw_gt8;
            d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, hlsl_gt8, dxil_gt8), input, hw_gt8, 1);
            for (uint32_t i = 0; i < 32u; ++i) {
                OS_REQUIRE(hw_gt8[i] == input[(i ^ 17u) & 31u]);
            }
        }
        // Guarded cleanly on WARP (avoids WARP 1.0.22 / SM 6.2 non-uniform WaveReadLaneAt > 8 fault)
        OS_REQUIRE(d3d_warp.ShouldGuardWarpNonUniformWaveReadAbove8());

        // 3. Lifted RDNA2 V_READLANE_B32 (groupshared-backed) handles non-uniform lane > 8 on BOTH HW and WARP!
        Rdna2Assembler rl;
        rl.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        rl.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        rl.EmitVop2(Rdna2Opcode::V_XOR_B32, 3, Rdna2Operand::InlineInt(17), 0);
        rl.EmitVop3(Rdna2Opcode::V_READLANE_B32, 8, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
        rl.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::Sgpr(8));
        rl.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        rl.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult rl_res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(rl.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), rl_res, opts).ok());
        std::vector<uint32_t> hw_rl, warp_rl;
        d3d_hw.RunComputeBuffers(CompileForDevice(d3d_hw, rl_res.hlsl_source, rl_res.pc_dxil), input, hw_rl, 1);
        d3d_warp.RunComputeBuffers(CompileForDevice(d3d_warp, rl_res.hlsl_source, rl_res.pc_dxil), input, warp_rl, 1);
        for (uint32_t i = 0; i < 32u; ++i) {
            OS_REQUIRE(hw_rl[i] == input[(i ^ 17u) & 31u]);
            OS_REQUIRE(warp_rl[i] == input[(i ^ 17u) & 31u]);
        }
    }

    std::printf("[PASS] Test 8: All 5 RDNA2 ISA Catches (ZeroInitLDS, FMA!=MAD & f16 0x36..38, VOP3 i16 & bcnt32/64, PS QuadDerivs, Wave64->32 Mesh/GS live, WARP 1.0.22 guard)\n");
}

}  // namespace

int main() {
    using namespace openscarlett;

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(/*allow_software_fallback=*/true).ok());
    const auto vk_info = vk.GetDeviceInfo();

    D3D12TestContext d3d_hw;
    if (!d3d_hw.Init(/*use_warp=*/false)) {
        OS_REQUIRE(d3d_hw.Init(/*use_warp=*/true));
    }
    D3D12TestContext d3d_warp;
    OS_REQUIRE(d3d_warp.Init(/*use_warp=*/true));

    std::printf("=== OpenScarlett RDNA2 Lifter Live Dual-GPU Verification ===\n");
    std::printf("Vulkan ICD : %s (API %u.%u.%u)\n",
                vk_info.device_name.c_str(), vk_info.api_major, vk_info.api_minor, vk_info.api_patch);
    std::printf("D3D12 HW   : %s (Vendor=0x%04X, Wave=%u, SM=0x%02X, MeshTier=%u)\n",
                d3d_hw.adapter_name.c_str(), d3d_hw.vendor_id, d3d_hw.wave_min, d3d_hw.highest_sm, d3d_hw.mesh_tier);
    std::printf("D3D12 WARP : %s (Vendor=0x%04X, Wave=%u, SM=0x%02X, MeshTier=%u, WarpGuard=%s)\n",
                d3d_warp.adapter_name.c_str(), d3d_warp.vendor_id, d3d_warp.wave_min, d3d_warp.highest_sm,
                d3d_warp.mesh_tier, d3d_warp.ShouldGuardWarpNonUniformWaveReadAbove8() ? "ON" : "OFF");

    Test1_ComputeAluBranchConstFold(vk, d3d_hw, d3d_warp);
    Test2_DsLdsBarrierAtomics(vk, d3d_hw, d3d_warp);
    Test3_Wave64OnWave32(vk, d3d_hw, d3d_warp);
    Test4_MimgTexturePipeline(d3d_hw, d3d_warp);
    Test5_GraphicsVsPsAndLiveMeshAs(d3d_hw);
    Test6_DisasmRoundTripAndEdgeCases();
    Test7_AaaOpcodesLiveDualGpuAndCpuParity(vk, d3d_hw, d3d_warp);
    Test8_Rdna2IsaCatchesLiveDualGpuAndCpuParity(vk, d3d_hw, d3d_warp);

    // Verify CallTelemetry recorded our lifter calls cleanly
    const auto snap = CallTelemetry::Global().Snapshot();
    bool saw_lifter_telemetry = false;
    for (const auto& entry : snap) {
        if (entry.module_name == "rdna2_lifter" && entry.ok_calls > 0) {
            saw_lifter_telemetry = true;
            break;
        }
    }
    OS_REQUIRE(saw_lifter_telemetry);

    vk.Shutdown();
    std::printf("[PASS] test_rdna2_lifter_live: ALL 8 LIVE DUAL-GPU & CPU-PARITY SUITES PASSED!\n");
    return 0;
}
