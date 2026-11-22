#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

using namespace openscarlett;

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

bool HasExternalSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(kDefaultSpirvValPath, ec);
}

void VerifySpirvStrict(const std::vector<uint32_t>& spv, const char* label, const char* target_env = "vulkan1.2") {
    auto bin_st = ValidateSpirvBinary(spv);
    if (!bin_st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, bin_st.message.c_str());
    }
    OS_REQUIRE(bin_st.ok());
    if (!HasExternalSpirvVal()) return;

    std::string diag;
    auto st = ValidateSpirvExternal(spv, diag, kDefaultSpirvValPath, target_env);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val (%s) for %s: %s\n", target_env, label, diag.c_str());
    }
    OS_REQUIRE(st.ok());
}

void RunOnVulkanSpirv(
    const std::vector<uint32_t>& spv,
    const std::vector<uint32_t>& in_uav0,
    const std::vector<uint32_t>& expected_uav1,
    uint32_t group_x,
    const char* label,
    uint32_t masked_sentinel = 0u) {
    VulkanBackend vk;
    if (!vk.Initialize(/*allow_software_fallback=*/false).ok() || vk.is_fallback_mode()) {
        return;  // Hardware Vulkan ICD unavailable on this host
    }
    std::vector<uint32_t> vk_in = in_uav0;
    if (vk_in.size() < expected_uav1.size()) {
        vk_in.resize(expected_uav1.size(), 0u);
    }
    std::vector<uint32_t> vk_out;
    Status st = vk.ExecuteComputeShader(spv, vk_in, vk_out, group_x);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Vulkan ExecuteComputeShader(%s) on %s: %s\n",
                     label, vk.GetDeviceInfo().device_name.c_str(), st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(vk_out.size() >= expected_uav1.size());
    for (size_t i = 0; i < expected_uav1.size(); ++i) {
        const uint32_t want = (masked_sentinel != 0u && expected_uav1[i] == masked_sentinel) ? 0u : expected_uav1[i];
        if (vk_out[i] != want) {
            std::fprintf(stderr, "[FAIL] Vulkan SPIR-V %s on %s idx=%zu: got=0x%08X expected=0x%08X\n",
                         label, vk.GetDeviceInfo().device_name.c_str(), i, vk_out[i], want);
        }
        OS_REQUIRE(vk_out[i] == want);
    }
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct D3D12CorpusContext {
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
    bool is_warp = false;
    uint32_t highest_sm = 0;

    ~D3D12CorpusContext() {
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
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
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
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&res))));
        return res;
    }

    void RunComputeDualUav(const std::vector<uint8_t>& dxil,
                           const std::vector<uint32_t>& in_uav0,
                           std::vector<uint32_t>& out_uav1,
                           uint32_t group_x) {
        const uint64_t in_bytes = std::max<uint64_t>(4u, static_cast<uint64_t>(in_uav0.size()) * sizeof(uint32_t));
        const uint64_t out_bytes = std::max<uint64_t>(4u, static_cast<uint64_t>(out_uav1.size()) * sizeof(uint32_t));

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
        ComPtr<ID3DBlob> sig_blob, err_blob;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob)));
        ComPtr<ID3D12RootSignature> root_sig;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(
            0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig))));

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = device->CreateComputePipelineState(&cpd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[FAIL] CreateComputePipelineState on %s: hr=0x%08lX\n",
                         adapter_name.c_str(), static_cast<unsigned long>(hr));
        }
        OS_REQUIRE(SUCCEEDED(hr));

        auto in_up = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        void* mapped = nullptr;
        OS_REQUIRE(SUCCEEDED(in_up->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, in_uav0.data(), static_cast<size_t>(in_bytes));
        in_up->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(out_up->Map(0, nullptr, &mapped)));
        std::memcpy(mapped, out_uav1.data(), static_cast<size_t>(out_bytes));
        out_up->Unmap(0, nullptr);

        OS_REQUIRE(SUCCEEDED(allocator->Reset()));
        OS_REQUIRE(SUCCEEDED(cmd_list->Reset(allocator.Get(), pso.Get())));
        cmd_list->CopyResource(in_gpu.Get(), in_up.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_up.Get());

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
        cmd_list->CopyResource(rb.Get(), out_gpu.Get());

        OS_REQUIRE(SUCCEEDED(cmd_list->Close()));
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitGpu();

        OS_REQUIRE(SUCCEEDED(rb->Map(0, nullptr, &mapped)));
        std::memcpy(out_uav1.data(), mapped, static_cast<size_t>(out_bytes));
        rb->Unmap(0, nullptr);
    }
};

std::vector<uint8_t> CompileForContext(const D3D12CorpusContext& ctx,
                                       const std::string& hlsl,
                                       const std::vector<uint8_t>& sm65_dxil,
                                       bool test_resign = false) {
    std::vector<uint8_t> dxil;
    if (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) {
        dxil = sm65_dxil;
    } else {
        const char* target = (ctx.highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
        std::string diag;
        auto st = CompileHlslWithDxc(hlsl, target, "main", dxil, diag);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] DXC %s compile for %s failed: %s\n",
                         target, ctx.adapter_name.c_str(), diag.c_str());
        }
        OS_REQUIRE(st.ok());
    }
    if (test_resign) {
        std::array<uint8_t, 16> orig{};
        std::memcpy(orig.data(), dxil.data() + 4u, 16u);
        std::memset(dxil.data() + 4u, 0x55, 16u);
        OS_REQUIRE(!VerifyDxbcContainerHash(dxil.data(), dxil.size()));
        OS_REQUIRE(ResignDxilContainer(dxil).ok());
        OS_REQUIRE(VerifyDxbcContainerHash(dxil.data(), dxil.size()));
        OS_REQUIRE(std::memcmp(dxil.data() + 4u, orig.data(), 16u) == 0);
    }
    return dxil;
}

