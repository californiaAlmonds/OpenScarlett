#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/shader_cache.h"
#include "os_check.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>
#endif

namespace {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeVal, typename InnerType>
struct alignas(void*) AutoLiftSubobj {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = TypeVal;
    InnerType inner{};
};

struct StandaloneMeshPsoStream {
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_sig;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
    AutoLiftSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct DeviceXHarness {
    ComPtr<ID3D12DeviceX> device;
    ComPtr<ID3D12CommandQueueX> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandListX> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;
    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};

    ~DeviceXHarness() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    void Init(bool use_warp) {
        OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);
        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1;
        OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(
            nullptr, &params, IID_ID3D12DeviceX,
            reinterpret_cast<void**>(device.ReleaseAndGetAddressOf()))));
        OS_REQUIRE(device.Get() != nullptr);
        OS_REQUIRE(SUCCEEDED(device->GetGpuHardwareInfoX(&gpu_info)));
        OS_REQUIRE((gpu_info.IsWarp != 0u) == use_warp);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        OS_REQUIRE(SUCCEEDED(device->CreateCommandQueue(
            &qd, IID_ID3D12CommandQueueX,
            reinterpret_cast<void**>(queue.ReleaseAndGetAddressOf()))));
        OS_REQUIRE(SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        OS_REQUIRE(SUCCEEDED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_ID3D12GraphicsCommandListX,
            reinterpret_cast<void**>(cmd.ReleaseAndGetAddressOf()))));
        OS_REQUIRE(SUCCEEDED(cmd->Close()));
        OS_REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(fence_event != nullptr);
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
                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
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
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res))));
        return res;
    }

    ComPtr<ID3D12RootSignature> CreateDualUavRootSignature() {
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = params;
        ComPtr<ID3DBlob> sig, err;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs))));
        return rs;
    }

    ComPtr<ID3D12RootSignature> CreateEmptyRootSignature() {
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        ComPtr<ID3DBlob> sig, err;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(
            0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs))));
        return rs;
    }

    void RunComputeShader(const std::vector<uint8_t>& xbox_or_host_cs,
                          const std::vector<uint32_t>& in_data,
                          std::vector<uint32_t>& out_data) {
        const uint64_t byte_size = static_cast<uint64_t>(in_data.size()) * sizeof(uint32_t);
        out_data.assign(in_data.size(), 0u);

        auto root_sig = CreateDualUavRootSignature();
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {xbox_or_host_cs.data(), xbox_or_host_cs.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] DeviceX::CreateComputePipelineState failed on %s: hr=0x%08lX\n",
                         gpu_info.AdapterName, static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));
        OS_REQUIRE(pso.Get() != nullptr);

        auto in_up = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuffer(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(byte_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(in_up->Map(0, nullptr, &mp)));
        std::memcpy(mp, in_data.data(), static_cast<size_t>(byte_size));
        in_up->Unmap(0, nullptr);
        OS_REQUIRE(SUCCEEDED(out_up->Map(0, nullptr, &mp)));
        std::memset(mp, 0, static_cast<size_t>(byte_size));
        out_up->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd->Reset(allocator.Get(), pso.Get())));
        cmd->CopyResource(in_gpu.Get(), in_up.Get());
        cmd->CopyResource(out_gpu.Get(), out_up.Get());

        D3D12_RESOURCE_BARRIER to_uav[2]{};
        to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd->ResourceBarrier(2, to_uav);

        cmd->SetComputeRootSignature(root_sig.Get());
        cmd->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
        cmd->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER uav_b{};
        uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_b.UAV.pResource = out_gpu.Get();
        cmd->ResourceBarrier(1, &uav_b);

        D3D12_RESOURCE_BARRIER to_cp{};
        to_cp.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_cp.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &to_cp);
        cmd->CopyResource(rb.Get(), out_gpu.Get());
        OS_REQUIRE(SUCCEEDED(cmd->Close()));

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &mp)));
        std::memcpy(out_data.data(), mp, static_cast<size_t>(byte_size));
        rb->Unmap(0, nullptr);
    }

    void RenderFullscreenQuad64x64(ID3D12RootSignature* root_sig,
                                   ID3D12PipelineState* pso,
                                   bool use_dispatch_mesh,
                                   std::vector<uint8_t>& out_rgba8) {
        constexpr UINT kW = 64, kH = 64, kPitch = kW * 4, kTotal = kPitch * kH;
        out_rgba8.assign(kTotal, 0u);

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
        ComPtr<ID3D12Resource> rt;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp_def, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt))));

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);

        auto rb = CreateBuffer(kTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd->Reset(allocator.Get(), pso)));
        const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd->ClearRenderTargetView(rtv_h, clr, 0, nullptr);
        cmd->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->SetGraphicsRootSignature(root_sig);
        cmd->SetPipelineState(pso);

        if (use_dispatch_mesh) {
            cmd->DispatchMesh(1, 1, 1);
        } else {
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawInstanced(3, 1, 0, 0);
        }

        D3D12_RESOURCE_BARRIER to_cp{};
        to_cp.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_cp.Transition = {rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &to_cp);

        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = rt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kPitch};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        OS_REQUIRE(SUCCEEDED(cmd->Close()));

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &mp)));
        std::memcpy(out_rgba8.data(), mp, kTotal);
        rb->Unmap(0, nullptr);
    }
};