void RunOnHwAndWarp(
    const std::string& hlsl,
    const std::vector<uint8_t>& dxil,
    const std::vector<uint32_t>& in_uav0,
    const std::vector<uint32_t>& init_uav1,
    const std::vector<uint32_t>& expected_uav1,
    uint32_t group_x,
    const char* label,
    bool test_resign = false) {
    bool ran_hw = false;
    bool ran_warp = false;
    for (bool use_warp : {false, true}) {
        D3D12CorpusContext ctx;
        if (!ctx.Init(use_warp)) continue;
        std::vector<uint8_t> dev_dxil = CompileForContext(ctx, hlsl, dxil, test_resign);
        std::vector<uint32_t> out = init_uav1;
        ctx.RunComputeDualUav(dev_dxil, in_uav0, out, group_x);
        for (size_t i = 0; i < expected_uav1.size(); ++i) {
            if (out[i] != expected_uav1[i]) {
                std::fprintf(stderr, "[FAIL] %s on %s idx=%zu: got=0x%08X expected=0x%08X\n",
                             label, ctx.adapter_name.c_str(), i, out[i], expected_uav1[i]);
            }
            OS_REQUIRE(out[i] == expected_uav1[i]);
        }
        if (use_warp) ran_warp = true;
        else ran_hw = true;
    }
    OS_REQUIRE(ran_warp);
    (void)ran_hw;
}
#endif

// ---------------------------------------------------------------------------
// Test 1: v_alignbyte_b32 + buffer_load_format sub-dword merge + MTBUF D16
// ---------------------------------------------------------------------------
void TestAlignByteAndMtbufD16() {
    Rdna2Assembler a;
    // Lane 0..3: test v_alignbyte_b32 with byte_sel = tid (0, 1, 2, 3) -> including 0 (32-0 == 32 UB trap!)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x11223344u));  // hi
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0xAABBCCDDu));  // lo
    a.EmitVop3(Rdna2Opcode::V_ALIGNBYTE_B32, 3, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(0));

    // Unaligned sub-dword buffer_load_format_x at byte offset = tid (0, 1, 2, 3) from uav0
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_FORMAT_X, 4, 0, 0, 0, true);

    // MTBUF D16 format loads from uav0[8..] (byte offset 8):
    //   uav0[2] (bytes 8..11) = {0x00, 0xFF, 0x80 (-128 SNORM -> -1.0f), 0x7F (+127 SNORM -> +1.0f)}
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(8));
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW, 12, 10, 0, Rdna2MtbufFormat::R8G8B8A8_UNORM, 0, true);
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW, 14, 10, 0, Rdna2MtbufFormat::R8G8B8A8_SNORM, 0, true);
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW, 16, 10, 0, Rdna2MtbufFormat::R8G8B8A8_UINT, 0, true);
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW, 18, 10, 0, Rdna2MtbufFormat::R8G8B8A8_SINT, 0, true);
    //   uav0[3] (bytes 12..15) = packed f16 (1.5=0x3E00, -2.0=0xC000)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(12));
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY, 20, 11, 0, Rdna2MtbufFormat::R16G16_FLOAT, 0, true);
    //   uav0[4] (bytes 16..19) = {0x8000 (-32768 SNORM -> -1.0f), 0x7FFF (+32767 SNORM -> +1.0f)}
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::InlineInt(16));
    a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY, 22, 21, 0, Rdna2MtbufFormat::R16G16_SNORM, 0, true);

    // Store per-lane results into uav1 (srsrc=4)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 25, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 25, 4, 0);     // [0..3]: v_alignbyte_b32
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 25, 4, 16);    // [4..7]: unaligned buffer_load_format_x
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 25, 4, 32);   // [8..11]: D16 R8G8B8A8_UNORM lo
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 25, 4, 48);   // [12..15]: D16 R8G8B8A8_SNORM lo
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 25, 4, 64);   // [16..19]: D16 R8G8B8A8_SNORM hi (-128 -> -1.0, 127 -> +1.0)
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 25, 4, 80);   // [20..23]: D16 R8G8B8A8_UINT lo
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 19, 25, 4, 96);   // [24..27]: D16 R8G8B8A8_SINT hi (-128=0xFF80, 127=0x007F)
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20, 25, 4, 112);  // [28..31]: D16 R16G16_FLOAT
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 22, 25, 4, 128);  // [32..35]: D16 R16G16_SNORM
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 16, 32, 32);

    std::vector<uint32_t> uav0 = {
        0xAABBCCDDu,  // bytes 0..3
        0x11223344u,  // bytes 4..7
        0x7F80FF00u,  // bytes 8..11: {0x00, 0xFF, 0x80(-128), 0x7F(+127)}
        0xC0003E00u,  // bytes 12..15: f16(1.5), f16(-2.0)
        0x7FFF8000u,  // bytes 16..19: i16(-32768), i16(+32767)
        0x00000000u,
    };

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1.assign(36, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

    // Verify exact v_alignbyte_b32 values (0..3)
    OS_REQUIRE(cpu.uav1[0] == 0xAABBCCDDu);
    OS_REQUIRE(cpu.uav1[1] == 0x44AABBCCu);
    OS_REQUIRE(cpu.uav1[2] == 0x3344AABBu);
    OS_REQUIRE(cpu.uav1[3] == 0x223344AAu);
    // Unaligned buffer_load_format_x at byte offsets 0..3 matches exact merge
    OS_REQUIRE(cpu.uav1[4] == 0xAABBCCDDu);
    OS_REQUIRE(cpu.uav1[5] == 0x44AABBCCu);
    OS_REQUIRE(cpu.uav1[6] == 0x3344AABBu);
    OS_REQUIRE(cpu.uav1[7] == 0x223344AAu);
    // D16 R8G8B8A8_UNORM lo: (0.0 -> 0x0000, 1.0 -> 0x3C00)
    OS_REQUIRE(cpu.uav1[8] == 0x3C000000u);
    // D16 R8G8B8A8_SNORM hi: (-128 -> -1.0 = 0xBC00, +127 -> +1.0 = 0x3C00)
    OS_REQUIRE(cpu.uav1[16] == 0x3C00BC00u);
    // D16 R8G8B8A8_UINT lo: (0, 255)
    OS_REQUIRE(cpu.uav1[20] == 0x00FF0000u);
    // D16 R8G8B8A8_SINT hi: (-128 -> 0xFF80, +127 -> 0x007F)
    OS_REQUIRE(cpu.uav1[24] == 0x007FFF80u);
    // D16 R16G16_FLOAT exact bit preservation
    OS_REQUIRE(cpu.uav1[28] == 0xC0003E00u);
    // D16 R16G16_SNORM: (-32768 -> -1.0 = 0xBC00, +32767 -> +1.0 = 0x3C00)
    OS_REQUIRE(cpu.uav1[32] == 0x3C00BC00u);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    auto st = LiftRdna2Xs2Shader(xs2, lifted, opts);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] Lift TestAlignByteAndMtbufD16: %s\n%s\n",
                     st.message.c_str(), lifted.dxc_diagnostics.c_str());
    }
    OS_REQUIRE(st.ok());
    VerifySpirvStrict(lifted.spirv_words, "AlignByteAndMtbufD16");
    RunOnVulkanSpirv(lifted.spirv_words, uav0, cpu.uav1, 1u, "AlignByteAndMtbufD16");

#if defined(_WIN32)
    std::vector<uint32_t> init_out(36, 0u);
    RunOnHwAndWarp(lifted.hlsl_source, lifted.pc_dxil, uav0, init_out, cpu.uav1, 1u, "AlignByteAndMtbufD16");
#endif
    std::printf("[PASS] Corpus 1: v_alignbyte_b32 + unaligned buffer_load_format + MTBUF D16 (CPU == DXIL GTX1650Ti == WARP == Vulkan SPIR-V + spirv-val)\n");
}

// ---------------------------------------------------------------------------
// Test 2: GFX10 v_cmpx execution-mask compare + split F16 compare range
// ---------------------------------------------------------------------------
void TestVcmpxAndSplitF16Compare() {
    Rdna2Assembler a;
    // Set VCC first via V_CMP_EQ_U32 (tid == 1) so we verify V_CMPX never clobbers VCC!
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    // Test split F16 compare range:
    //   uav0[tid] holds packed f16 pair (lo = a, hi = b)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
    // Upper split-range F16 compare: V_CMP_U_F16 (0xD8, true iff either is NaN) with op_sel=0b10 (src0=lo, src1=hi)
    a.EmitVop3(Rdna2Opcode::V_CMP_U_F16, 10, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(2),
               Rdna2Operand::InlineInt(0), false, 0x2u);
    a.EmitVop3(Rdna2Opcode::V_CNDMASK_B32, 3, Rdna2Operand::InlineInt(0), Rdna2Operand::InlineInt(1), Rdna2Operand::Sgpr(10));
    // Lower split-range F16 compare: V_CMP_LT_F16 (0xC9) with op_sel=0b10 (lo < hi)
    a.EmitVop3(Rdna2Opcode::V_CMP_LT_F16, 12, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(2),
               Rdna2Operand::InlineInt(0), false, 0x2u);
    a.EmitVop3(Rdna2Opcode::V_CNDMASK_B32, 4, Rdna2Operand::InlineInt(0), Rdna2Operand::InlineInt(1), Rdna2Operand::Sgpr(12));
    // Upper split-range F16 compare: V_CMP_NEQ_F16 (0xDD, true iff lo != hi OR unordered/NaN)
    a.EmitVop3(Rdna2Opcode::V_CMP_NEQ_F16, 14, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(2),
               Rdna2Operand::InlineInt(0), false, 0x2u);
    a.EmitVop3(Rdna2Opcode::V_CNDMASK_B32, 5, Rdna2Operand::InlineInt(0), Rdna2Operand::InlineInt(1), Rdna2Operand::Sgpr(14));

    // Store F16 compare results before V_CMPX restricts EXEC
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);   // [0..3]: V_CMP_U_F16
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 16);  // [4..7]: V_CMP_LT_F16
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 32);  // [8..11]: V_CMP_NEQ_F16

    // Now test GFX10 V_CMPX_LT_U32: keep only lanes where tid < 2 active in EXEC (must NOT clobber VCC!)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineInt(2));
    a.EmitVopc(Rdna2Opcode::V_CMPX_LT_U32, Rdna2Operand::Vgpr(0), 6);  // EXEC = (tid < 2)
    // Verify VCC was preserved by V_CMPX (lane 1 had vcc=1, lane 0 had vcc=0)
    a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 7, Rdna2Operand::InlineInt(10), 6);  // vcc ? 2 : 10 -> lane0=10, lane1=2
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 48);  // [12..15]: only lanes 0 & 1 write! Lanes 2 & 3 stay 0xDEAD!
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    // Verify disassembly round-trip of split F16 opcodes and V_CMPX
    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());
    bool saw_u_f16 = false, saw_neq_f16 = false, saw_cmpx = false;
    for (const auto& ins : insns) {
        if (ins.opcode == Rdna2Opcode::V_CMP_U_F16) { OS_REQUIRE(ins.raw_opcode == 0xD8u); saw_u_f16 = true; }
        if (ins.opcode == Rdna2Opcode::V_CMP_NEQ_F16) { OS_REQUIRE(ins.raw_opcode == 0xDDu); saw_neq_f16 = true; }
        if (ins.opcode == Rdna2Opcode::V_CMPX_LT_U32) { OS_REQUIRE(ins.raw_opcode == 0xD1u); saw_cmpx = true; }
    }
    OS_REQUIRE(saw_u_f16 && saw_neq_f16 && saw_cmpx);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 20, 16, 32);
    // Input packed f16 pairs:
    //   lane 0: (1.0=0x3C00, 2.0=0x4000) -> U=0, LT=1, NEQ=1
    //   lane 1: (2.0=0x4000, 2.0=0x4000) -> U=0, LT=0, NEQ=0
    //   lane 2: (NaN=0x7E00, 1.0=0x3C00) -> U=1, LT=0, NEQ=1
    //   lane 3: (3.0=0x4200, 1.0=0x3C00) -> U=0, LT=0, NEQ=1
    std::vector<uint32_t> uav0 = {0x40003C00u, 0x40004000u, 0x3C007E00u, 0x3C004200u};
    std::vector<uint32_t> init_uav1(16, 0x0000DEADu);

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1 = init_uav1;
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

    OS_REQUIRE(cpu.uav1[0] == 0u && cpu.uav1[1] == 0u && cpu.uav1[2] == 1u && cpu.uav1[3] == 0u);  // V_CMP_U_F16
    OS_REQUIRE(cpu.uav1[4] == 1u && cpu.uav1[5] == 0u && cpu.uav1[6] == 0u && cpu.uav1[7] == 0u);  // V_CMP_LT_F16
    OS_REQUIRE(cpu.uav1[8] == 1u && cpu.uav1[9] == 0u && cpu.uav1[10] == 1u && cpu.uav1[11] == 1u);// V_CMP_NEQ_F16
    // V_CMPX masked out lanes 2 & 3, and preserved VCC (lane 0: 10, lane 1: 2, lanes 2 & 3: untouched 0xDEAD)
    OS_REQUIRE(cpu.uav1[12] == 10u);
    OS_REQUIRE(cpu.uav1[13] == 2u);
    OS_REQUIRE(cpu.uav1[14] == 0x0000DEADu);
    OS_REQUIRE(cpu.uav1[15] == 0x0000DEADu);

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "VcmpxAndSplitF16Compare");
    RunOnVulkanSpirv(lifted.spirv_words, uav0, cpu.uav1, 1u, "VcmpxAndSplitF16Compare", 0x0000DEADu);