std::vector<uint8_t> BuildComputeXs2(uint32_t mul_const, uint32_t add_const) {
    using namespace openscarlett;
    Rdna2Assembler a;
    // v1 = tid * 4; v2 = in[tid]; v3 = v2 * mul_const + add_const; v4 = bfi(0x00FF0000, v3, v2); out[tid] = v4
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::LiteralU32(mul_const));
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::LiteralU32(add_const), 3);
    a.EmitVop3(Rdna2Opcode::V_BFI_B32, 4, Rdna2Operand::LiteralU32(0x00FFFF00u), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(2));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
}

std::vector<uint8_t> BuildFullscreenVsXs2() {
    using namespace openscarlett;
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
    // Param0 = (0.25, 0.50, 0.75, 1.0)
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.25f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.75f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return vs.BuildXs2(ShaderStage::Vertex);
}

std::vector<uint8_t> BuildFullscreenPsXs2() {
    using namespace openscarlett;
    Rdna2Assembler ps;
    // Doubles R (0.25 -> 0.50 -> ~128), G=0.50 (~128), B=0.75 (~191), A=1.0 (255)
    ps.EmitVop2(Rdna2Opcode::V_MUL_F32, 4, Rdna2Operand::InlineFloat(2.0f), 0);
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 1, 2, 3, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return ps.BuildXs2(ShaderStage::Pixel);
}

std::vector<uint8_t> BuildFullscreenMsXs2() {
    using namespace openscarlett;
    Rdna2Assembler ms;
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
    ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

    // Param0 = (0.375, 0.25, 0.875, 1.0) -> PS doubles R -> (0.75=~191, 0.25=~64, 0.875=~223, 255)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralF32(0.375f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralF32(0.25f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.875f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 3, 4, 0xFu, false);
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, false);

    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0);
    const uint32_t br_skip = ms.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(1));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(2));
    ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 11, 12, 0, 0x7u, true);
    ms.PatchBranch(br_skip, ms.CurrentWordOffset());
    ms.EmitSopp(Rdna2Opcode::S_ENDPGM);
    return ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1);
}

uint64_t GetDllTelemetryCount(const char* sym) {
    int32_t slot = -1;
    uint64_t total = 0;
    if (OpenScarlettD3D12GetTelemetryEntry(sym, &slot, &total)) {
        return total;
    }
    return 0;
}