#if defined(_WIN32)
    RunOnHwAndWarp(lifted.hlsl_source, lifted.pc_dxil, uav0, init_uav1, cpu.uav1, 1u, "VcmpxAndSplitF16Compare");
#endif
    std::printf("[PASS] Corpus 2: GFX10 v_cmpx exec-mask + split-range F16 compares (CPU == DXIL GTX1650Ti == WARP == Vulkan SPIR-V + spirv-val)\n");
}

// ---------------------------------------------------------------------------
// Test 3: IEEE/ISA-exact NaN quieting + denormal floor/ceil semantics
// ---------------------------------------------------------------------------
void TestNanQuietingAndDenormalFloorCeil() {
    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
    a.EmitVop1(Rdna2Opcode::V_FLOOR_F32, 3, Rdna2Operand::Vgpr(2));
    a.EmitVop1(Rdna2Opcode::V_CEIL_F32, 4, Rdna2Operand::Vgpr(2));
    // Test FMin/FMax with sNaN (0x7F800005)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(0x7F800005u));
    a.EmitVop2(Rdna2Opcode::V_MIN_F32, 6, Rdna2Operand::Vgpr(2), 5);
    a.EmitVop2(Rdna2Opcode::V_MAX_F32, 7, Rdna2Operand::Vgpr(2), 5);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);   // [0..3]: floor
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 16);  // [4..7]: ceil
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 32);  // [8..11]: min(x, sNaN)
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 48);  // [12..15]: max(x, sNaN)
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 16, 16, 32);
    // Inputs:
    //   lane 0: +denorm (0x00000001) -> floor = +0.0 (0x00000000), ceil = +1.0 (0x3F800000)
    //   lane 1: -denorm (0x80000001) -> floor = -1.0 (0xBF800000), ceil = -0.0 (0x80000000)
    //   lane 2: sNaN    (0x7F800001) -> floor = qNaN (0x7FC00001), ceil = qNaN (0x7FC00001), min/max = qNaN (0x7FC00001)
    //   lane 3: -2.25f               -> floor = -3.0, ceil = -2.0, min(x, sNaN) = -2.25f, max(x, sNaN) = -2.25f
    std::vector<uint32_t> uav0 = {
        0x00000001u,
        0x80000001u,
        0x7F800001u,
        FloatToBits(-2.25f),
    };

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1.assign(16, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());

    OS_REQUIRE(cpu.uav1[0] == 0x00000000u);           // floor(+denorm) == +0.0
    OS_REQUIRE(cpu.uav1[1] == FloatToBits(-1.0f));    // floor(-denorm) == -1.0
    OS_REQUIRE(cpu.uav1[2] == 0x7FC00001u);           // floor(sNaN) == quieted qNaN
    OS_REQUIRE(cpu.uav1[3] == FloatToBits(-3.0f));    // floor(-2.25) == -3.0
    OS_REQUIRE(cpu.uav1[4] == FloatToBits(1.0f));     // ceil(+denorm) == +1.0
    OS_REQUIRE(cpu.uav1[5] == 0x80000000u);           // ceil(-denorm) == -0.0
    OS_REQUIRE(cpu.uav1[6] == 0x7FC00001u);           // ceil(sNaN) == quieted qNaN
    OS_REQUIRE(cpu.uav1[7] == FloatToBits(-2.0f));    // ceil(-2.25) == -2.0
    OS_REQUIRE(cpu.uav1[10] == 0x7FC00001u);          // min(sNaN, sNaN) == quieted qNaN
    OS_REQUIRE(cpu.uav1[11] == FloatToBits(-2.25f));  // min(-2.25, sNaN) == -2.25
    OS_REQUIRE(cpu.uav1[14] == 0x7FC00001u);          // max(sNaN, sNaN) == quieted qNaN
    OS_REQUIRE(cpu.uav1[15] == FloatToBits(-2.25f));  // max(-2.25, sNaN) == -2.25

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "NanQuietingAndDenormalFloorCeil");
    RunOnVulkanSpirv(lifted.spirv_words, uav0, cpu.uav1, 1u, "NanQuietingAndDenormalFloorCeil");

#if defined(_WIN32)
    std::vector<uint32_t> init_out(16, 0u);
    RunOnHwAndWarp(lifted.hlsl_source, lifted.pc_dxil, uav0, init_out, cpu.uav1, 1u, "NanQuietingAndDenormalFloorCeil");
#endif
    std::printf("[PASS] Corpus 3: IEEE/ISA-exact NaN quieting + denormal floor/ceil (CPU == DXIL GTX1650Ti == WARP == Vulkan SPIR-V + spirv-val)\n");
}

// ---------------------------------------------------------------------------
// Test 4: Active-mask Bitmask / Xor / Ballot implication & disjoint pruning
// ---------------------------------------------------------------------------
void TestActiveMaskIrPruning() {
    Rdna2Assembler a;
    // s0 = root_const[0] (exec mask m), s1 = root_const[1] (predicate mask p)
    a.EmitSop2(Rdna2Opcode::S_AND_B32, 2, Rdna2Operand::Sgpr(0), Rdna2Operand::Sgpr(1));     // s2 = m & p (subset of m)
    a.EmitSop2(Rdna2Opcode::S_ANDN2_B32, 3, Rdna2Operand::Sgpr(0), Rdna2Operand::Sgpr(2));   // s3 = m & ~s2 (else mask)
    a.EmitSop2(Rdna2Opcode::S_AND_B32, 4, Rdna2Operand::Sgpr(3), Rdna2Operand::Sgpr(2));     // s4 = (m & ~s2) & s2 -> DISJOINT 0!
    // Xor complement pruning: s5 = m ^ s2 (since s2 <= m), then s5 & s2 -> DISJOINT 0!
    a.EmitSop2(Rdna2Opcode::S_XOR_B32, 5, Rdna2Operand::Sgpr(0), Rdna2Operand::Sgpr(2));
    a.EmitSop2(Rdna2Opcode::S_AND_B32, 6, Rdna2Operand::Sgpr(5), Rdna2Operand::Sgpr(2));     // s6 = (m ^ s2) & s2 -> DISJOINT 0!
    // Double XOR cancellation: (m ^ p) ^ p -> m
    a.EmitSop2(Rdna2Opcode::S_XOR_B32, 7, Rdna2Operand::Sgpr(0), Rdna2Operand::Sgpr(1));
    a.EmitSop2(Rdna2Opcode::S_XOR_B32, 8, Rdna2Operand::Sgpr(7), Rdna2Operand::Sgpr(1));     // s8 -> m
    // Subset implication: (m & p) & m -> (m & p)
    a.EmitSop2(Rdna2Opcode::S_AND_B32, 9, Rdna2Operand::Sgpr(2), Rdna2Operand::Sgpr(0));     // s9 -> s2
    // Sum s4 + s6 + s8 + s9 and store
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 10, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(6));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 11, Rdna2Operand::Sgpr(10), Rdna2Operand::Sgpr(8));
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 12, Rdna2Operand::Sgpr(11), Rdna2Operand::Sgpr(9));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::Sgpr(12));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineInt(0));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 1, 2, 0, 0);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());
    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.code_words = a.Words();

    Rdna2IrModule ir{};
    OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
    Rdna2IrOptStats stats{};
    OS_REQUIRE(OptimizeRdna2Ir(ir, &stats).ok());
    OS_REQUIRE(stats.pruned_mask_ops >= 4u);
    OS_REQUIRE(stats.final_ops < stats.initial_ops);

    // Also test WaveBallot disjoint pruning directly at IR level: Ballot(p) & Ballot(!p) -> 0
    {
        Rdna2IrModule bm{};
        bm.stage = ShaderStage::Compute;
        bm.blocks.resize(1);
        auto& bb = bm.blocks[0];
        auto add_i = [&](IrOp op, IrType t, std::vector<IrValueId> ops, uint32_t imm = 0) -> IrValueId {
            IrInstruction ins{};
            ins.id = (t != IrType::Void) ? bm.AllocateValueId() : kInvalidIrValue;
            ins.op = op;
            ins.type = t;
            ins.operands = std::move(ops);
            ins.imm_u32 = imm;
            bb.instructions.push_back(std::move(ins));
            return bb.instructions.back().id;
        };
        IrValueId tid = add_i(IrOp::ThreadIdX, IrType::U32, {});
        IrValueId c16 = add_i(IrOp::ConstU32, IrType::U32, {}, 16u);
        IrValueId p = add_i(IrOp::CmpLtU, IrType::Bool, {tid, c16});
        IrValueId not_p = add_i(IrOp::LogicalNot, IrType::Bool, {p});
        IrValueId bp = add_i(IrOp::WaveBallotLo, IrType::U32, {p});
        IrValueId bnp = add_i(IrOp::WaveBallotLo, IrType::U32, {not_p});
        IrValueId conj = add_i(IrOp::BitAnd, IrType::U32, {bp, bnp});  // Disjoint ballots -> 0!
        IrValueId c0 = add_i(IrOp::ConstU32, IrType::U32, {}, 0u);
        add_i(IrOp::StoreUav, IrType::Void, {c0, conj}, 0u);
        add_i(IrOp::Return, IrType::Void, {});

        Rdna2IrOptStats bstats{};
        OS_REQUIRE(OptimizeRdna2Ir(bm, &bstats).ok());
        OS_REQUIRE(bstats.pruned_mask_ops >= 1u);
        const IrInstruction* def_conj = bm.FindDef(conj);
        OS_REQUIRE(def_conj != nullptr && def_conj->op == IrOp::ConstU32 && def_conj->imm_u32 == 0u);
    }

    std::printf("[PASS] Corpus 4: Active-mask Bitmask/Xor/Ballot implication & disjoint pruning (%u pruned, %u -> %u ops)\n",
                stats.pruned_mask_ops, stats.initial_ops, stats.final_ops);
}