bool HasHardwareGpu(openscarlett::D3D12AdapterInfo& out_info) {
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    ComPtr<ID3D12Device> dev;
    ComPtr<IDXGIAdapter1> ad;
    auto st = openscarlett::SelectD3D12AdapterAndCreateDevice(opts, &dev, &ad, &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

#endif  // _WIN32

}  // namespace

int main() {
#if !defined(_WIN32)
    std::printf("[SKIP] test_d3d12_xs_autolift requires Windows\n");
    return 0;
#else
    using namespace openscarlett;

    // Put isolated shader cache strictly under %TEMP% (E:\ under build_wt.cmd)
    std::filesystem::path cache_dir;
    {
        char env_buf[1024]{};
        DWORD len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
        if (len > 0 && len < sizeof(env_buf)) {
            cache_dir = std::filesystem::path(std::string(env_buf, len)) / "openscarlett_test_autolift_cache";
        } else {
            cache_dir = std::filesystem::temp_directory_path() / "openscarlett_test_autolift_cache";
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", cache_dir.string().c_str());
    SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", "0");

    D3D12AdapterInfo hw_info{};
    const bool has_hw = HasHardwareGpu(hw_info);
    std::printf("=== OpenScarlett d3d12_xs Auto-Lift + ShaderCache Verification ===\n");
    std::printf("Cache Dir : %s\n", cache_dir.string().c_str());
    if (has_hw) {
        std::printf("HW GPU    : %s (SM=0x%02X, MeshTier=%u)\n",
                    hw_info.description.c_str(), hw_info.highest_shader_model, hw_info.mesh_shader_tier);
    } else {
        std::printf("HW GPU    : none (WARP-only CI runner)\n");
    }

    // -----------------------------------------------------------------------
    // Test 1: XS2\0 Compute PSO Auto-Lifted via DeviceX on HW + WARP + CPU Parity
    //         (+ Standard Host PC DXBC Passthrough)
    // -----------------------------------------------------------------------
    const auto cs_xs2 = BuildComputeXs2(5u, 17u);
    std::vector<uint32_t> cs_input(64);
    for (uint32_t i = 0; i < 64u; ++i) cs_input[i] = 0x11000000u | (i * 13u + 7u);

    Rdna2CpuExecState cpu_ref{};
    cpu_ref.uav0 = cs_input;
    cpu_ref.uav1.assign(64, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(cs_xs2, cpu_ref).ok());

    OpenScarlettD3D12ResetTelemetry();
    {
        if (has_hw) {
            DeviceXHarness hw;
            hw.Init(/*use_warp=*/false);
            std::vector<uint32_t> hw_out;
            hw.RunComputeShader(cs_xs2, cs_input, hw_out);
            OS_REQUIRE(hw_out == cpu_ref.uav1);

            // Second PSO on same DeviceX -> MemoryHit!
            std::vector<uint32_t> hw_out2;
            hw.RunComputeShader(cs_xs2, cs_input, hw_out2);
            OS_REQUIRE(hw_out2 == cpu_ref.uav1);
            OS_REQUIRE(GetDllTelemetryCount("MemoryHit") >= 1u);
        }

        DeviceXHarness warp;
        warp.Init(/*use_warp=*/true);
        std::vector<uint32_t> warp_out;
        warp.RunComputeShader(cs_xs2, cs_input, warp_out);
        OS_REQUIRE(warp_out == cpu_ref.uav1);

        // Also verify standard host PC DXBC shader passes straight through untouched
        const char* host_cs_src =
            "RWByteAddressBuffer _in0 : register(u0);\n"
            "RWByteAddressBuffer _out0 : register(u1);\n"
            "[numthreads(64,1,1)] void main(uint3 tid : SV_DispatchThreadID) {\n"
            "    _out0.Store(tid.x * 4, _in0.Load(tid.x * 4) + 1u);\n"
            "}\n";
        ComPtr<ID3DBlob> host_bc, host_err;
        OS_REQUIRE(SUCCEEDED(D3DCompile(
            host_cs_src, std::strlen(host_cs_src), "host_cs.hlsl", nullptr, nullptr,
            "main", "cs_5_0", 0, 0, &host_bc, &host_err)));
        std::vector<uint8_t> host_bytes(
            static_cast<const uint8_t*>(host_bc->GetBufferPointer()),
            static_cast<const uint8_t*>(host_bc->GetBufferPointer()) + host_bc->GetBufferSize());
        OS_REQUIRE(!IsXboxShaderBytecode(host_bytes.data(), host_bytes.size()));
        std::vector<uint32_t> host_out;
        warp.RunComputeShader(host_bytes, cs_input, host_out);
        for (uint32_t i = 0; i < 64u; ++i) {
            OS_REQUIRE(host_out[i] == cs_input[i] + 1u);
        }
    }
    const uint64_t t1_misses = GetDllTelemetryCount("CacheMiss");
    const uint64_t t1_memhits = GetDllTelemetryCount("MemoryHit");
    OS_REQUIRE(t1_misses >= 1u);
    std::printf("[PASS] Test 1: XS2 Compute auto-lift on %s + WARP == CPU Interpreter (CacheMiss=%llu, MemoryHit=%llu) + Host DXBC passthrough\n",
                has_hw ? hw_info.description.c_str() : "WARP-only",
                static_cast<unsigned long long>(t1_misses),
                static_cast<unsigned long long>(t1_memhits));

    // -----------------------------------------------------------------------
    // Test 2: Second Run Hits On-Disk Shader Cache (DiskHit >= 1) + Identical Execution
    // -----------------------------------------------------------------------
    OpenScarlettD3D12ResetTelemetry();
    uint32_t dev2_sm = has_hw ? hw_info.highest_shader_model : 0x62u;
    {
        // Fresh DeviceX instance = clean empty in-memory cache; must hit on-disk cache!
        DeviceXHarness dev2;
        dev2.Init(/*use_warp=*/!has_hw);
        if (dev2.gpu_info.HighestShaderModel != 0u) {
            dev2_sm = dev2.gpu_info.HighestShaderModel;
        }
        std::vector<uint32_t> run2_out;
        dev2.RunComputeShader(cs_xs2, cs_input, run2_out);
        OS_REQUIRE(run2_out == cpu_ref.uav1);
    }
    const uint64_t dll_disk_hits = GetDllTelemetryCount("DiskHit");
    OS_REQUIRE(dll_disk_hits >= 1u);

    // Also verify ShaderCache::Global().ClearMemoryCache() + Stats().disk_hits directly!
    {
        ShaderCache& sc = ShaderCache::Global();
        sc.SetCacheDirectory(cache_dir.string());
        sc.ClearMemoryCache();
        sc.ResetStats();
        ShaderCacheLiftRequest req{};
        req.highest_shader_model = dev2_sm ? dev2_sm : 0x60u;
        CachedShaderResult cached{};
        OS_REQUIRE(sc.GetOrLiftXboxShader(cs_xs2.data(), cs_xs2.size(), req, cached).ok());
        OS_REQUIRE(cached.from_disk_cache);
        OS_REQUIRE(sc.Stats().disk_hits >= 1u);
    }
    std::printf("[PASS] Test 2: On-Disk Shader Cache Hit verified (DeviceX DiskHit=%llu, ShaderCache::Stats().disk_hits=%llu)\n",
                static_cast<unsigned long long>(dll_disk_hits),
                static_cast<unsigned long long>(ShaderCache::Global().Stats().disk_hits));

    // -----------------------------------------------------------------------
    // Test 3: XS2\0 Graphics (VS+PS) PSO Auto-Lifted via DeviceX::CreateGraphicsPipelineState
    // -----------------------------------------------------------------------
    const auto vs_xs2 = BuildFullscreenVsXs2();
    const auto ps_xs2 = BuildFullscreenPsXs2();

    auto run_gfx_test = [&](bool use_warp, const char* label) {
        DeviceXHarness h;
        h.Init(use_warp);
        auto rs = h.CreateEmptyRootSignature();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
        gpd.pRootSignature = rs.Get();
        gpd.VS = {vs_xs2.data(), vs_xs2.size()};
        gpd.PS = {ps_xs2.data(), ps_xs2.size()};
        gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gpd.SampleMask = 0xFFFFFFFFu;
        gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gpd.NumRenderTargets = 1;
        gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gpd.SampleDesc = {1, 0};

        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = h.device->CreateGraphicsPipelineState(&gpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] Test 3 CreateGraphicsPipelineState (%s) failed: hr=0x%08lX\n",
                         label, static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        std::vector<uint8_t> pixels;
        h.RenderFullscreenQuad64x64(rs.Get(), pso.Get(), /*use_dispatch_mesh=*/false, pixels);
        OS_REQUIRE(pixels.size() == 64u * 64u * 4u);
        for (uint32_t p = 0; p < 64u * 64u; ++p) {
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 0]) - 128) <= 1);
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 1]) - 128) <= 1);
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 2]) - 191) <= 1);
            OS_REQUIRE(pixels[p * 4 + 3] == 255u);
        }
    };

    if (has_hw) run_gfx_test(false, "HW");
    run_gfx_test(true, "WARP");
    std::printf("[PASS] Test 3: XS2 Graphics (VS+PS) auto-lifted via DeviceX::CreateGraphicsPipelineState (64x64 RT=128,128,191,255 on %s + WARP)\n",
                has_hw ? "HW" : "WARP-only");

    // -----------------------------------------------------------------------
    // Test 4: XS2\0 Mesh Shader (MS+PS) PSO Auto-Lifted via DeviceX::CreatePipelineState Stream
    // -----------------------------------------------------------------------
    const auto ms_xs2 = BuildFullscreenMsXs2();
    if (has_hw && hw_info.mesh_shader_tier >= 10u) {
        DeviceXHarness hw;
        hw.Init(/*use_warp=*/false);
        auto rs = hw.CreateEmptyRootSignature();

        StandaloneMeshPsoStream stream{};
        stream.root_sig.inner = rs.Get();
        stream.ms.inner = {ms_xs2.data(), ms_xs2.size()};
        stream.ps.inner = {ps_xs2.data(), ps_xs2.size()};
        stream.blend.inner.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.sample_mask.inner = 0xFFFFFFFFu;
        stream.rasterizer.inner.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rasterizer.inner.CullMode = D3D12_CULL_MODE_NONE;
        stream.depth_stencil.inner.DepthEnable = FALSE;
        stream.rt_formats.inner.NumRenderTargets = 1;
        stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample_desc.inner = {1, 0};

        D3D12_PIPELINE_STATE_STREAM_DESC sdesc{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> mesh_pso;
        HRESULT hr = hw.device->CreatePipelineState(&sdesc, IID_PPV_ARGS(&mesh_pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] Test 4 Mesh CreatePipelineState failed: hr=0x%08lX\n",
                         static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        std::vector<uint8_t> pixels;
        hw.RenderFullscreenQuad64x64(rs.Get(), mesh_pso.Get(), /*use_dispatch_mesh=*/true, pixels);
        for (uint32_t p = 0; p < 64u * 64u; ++p) {
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 0]) - 191) <= 1);
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 1]) - 64) <= 1);
            OS_REQUIRE(std::abs(static_cast<int>(pixels[p * 4 + 2]) - 223) <= 1);
            OS_REQUIRE(pixels[p * 4 + 3] == 255u);
        }
        std::printf("[PASS] Test 4: XS2 Mesh (MS+PS) auto-lifted via DeviceX::CreatePipelineState stream on %s (MeshTier=%u, 64x64 RT=%u,%u,%u,%u)\n",
                    hw_info.description.c_str(), hw_info.mesh_shader_tier,
                    pixels[0], pixels[1], pixels[2], pixels[3]);
    } else {
        // WARP-only CI runner: still verify MS lifting + ShaderCache compilation cleanly!
        ShaderCacheLiftRequest req{};
        req.highest_shader_model = 0x65u;
        req.mesh_has_payload = false;
        CachedShaderResult cached_ms{};
        OS_REQUIRE(ShaderCache::Global().GetOrLiftXboxShader(ms_xs2.data(), ms_xs2.size(), req, cached_ms).ok());
        OS_REQUIRE(cached_ms.target_profile == "ms_6_5");
        OS_REQUIRE(cached_ms.dxil.size() >= 32u);
        std::printf("[PASS] Test 4: XS2 Mesh (MS) lift+cache validated (live DispatchMesh skipped on WARP-only runner)\n");
    }

    // -----------------------------------------------------------------------
    // Test 5: Xbox-Wrapped DXBC (XBOX/XS2M + ISG1/OSG1 + STSM/RTS0) Auto-Lifted via DeviceX
    // -----------------------------------------------------------------------
    {
        const auto cs_wrapped_raw = BuildComputeXs2(9u, 33u);
        XboxDxbcBuildDesc bx{};
        bx.xs2_blob = cs_wrapped_raw;
        bx.xbox_chunk_fourcc = "XBOX";
        bx.input_signature.push_back({"SV_DispatchThreadID", 0, 0, 1, 0, 0x7u, 0x7u, 0});
        bx.output_signature.push_back({"UAV_OUT", 0, 0, 1, 0, 0x1u, 0x1u, 0});
        XboxStaticSamplerInfo ss{};
        ss.filter = 0u;
        ss.shader_register = 0u;
        bx.static_samplers.push_back(ss);
        const auto dxbc_xbox = BuildXboxDxbcContainer(bx);

        // Also test XS2M + RTS0 variant
        XboxDxbcBuildDesc bx2 = bx;
        bx2.xbox_chunk_fourcc = "XS2M";
        bx2.emit_rts0_for_samplers = true;
        const auto dxbc_xs2m = BuildXboxDxbcContainer(bx2);

        OS_REQUIRE(IsXboxShaderBytecode(dxbc_xbox.data(), dxbc_xbox.size()));
        OS_REQUIRE(IsXboxShaderBytecode(dxbc_xs2m.data(), dxbc_xs2m.size()));

        Rdna2CpuExecState cpu_dxbc{};
        cpu_dxbc.uav0 = cs_input;
        cpu_dxbc.uav1.assign(64, 0u);
        OS_REQUIRE(ExecuteRdna2BlobCpu(dxbc_xbox, cpu_dxbc).ok());

        if (has_hw) {
            DeviceXHarness hw;
            hw.Init(/*use_warp=*/false);
            std::vector<uint32_t> hw_out1, hw_out2;
            hw.RunComputeShader(dxbc_xbox, cs_input, hw_out1);
            hw.RunComputeShader(dxbc_xs2m, cs_input, hw_out2);
            OS_REQUIRE(hw_out1 == cpu_dxbc.uav1);
            OS_REQUIRE(hw_out2 == cpu_dxbc.uav1);
        }

        DeviceXHarness warp;
        warp.Init(/*use_warp=*/true);
        std::vector<uint32_t> warp_out;
        warp.RunComputeShader(dxbc_xbox, cs_input, warp_out);
        OS_REQUIRE(warp_out == cpu_dxbc.uav1);
    }
    std::printf("[PASS] Test 5: Xbox-wrapped DXBC (XBOX/ISG1/OSG1/STSM & XS2M/RTS0) auto-lifted via DeviceX == CPU Interpreter\n");

    // -----------------------------------------------------------------------
    // Test 6: Async Background PSO Compilation Returns Instant Valid Stand-In PSO & Completes Cleanly
    // -----------------------------------------------------------------------
    {
        const auto cs_async = BuildComputeXs2(11u, 42u);
        Rdna2CpuExecState cpu_async{};
        cpu_async.uav0 = cs_input;
        cpu_async.uav1.assign(64, 0u);
        OS_REQUIRE(ExecuteRdna2BlobCpu(cs_async, cpu_async).ok());

        OpenScarlettD3D12ResetTelemetry();
        SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", "1");

        double standin_ms = 0.0;
        {
            DeviceXHarness async_dev;
            async_dev.Init(/*use_warp=*/!has_hw);
            auto rs = async_dev.CreateDualUavRootSignature();

            D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = rs.Get();
            cpd.CS = {cs_async.data(), cs_async.size()};
            ComPtr<ID3D12PipelineState> standin_pso;

            const auto t0 = std::chrono::steady_clock::now();
            HRESULT hr = async_dev.device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&standin_pso));
            const auto t1 = std::chrono::steady_clock::now();
            standin_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

            OS_REQUIRE(SUCCEEDED(hr));
            OS_REQUIRE(standin_pso.Get() != nullptr);
            OS_REQUIRE(GetDllTelemetryCount("AsyncCompileQueued") >= 1u);
            // async_dev destructor cleanly waits for background dxc worker via WaitForPendingCompiles()!
        }

        const uint64_t async_queued = GetDllTelemetryCount("AsyncCompileQueued");
        const uint64_t async_done = GetDllTelemetryCount("AsyncCompileCompleted");
        OS_REQUIRE(async_queued >= 1u);
        OS_REQUIRE(async_done >= 1u);

        // Now disable async mode and create a fresh DeviceX: real compiled DXIL must hit disk cache & match CPU!
        SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", "0");
        OpenScarlettD3D12ResetTelemetry();
        uint32_t verify_sm = dev2_sm;
        {
            DeviceXHarness verify_dev;
            verify_dev.Init(/*use_warp=*/!has_hw);
            if (verify_dev.gpu_info.HighestShaderModel != 0u) {
                verify_sm = verify_dev.gpu_info.HighestShaderModel;
            }
            std::vector<uint32_t> real_out;
            verify_dev.RunComputeShader(cs_async, cs_input, real_out);
            OS_REQUIRE(real_out == cpu_async.uav1);
            OS_REQUIRE(GetDllTelemetryCount("DiskHit") >= 1u);
        }

        // Also test direct ShaderCache::EnableAsyncPsoCompilation(true) + WaitForPendingCompiles()
        {
            const auto cs_async2 = BuildComputeXs2(13u, 55u);
            ShaderCache local_cache;
            local_cache.SetCacheDirectory(cache_dir.string());
            local_cache.EnableAsyncPsoCompilation(true);

            ShaderCacheLiftRequest req{};
            req.highest_shader_model = verify_sm ? verify_sm : 0x60u;
            CachedShaderResult standin{};
            OS_REQUIRE(local_cache.GetOrLiftXboxShader(cs_async2.data(), cs_async2.size(), req, standin).ok());
            OS_REQUIRE(standin.is_standin_async);
            OS_REQUIRE(standin.dxil.size() >= 32u);

            local_cache.WaitForPendingCompiles();
            OS_REQUIRE(local_cache.IsCompileReady(standin.key));
            OS_REQUIRE(local_cache.Stats().async_compiles_queued == 1u);
            OS_REQUIRE(local_cache.Stats().async_compiles_completed == 1u);
        }

        std::printf("[PASS] Test 6: Async background PSO compilation (stand-in returned in %.2f ms, queued=%llu, completed=%llu, subsequent DiskHit verified)\n",
                    standin_ms,
                    static_cast<unsigned long long>(async_queued),
                    static_cast<unsigned long long>(async_done));
    }

    // Cleanup temp cache dir & restore env vars
    SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", nullptr);
    SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", nullptr);
    std::filesystem::remove_all(cache_dir, ec);

    std::printf("[PASS] test_d3d12_xs_autolift: ALL 6 AUTO-LIFT & CACHE SUITES PASSED!\n");
    return 0;
#endif
}