// ---------------------------------------------------------------------------
// Test 5: PSV0 wave lane bounds + Guest Wave32 isolation on 64-lane host waves
// ---------------------------------------------------------------------------
void TestPsv0WaveBoundsAndWave32Isolation() {
    // 1. Verify PSV0 wave lane bounds parsing & Wave32/Wave64 selection
    Rdna2Assembler trivial;
    trivial.EmitSopp(Rdna2Opcode::S_ENDPGM);
    const auto xs2_w32 = trivial.BuildXs2(ShaderStage::Compute, 1, 1, 1, 8, 8, 32);
    const auto xs2_w64 = trivial.BuildXs2(ShaderStage::Compute, 1, 1, 1, 8, 8, 64);

    // Case A: XS2 says 32, PSV0 requires min=64, max=64 + NumThreads(64,1,1) -> selects Wave64 + 64x1x1
    {
        XboxDxbcBuildDesc d{};
        d.xs2_blob = xs2_w32;
        d.psv0.present = true;
        d.psv0.min_wave_lane_count = 64u;
        d.psv0.max_wave_lane_count = 64u;
        d.psv0.num_threads_x = 64u;
        d.psv0.num_threads_y = 1u;
        d.psv0.num_threads_z = 1u;
        const auto blob = BuildXboxDxbcContainer(d);
        OS_REQUIRE(VerifyDxbcContainerHash(blob.data(), blob.size()));
        XboxDxbcContainer parsed{};
        OS_REQUIRE(ParseXboxShaderContainer(blob, parsed).ok());
        OS_REQUIRE(parsed.psv0.present);
        OS_REQUIRE(parsed.psv0.min_wave_lane_count == 64u);
        OS_REQUIRE(parsed.psv0.max_wave_lane_count == 64u);
        OS_REQUIRE(parsed.xs2.wave_size == 64u);
        OS_REQUIRE(parsed.xs2.threadgroup_x == 64u);
    }
    // Case B: XS2 says 64, PSV0 caps max=32 -> constrains to Wave32
    {
        XboxDxbcBuildDesc d{};
        d.xs2_blob = xs2_w64;
        d.psv0.present = true;
        d.psv0.min_wave_lane_count = 32u;
        d.psv0.max_wave_lane_count = 32u;
        const auto blob = BuildXboxDxbcContainer(d);
        XboxDxbcContainer parsed{};
        OS_REQUIRE(ParseXboxShaderContainer(blob, parsed).ok());
        OS_REQUIRE(parsed.xs2.wave_size == 32u);
    }

    // 2. Guest Wave32 lane ops across a 64-thread workgroup (2 guest Wave32 halves: lanes 0..31 and 32..63)
    //    On a 64-lane AMD host wave, lanes 0..31 and lanes 32..63 share one HW wave; guest Wave32 MUST
    //    keep their ballots, ReadFirstLane, and ReadLaneAt strictly isolated per 32-lane guest half!
    Rdna2Assembler w;
    // Predicate: wave0 (tid 0..31): tid < 5 (mask = 0x1F); wave1 (tid 32..63): tid >= 60 (bits 28..31 -> mask = 0xF0000000)
    // Simpler deterministic predicate: (tid & 31) < (tid < 32 ? 4 : 8) ->
    //   wave0 active bits = 0x0000000F (count = 4), wave1 active bits = 0x000000FF (count = 8)!
    w.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(31), 0);  // v1 = tid & 31
    w.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 1);        // vcc = (tid < (tid&31)) -> always false; use literal 32
    w.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineInt(32));
    w.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 2);        // vcc = (tid < 32)
    w.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineInt(4));
    w.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 4, Rdna2Operand::InlineInt(8), 3);  // v4 = (tid < 32) ? 4 : 8
    w.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(1), 4);        // vcc = (tid & 31) < (4 or 8)
    w.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 10, Rdna2Operand::VccLo());    // s10 = popcnt(ballot_lo) -> 4 in w0, 8 in w1!
    w.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 11, Rdna2Operand::VccHi());    // s11 = popcnt(ballot_hi) -> ALWAYS 0 in Wave32!
    // ReadFirstLane(tid): must be 0 for wave0 and 32 for wave1!
    w.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 12, Rdna2Operand::Vgpr(0));
    // Pack: (readfirstlane << 16) | (s11 << 8) | s10
    w.EmitSop2(Rdna2Opcode::S_LSHL_B32, 13, Rdna2Operand::Sgpr(12), Rdna2Operand::InlineInt(16));
    w.EmitSop2(Rdna2Opcode::S_LSHL_B32, 14, Rdna2Operand::Sgpr(11), Rdna2Operand::InlineInt(8));
    w.EmitSop2(Rdna2Opcode::S_OR_B32, 15, Rdna2Operand::Sgpr(13), Rdna2Operand::Sgpr(14));
    w.EmitSop2(Rdna2Opcode::S_OR_B32, 16, Rdna2Operand::Sgpr(15), Rdna2Operand::Sgpr(10));
    w.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::Sgpr(16));
    w.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 0);
    w.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 6, 4, 0);
    w.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = w.BuildXs2(ShaderStage::Compute, 64, 1, 1, 20, 16, 32);  // Guest Wave32, 64 threads = 2 guest waves!
    Rdna2CpuExecState cpu{};
    cpu.uav0.assign(64, 0u);
    cpu.uav1.assign(64, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(xs2, cpu).ok());
    for (uint32_t i = 0; i < 32u; ++i) {
        OS_REQUIRE(cpu.uav1[i] == ((0u << 16) | 4u));
    }
    for (uint32_t i = 32u; i < 64u; ++i) {
        OS_REQUIRE(cpu.uav1[i] == ((32u << 16) | 8u));
    }

    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = true;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
    VerifySpirvStrict(lifted.spirv_words, "Wave32Isolation64Threads");
    RunOnVulkanSpirv(lifted.spirv_words, cpu.uav0, cpu.uav1, 1u, "Wave32Isolation64Threads");

#if defined(_WIN32)
    std::vector<uint32_t> init_out(64, 0u);
    RunOnHwAndWarp(lifted.hlsl_source, lifted.pc_dxil, cpu.uav0, init_out, cpu.uav1, 1u, "Wave32Isolation64Threads");
#endif
    std::printf("[PASS] Corpus 5: PSV0 wave lane bounds + Guest Wave32 half isolation across 64 threads (CPU == DXIL GTX1650Ti == WARP == Vulkan SPIR-V + spirv-val)\n");
}

// ---------------------------------------------------------------------------
// Test 6: Mesh/Pixel stage contract, merged_wave_info, GS_ALLOC_REQ, NumThreads/v5,
//         CullPrimitiveEXT (Block+bool), FSSM/vertex-fetch, interp match, DXIL MD5 re-sign
// ---------------------------------------------------------------------------
void TestMeshPixelContractAndDxilResign() {
    // 1. merged_wave_info s3 + ISA-exact GS_ALLOC_REQ 11-bit M0 fields + Mesh NumThreads/v5 + CullPrimitiveEXT Block+bool
    {
        Rdna2Assembler ms;
        // Pack M0 with 11-bit vertex count = 128 and 11-bit primitive count = 96 (both > 63!)
        const uint32_t m0_val = PackGsAllocReqM0(128u, 96u);
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u /* M0 */, Rdna2Operand::LiteralU32(m0_val));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        // Read s3 (merged_wave_info) and v5 (group * NumThreads + lane) and export into Pos0 & Param0
        ms.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(5));   // float(group * NumThreads + lane)
        ms.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 11, Rdna2Operand::Sgpr(3));   // float(merged_wave_info)
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 10, 11, 12, 13, 0xFu, false);
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 10, 11, 12, 13, 0xFu, false);
        // Export packed primitive with cull bit in bit 31 (compr = true -> emits CullPrimitiveEXT Block+bool!)
        // Packed: i0=0, i1=1, i2=2, cull=1 -> 0x80000000 | (2<<20) | (1<<10) | 0
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::LiteralU32(0x80200400u));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 14, 0, 0, 0, 0x1u, true, true /* compr */);
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        // Threadgroup = 16x1x1 (NumThreads = 16), dispatch 3 groups on CPU -> verify v5 == group * 16 + lane!
        const auto xs2_ms = ms.BuildXs2(ShaderStage::Mesh, 16, 1, 1, 16, 20, 32);

        Rdna2CpuExecState cpu{};
        cpu.dispatch_x = 3;
        OS_REQUIRE(ExecuteRdna2BlobCpu(xs2_ms, cpu).ok());
        // Verify ISA-exact 11-bit GS_ALLOC_REQ field extraction from M0
        OS_REQUIRE(cpu.mesh_vertex_count == 128u);
        OS_REQUIRE(cpu.mesh_primitive_count == 96u);
        // Verify v5 seeding = group * 16 + lane across all 3 groups (0..47)
        for (uint32_t g = 0; g < 3u; ++g) {
            for (uint32_t l = 0; l < 16u; ++l) {
                const uint32_t idx = g * 16u + l;
                OS_REQUIRE(cpu.exports[idx].wrote_pos0);
                OS_REQUIRE(std::fabs(cpu.exports[idx].pos0[0] - static_cast<float>(g * 16u + l)) < 1e-4f);
                // Verify s3 merged_wave_info was seeded
                const uint32_t mwi_bits = static_cast<uint32_t>(cpu.exports[idx].pos0[1]);
                OS_REQUIRE(mwi_bits != 0u);
                OS_REQUIRE(cpu.exports[idx].wrote_prim);
                OS_REQUIRE(cpu.exports[idx].culled_prim);
            }
        }

        // Lift Mesh shader to BOTH SPIR-V 1.4 (CullPrimitiveEXT as Block+bool!) and HLSL SM 6.5 (DXC ms_6_5)!
        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_mesh_spirv = true;
        opts.emit_hlsl = true;
        opts.mesh_has_payload = false;
        opts.compile_dxil_with_dxc = true;
        opts.spirv_target_env = "vulkan1.3";
        Rdna2LiftResult lifted{};
        auto st = LiftRdna2Xs2Shader(xs2_ms, lifted, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] Mesh lift: %s\nDXC: %s\nSPV: %s\n",
                         st.message.c_str(), lifted.dxc_diagnostics.c_str(), lifted.spirv_val_diagnostics.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(lifted.ir.mesh_num_threads == 16u);
        OS_REQUIRE(lifted.ir.uses_mesh_global_tid);
        OS_REQUIRE(lifted.ir.uses_merged_wave_info);
        OS_REQUIRE(lifted.ir.uses_mesh_cull);
        OS_REQUIRE(lifted.hlsl_source.find("SV_GroupID") != std::string::npos);
        OS_REQUIRE(lifted.hlsl_source.find("SV_CullPrimitive") != std::string::npos);

        // Validate SPIR-V 1.4 Mesh shader with Block+bool CullPrimitiveEXT against external spirv-val (v2022.4)!
        VerifySpirvStrict(lifted.spirv_words, "MeshCullPrimitiveEXT_BlockBool", "vulkan1.3");
        // Verify compiled DXIL has valid MD5 digest
        OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));
    }

    // 2. FSSM pixel-input + vertex-fetch subroutine table mapping + interpolation modifier matching (VS -> PS)
    {
        StageContractTable contract{};
        // Vertex fetch entry: loads 4 floats from VB0 into v4..v7
        VertexFetchEntry vf{};
        vf.table_index = 0;
        vf.vb_slot = 0;
        vf.byte_offset = 0;
        vf.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
        vf.dst_vgpr = 4;
        vf.num_components = 4;
        contract.vertex_fetches.push_back(vf);
        // FSSM pixel input entry: attr0 -> location 0, Flat (nointerpolation)
        FssmPixelInputEntry fssm{};
        fssm.param_slot = 0;
        fssm.semantic_location = 0;
        fssm.interp_mode = Rdna2InterpMode::Flat;
        fssm.component_mask = 0xFu;
        contract.pixel_inputs.push_back(fssm);

        // VS calls vertex-fetch subroutine via S_SWAPPC_B64, then exports v4..v7 to Pos0 & Param0
        Rdna2Assembler vs;
        vs.EmitSop1(Rdna2Opcode::S_SWAPPC_B64, 8, Rdna2Operand::Sgpr(0));
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 4, 5, 6, 7, 0xFu, false);
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 4, 5, 6, 7, 0xFu, true);
        vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

        // PS reads attr0 via V_INTERP_MOV_F32 (Flat) and exports to Mrt0
        Rdna2Assembler ps;
        ps.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 10, 0, 0, 0);
        ps.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 11, 0, 0, 1);
        ps.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 12, 0, 0, 2);
        ps.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 13, 0, 0, 3);
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 10, 11, 12, 13, 0xFu, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Xs2ShaderContainer c_vs{}, c_ps{};
        c_vs.stage = ShaderStage::Vertex;
        c_vs.code_words = vs.Words();
        c_ps.stage = ShaderStage::Pixel;
        c_ps.code_words = ps.Words();

        std::vector<Rdna2Instruction> ins_vs, ins_ps;
        OS_REQUIRE(DisassembleRdna2(c_vs.code_words, ins_vs).ok());
        OS_REQUIRE(DisassembleRdna2(c_ps.code_words, ins_ps).ok());

        Rdna2IrModule ir_vs{}, ir_ps{};
        OS_REQUIRE(BuildRdna2Ir(c_vs, ins_vs, ir_vs, &contract).ok());
        OS_REQUIRE(BuildRdna2Ir(c_ps, ins_ps, ir_ps, &contract).ok());
        OS_REQUIRE(!ir_ps.input_interp_modes.empty() && ir_ps.input_interp_modes[0] == Rdna2InterpMode::Flat);

        // Reconcile interpolation modifiers between feeder VS and PS
        ReconcileStageInterpolation(ir_vs, ir_ps);
        OS_REQUIRE(!ir_vs.output_interp_modes.empty() && ir_vs.output_interp_modes[0] == Rdna2InterpMode::Flat);

        std::string hlsl_vs, tgt_vs, hlsl_ps, tgt_ps;
        OS_REQUIRE(EmitHlsl(ir_vs, hlsl_vs, tgt_vs, true).ok());
        OS_REQUIRE(EmitHlsl(ir_ps, hlsl_ps, tgt_ps, true).ok());
        OS_REQUIRE(hlsl_vs.find("nointerpolation float4 param0") != std::string::npos);
        OS_REQUIRE(hlsl_ps.find("nointerpolation float4 param0") != std::string::npos);

        std::vector<uint32_t> spv_vs, spv_ps;
        OS_REQUIRE(EmitSpirv(ir_vs, spv_vs).ok());
        OS_REQUIRE(EmitSpirv(ir_ps, spv_ps).ok());
        VerifySpirvStrict(spv_vs, "VertexFetch_FlatOut");
        VerifySpirvStrict(spv_ps, "FssmPixel_FlatIn");

        std::vector<uint8_t> dxil_vs, dxil_ps;
        std::string diag;
        OS_REQUIRE(CompileHlslWithDxc(hlsl_vs, tgt_vs, "main", dxil_vs, diag).ok());
        OS_REQUIRE(CompileHlslWithDxc(hlsl_ps, tgt_ps, "main", dxil_ps, diag).ok());
        OS_REQUIRE(VerifyDxbcContainerHash(dxil_vs.data(), dxil_vs.size()));
        OS_REQUIRE(VerifyDxbcContainerHash(dxil_ps.data(), dxil_ps.size()));
    }

    // 3. DXIL container MD5 hash re-signing (verified bit-for-bit against dxc.exe + live D3D12 PSO)
    {
        Rdna2Assembler cs;
        cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(0x2Au));
        cs.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 2, Rdna2Operand::InlineInt(2), 0);
        cs.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 1, 2, 4, 0);
        cs.EmitSopp(Rdna2Opcode::S_ENDPGM);
        const auto xs2 = cs.BuildXs2(ShaderStage::Compute, 4, 1, 1, 8, 8, 32);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = false;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = true;
        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(xs2, lifted, opts).ok());
        OS_REQUIRE(lifted.pc_dxil.size() >= 32u);

        // Save original dxc.exe-signed MD5 digest
        std::array<uint8_t, 16> orig_digest{};
        std::memcpy(orig_digest.data(), lifted.pc_dxil.data() + 4u, 16u);
        OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));

        // Zero / corrupt the 16-byte DXBC digest and verify detection
        std::memset(lifted.pc_dxil.data() + 4u, 0xAA, 16u);
        OS_REQUIRE(!VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));

        // Re-sign using our DXIL MD5 signer and confirm bit-for-bit match with dxc.exe!
        OS_REQUIRE(ResignDxilContainer(lifted.pc_dxil).ok());
        OS_REQUIRE(VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()));
        OS_REQUIRE(std::memcmp(lifted.pc_dxil.data() + 4u, orig_digest.data(), 16u) == 0);

#if defined(_WIN32)
        // Confirm real D3D12 runtime accepts the re-signed DXIL blob for PSO creation & execution on HW + WARP!
        std::vector<uint32_t> uav0(4, 0u);
        std::vector<uint32_t> init_out(4, 0u);
        std::vector<uint32_t> expected(4, 0x2Au);
        RunOnHwAndWarp(lifted.hlsl_source, lifted.pc_dxil, uav0, init_out, expected, 1u, "ResignedDxilPso", true);
#endif
    }

    std::printf("[PASS] Corpus 6: merged_wave_info/s3 + 11-bit GS_ALLOC_REQ + Mesh NumThreads/v5 + CullPrimitiveEXT Block+bool + FSSM/VS-fetch + DXIL MD5 re-sign\n");
}

}  // namespace

int main() {
    TestAlignByteAndMtbufD16();
    TestVcmpxAndSplitF16Compare();
    TestNanQuietingAndDenormalFloorCeil();
    TestActiveMaskIrPruning();
    TestPsv0WaveBoundsAndWave32Isolation();
    TestMeshPixelContractAndDxilResign();
    std::printf("=== ALL RDNA2 CORPUS PARITY TESTS PASSED ===\n");
    return 0;
}
