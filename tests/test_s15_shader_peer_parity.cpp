// OpenScarlett - MIT License
// Sprint 15 Worker 2: RDNA2 Shader & ISA Peer Parity Verification Suite
// Covers all 7 fresh 08:10 IST Radar Shader Peer Gaps across disasm + IR + CPU-ref + HLSL/DXIL + SPIR-V 1.2/1.3 + live GPU:
//   1. ds_swizzle_b32 full 4-mode semantics (BitMode, QuadPermute, Rotate, FFT/butterfly) (xbox-emulator #184/#186)
//   2. Direct PC-relative branch lowering for s_getpc_b64 + s_add_u32 + s_addc_u32 + s_setpc_b64 (xbox-emulator #185)
//   3. Signed 21-bit SMEM offsets (#206), FLAT/GLOBAL/SCRATCH 12-bit offset pinning vs DLC/GLC/SLC (b2a77f3), LDS D16 loads (#205)
//   4. Safe out-of-range s_buffer_load clamping (Leonida #4573)
//   5. Wave64 Compute dual-half Wave32 / native Wave64 execution (#67/#70/#71, #204) & folded EXECZ CFG fix (Leonida #4574)
//   6. Hardware float/int accuracy (v_fract_f32(±inf/NaN)->qNaN, NaN-safe V_CMP, f64 accuracy, Blackwell select guard, ALU & PS ops)
//   7. SPIR-V 1.2 core descriptor indexing, image_sample_c_lz, authoritative MTBUF format table, truncated DXBC ISGN/ISG1 rejection, DXIL cache diagnostics

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "openscarlett/gpu/shader_cache.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "rdna2_isa_mem_img.h"
#include "rdna2_isa_scalar_vector.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace {

using namespace openscarlett;

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        std::fflush(stderr);
        ++g_failures;
    } else {
        std::printf("[PASS] %s\n", msg);
        std::fflush(stdout);
    }
}

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

uint64_t DoubleToBits(double d) noexcept {
    uint64_t b = 0ull;
    std::memcpy(&b, &d, sizeof(b));
    return b;
}

double BitsToDouble(uint64_t b) noexcept {
    double d = 0.0;
    std::memcpy(&d, &b, sizeof(d));
    return d;
}

bool IsNanBitsF32(uint32_t b) noexcept {
    return (b & 0x7FFFFFFFu) > 0x7F800000u;
}

bool SpirvContainsOpcode(const std::vector<uint32_t>& spv, uint16_t target_op) noexcept {
    size_t pos = 5u;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16u);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0u || pos + wc > spv.size()) break;
        if (op == target_op) return true;
        pos += wc;
    }
    return false;
}

bool SpirvContainsCapability(const std::vector<uint32_t>& spv, uint32_t cap) noexcept {
    size_t pos = 5u;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16u);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0u || pos + wc > spv.size()) break;
        if (op == 17u && wc >= 2u && spv[pos + 1u] == cap) return true;
        pos += wc;
    }
    return false;
}

bool SpirvContainsDecoration(const std::vector<uint32_t>& spv, uint32_t decor) noexcept {
    size_t pos = 5u;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>(spv[pos] >> 16u);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0u || pos + wc > spv.size()) break;
        if (op == 71u && wc >= 3u && spv[pos + 2u] == decor) return true;
        pos += wc;
    }
    return false;
}

bool HasSpirvVal() {
    std::error_code ec;
    return std::filesystem::exists(kDefaultSpirvValPath, ec);
}

bool HasDxc() {
    const std::string p = ResolvePortableDxcPath("");
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec);
}

bool ValidateSpirvStrict(const std::vector<uint32_t>& spv, const char* label, const char* target_env = "vulkan1.2") {
    const Status bin_st = ValidateSpirvBinary(spv);
    if (!bin_st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, bin_st.message.c_str());
        return false;
    }
    if (!HasSpirvVal()) return true;
    std::string diag;
    const Status ext_st = ValidateSpirvExternal(spv, diag, kDefaultSpirvValPath, target_env);
    if (!ext_st.ok()) {
        std::fprintf(stderr, "[FAIL] spirv-val(%s, %s): %s\n", label, target_env, diag.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Portable Live D3D12 Compute Runner (Zero WRL — compiles on MSVC, Clang-CL, MinGW-w64)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
struct LiveD3D12Compute {
    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0u;
    std::string adapter_name;
    uint32_t wave_min = 0u;
    uint32_t highest_sm = 0x65u;
    bool is_warp = false;
    bool ready = false;

    ~LiveD3D12Compute() { Shutdown(); }

    void Shutdown() {
        if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
        if (fence) { fence->Release(); fence = nullptr; }
        if (cmd_list) { cmd_list->Release(); cmd_list = nullptr; }
        if (allocator) { allocator->Release(); allocator = nullptr; }
        if (queue) { queue->Release(); queue = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        if (adapter) { adapter->Release(); adapter = nullptr; }
        if (factory) { factory->Release(); factory = nullptr; }
        ready = false;
    }

    bool Init(bool use_warp) {
        Shutdown();
        is_warp = use_warp;
        if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory))) || !factory) {
            return false;
        }
        HRESULT hr = E_FAIL;
        if (use_warp) {
            hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        } else {
            hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        }
        if (FAILED(hr) || !adapter) return false;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;

        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                     __uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) || !device) {
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1)))) {
            wave_min = opt1.WaveLaneCountMin;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
            return false;
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator)))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd_list)))) {
            return false;
        }
        cmd_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)))) {
            return false;
        }
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        ready = (fence_event != nullptr);
        return ready;
    }

    ID3D12Resource* CreateBuf(uint64_t bytes, D3D12_HEAP_TYPE heap_type,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init_state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<uint64_t>(bytes, 256u);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ID3D12Resource* res = nullptr;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
                                                   __uuidof(ID3D12Resource), reinterpret_cast<void**>(&res)))) {
            return nullptr;
        }
        return res;
    }

    bool Run(const std::vector<uint8_t>& dxil,
             const std::vector<uint32_t>& uav0_in,
             std::vector<uint32_t>& uav1_out,
             const std::vector<uint32_t>& root_consts,
             uint32_t group_x = 1u) {
        if (!ready || dxil.empty()) return false;
        const size_t elem_count = std::max<size_t>(std::max(uav0_in.size(), uav1_out.size()), 64u);
        const uint64_t byte_size = static_cast<uint64_t>(elem_count) * sizeof(uint32_t);
        std::vector<uint32_t> padded_in(elem_count, 0u);
        std::copy(uav0_in.begin(), uav0_in.end(), padded_in.begin());

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[2].Descriptor.ShaderRegister = 0;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3;
        rsd.pParameters = params;
        ID3DBlob* sig_blob = nullptr;
        ID3DBlob* err_blob = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob))) {
            if (err_blob) err_blob->Release();
            return false;
        }
        if (err_blob) err_blob->Release();

        ID3D12RootSignature* root_sig = nullptr;
        const HRESULT rs_hr = device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                                          __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root_sig));
        sig_blob->Release();
        if (FAILED(rs_hr) || !root_sig) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig;
        cpd.CS.pShaderBytecode = dxil.data();
        cpd.CS.BytecodeLength = dxil.size();
        ID3D12PipelineState* pso = nullptr;
        const HRESULT pso_hr = device->CreateComputePipelineState(&cpd, __uuidof(ID3D12PipelineState),
                                                                  reinterpret_cast<void**>(&pso));
        if (FAILED(pso_hr) || !pso) {
            root_sig->Release();
            return false;
        }

        ID3D12Resource* in_up = CreateBuf(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* out_up = CreateBuf(byte_size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* cb_up = CreateBuf(256u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        ID3D12Resource* in_gpu = CreateBuf(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* out_gpu = CreateBuf(byte_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource* rb = CreateBuf(byte_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        bool ok = (in_up && out_up && cb_up && in_gpu && out_gpu && rb);
        if (ok) {
            void* p = nullptr;
            ok = SUCCEEDED(in_up->Map(0, nullptr, &p));
            if (ok) { std::memcpy(p, padded_in.data(), static_cast<size_t>(byte_size)); in_up->Unmap(0, nullptr); }
            if (ok) {
                ok = SUCCEEDED(out_up->Map(0, nullptr, &p));
                if (ok) { std::memset(p, 0, static_cast<size_t>(byte_size)); out_up->Unmap(0, nullptr); }
            }
            if (ok) {
                ok = SUCCEEDED(cb_up->Map(0, nullptr, &p));
                if (ok) {
                    std::memset(p, 0, 256u);
                    const size_t rc_bytes = std::min<size_t>(root_consts.size() * 4u, 256u);
                    if (rc_bytes > 0u) std::memcpy(p, root_consts.data(), rc_bytes);
                    cb_up->Unmap(0, nullptr);
                }
            }
        }
        if (ok) {
            ok = SUCCEEDED(allocator->Reset()) && SUCCEEDED(cmd_list->Reset(allocator, pso));
        }
        if (ok) {
            cmd_list->CopyBufferRegion(in_gpu, 0, in_up, 0, byte_size);
            cmd_list->CopyBufferRegion(out_gpu, 0, out_up, 0, byte_size);
            D3D12_RESOURCE_BARRIER b2[2]{};
            b2[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2[0].Transition = {in_gpu, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            b2[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2[1].Transition = {out_gpu, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
            cmd_list->ResourceBarrier(2, b2);
            cmd_list->SetComputeRootSignature(root_sig);
            cmd_list->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
            cmd_list->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
            cmd_list->SetComputeRootConstantBufferView(2, cb_up->GetGPUVirtualAddress());
            cmd_list->Dispatch(group_x, 1, 1);

            D3D12_RESOURCE_BARRIER ub{};
            ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            ub.UAV.pResource = out_gpu;
            cmd_list->ResourceBarrier(1, &ub);

            D3D12_RESOURCE_BARRIER tr{};
            tr.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tr.Transition = {out_gpu, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            cmd_list->ResourceBarrier(1, &tr);
            cmd_list->CopyBufferRegion(rb, 0, out_gpu, 0, byte_size);
            ok = SUCCEEDED(cmd_list->Close());
        }
        if (ok) {
            ID3D12CommandList* lists[] = {cmd_list};
            queue->ExecuteCommandLists(1, lists);
            ++fence_val;
            ok = SUCCEEDED(queue->Signal(fence, fence_val));
            if (ok && fence->GetCompletedValue() < fence_val) {
                ok = SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event));
                if (ok) WaitForSingleObject(fence_event, 15000);
            }
        }
        if (ok) {
            void* p = nullptr;
            ok = SUCCEEDED(rb->Map(0, nullptr, &p));
            if (ok) {
                std::vector<uint32_t> read_vec(elem_count, 0u);
                std::memcpy(read_vec.data(), p, static_cast<size_t>(byte_size));
                rb->Unmap(0, nullptr);
                if (uav1_out.empty()) uav1_out = std::move(read_vec);
                else std::copy_n(read_vec.begin(), std::min(read_vec.size(), uav1_out.size()), uav1_out.begin());
            }
        }
        if (rb) rb->Release();
        if (out_gpu) out_gpu->Release();
        if (in_gpu) in_gpu->Release();
        if (cb_up) cb_up->Release();
        if (out_up) out_up->Release();
        if (in_up) in_up->Release();
        pso->Release();
        root_sig->Release();
        return ok;
    }
};
#endif

// Helper: execute lifted compute shader on live D3D12 (HW and/or WARP) and live Vulkan (when ICD available)
void VerifyLiveComputeParity(
    const Rdna2LiftResult& lift,
    const std::vector<uint32_t>& uav0_in,
    const std::vector<uint32_t>& expected_uav1,
    const std::vector<uint32_t>& root_consts,
    const char* label,
    bool compare_nan_as_any_nan = false,
    bool allow_warp_wave_guard = false,
    bool test_vulkan = true) {
    auto eq_vec = [&](const std::vector<uint32_t>& got) -> bool {
        if (got.size() < expected_uav1.size()) return false;
        for (size_t i = 0u; i < expected_uav1.size(); ++i) {
            if (compare_nan_as_any_nan && IsNanBitsF32(expected_uav1[i])) {
                if (!IsNanBitsF32(got[i])) {
                    std::fprintf(stderr, "  [%s] mismatch at [%zu]: got=0x%08X expected=NaN\n", label, i, got[i]);
                    return false;
                }
            } else if (got[i] != expected_uav1[i]) {
                std::fprintf(stderr, "  [%s] mismatch at [%zu]: got=0x%08X expected=0x%08X\n",
                             label, i, got[i], expected_uav1[i]);
                return false;
            }
        }
        return true;
    };

#if defined(_WIN32)
    if (!lift.pc_dxil.empty()) {
        static LiveD3D12Compute hw_ctx;
        static LiveD3D12Compute warp_ctx;
        static bool init_done = false;
        if (!init_done) {
            (void)hw_ctx.Init(false);
            (void)warp_ctx.Init(true);
            init_done = true;
            std::printf("[INFO] Live D3D12 adapters: HW=%s (wave_min=%u) | WARP=%s (wave_min=%u)\n",
                        hw_ctx.ready ? hw_ctx.adapter_name.c_str() : "<none>", hw_ctx.wave_min,
                        warp_ctx.ready ? warp_ctx.adapter_name.c_str() : "<none>", warp_ctx.wave_min);
        }
        if (hw_ctx.ready) {
            std::vector<uint8_t> dev_dxil = lift.pc_dxil;
            if (hw_ctx.highest_sm < 0x65u && !lift.hlsl_source.empty() && HasDxc()) {
                std::string diag;
                const std::string prof = SelectTargetProfileForStage(ShaderStage::Compute, hw_ctx.highest_sm);
                (void)CompileHlslWithDxc(lift.hlsl_source, prof, "main", dev_dxil, diag);
            }
            std::vector<uint32_t> out(expected_uav1.size(), 0u);
            const bool ran = hw_ctx.Run(dev_dxil, uav0_in, out, root_consts, 1u);
            Check(ran && eq_vec(out), (std::string("Live D3D12 HW parity: ") + label).c_str());
        }
        if (warp_ctx.ready && !(allow_warp_wave_guard && warp_ctx.wave_min <= 8u)) {
            std::vector<uint8_t> dev_dxil = lift.pc_dxil;
            if (warp_ctx.highest_sm < 0x65u && !lift.hlsl_source.empty() && HasDxc()) {
                std::string diag;
                const std::string prof = SelectTargetProfileForStage(ShaderStage::Compute, warp_ctx.highest_sm);
                (void)CompileHlslWithDxc(lift.hlsl_source, prof, "main", dev_dxil, diag);
            }
            std::vector<uint32_t> out(expected_uav1.size(), 0u);
            const bool ran = warp_ctx.Run(dev_dxil, uav0_in, out, root_consts, 1u);
            Check(ran && eq_vec(out), (std::string("Live D3D12 WARP parity: ") + label).c_str());
        }
    }
#endif

    if (test_vulkan && !lift.spirv_words.empty() && root_consts.empty() &&
        ValidateSpirvStrict(lift.spirv_words, label)) {
        static VulkanBackend vk;
        static bool vk_init = false;
        static bool vk_hw_ok = false;
        if (!vk_init) {
            vk_init = true;
            vk_hw_ok = vk.Initialize(false).ok() && !vk.is_fallback_mode();
            std::printf("[INFO] Live Vulkan ICD: %s (subgroup=%u)\n",
                        vk_hw_ok ? vk.GetDeviceInfo().device_name.c_str() : "<none>",
                        vk_hw_ok ? vk.GetDeviceInfo().subgroup_size : 0u);
        }
        if (vk_hw_ok) {
            std::vector<uint32_t> out(expected_uav1.size(), 0u);
            const Status vst = vk.ExecuteComputeShader(lift.spirv_words, uav0_in, out, 1u);
            Check(vst.ok() && eq_vec(out), (std::string("Live Vulkan HW parity: ") + label).c_str());
        }
    }
}

// ===========================================================================
// Deliverable 1: ds_swizzle_b32 full 4-mode semantics (xbox-emulator #184/#186)
// ===========================================================================
void TestDeliverable1DsSwizzleAllModes() {
    std::printf("\n=== Deliverable 1: ds_swizzle_b32 Full 4-Mode Semantics ===\n");

    // 1a. Unit-verify all 4 modes of DecodeRdna2DsSwizzleOffset + ComputeRdna2DsSwizzleSrcLane
    {
        // BitMode: bit15=1, xor=1, or=0, and=31 -> 0x841F (pair swap)
        const auto bit_info = DecodeRdna2DsSwizzleOffset(0x841Fu);
        Check(bit_info.mode == Rdna2DsSwizzleMode::BitMode &&
                  bit_info.and_mask == 31u && bit_info.or_mask == 0u && bit_info.xor_mask == 1u,
              "ds_swizzle_b32 BitMode decode (0x841F -> and=31,or=0,xor=1)");
        bool bit_ok = true;
        for (uint32_t l = 0u; l < 32u; ++l) {
            if (ComputeRdna2DsSwizzleSrcLane(l, 0x841Fu) != (l ^ 1u)) bit_ok = false;
        }
        Check(bit_ok, "ds_swizzle_b32 BitMode src_lane == lane ^ 1 across all 32 lanes");

        // QuadPermute: bit15=0, [14:13]=0, offset=0x001B -> {3, 2, 1, 0} quad reverse
        const auto q_info = DecodeRdna2DsSwizzleOffset(0x001Bu);
        Check(q_info.mode == Rdna2DsSwizzleMode::QuadPermute &&
                  q_info.quad_sel[0] == 3u && q_info.quad_sel[1] == 2u &&
                  q_info.quad_sel[2] == 1u && q_info.quad_sel[3] == 0u,
              "ds_swizzle_b32 QuadPermute decode (0x001B -> {3,2,1,0})");
        bool quad_ok = true;
        for (uint32_t l = 0u; l < 32u; ++l) {
            if (ComputeRdna2DsSwizzleSrcLane(l, 0x001Bu) != ((l & ~3u) | (3u - (l & 3u)))) quad_ok = false;
        }
        Check(quad_ok, "ds_swizzle_b32 QuadPermute reverses each 4-lane quad across 32 lanes");

        // Rotate: bit15=0, [14:13]=1 (0x2000), cluster_mask=7 (bits[9:5] -> 8-lane cluster), rot=1 (bits[4:0]) -> 0x20E1
        const uint16_t rot_off = static_cast<uint16_t>(0x2000u | (7u << 5u) | 1u);
        const auto r_info = DecodeRdna2DsSwizzleOffset(rot_off);
        Check(r_info.mode == Rdna2DsSwizzleMode::Rotate &&
                  r_info.rotate_cluster_mask == 7u && r_info.rotate_shift == 1u,
              "ds_swizzle_b32 Rotate decode (0x20E1 -> rotate_cluster_mask=7, rotate_shift=1)");
        bool rot_ok = true;
        for (uint32_t l = 0u; l < 32u; ++l) {
            const uint32_t exp = (l & ~7u) | ((l - 1u) & 7u);
            if (ComputeRdna2DsSwizzleSrcLane(l, rot_off) != exp) rot_ok = false;
        }
        Check(rot_ok, "ds_swizzle_b32 Rotate rotates-right within 8-lane cluster across 32 lanes");

        // FFT/Butterfly: bit15=0, [14:13]=2 (0x4000), fft_mask=4 -> 0x4004
        const uint16_t fft_off = 0x4004u;
        const auto f_info = DecodeRdna2DsSwizzleOffset(fft_off);
        Check(f_info.mode == Rdna2DsSwizzleMode::Fft && f_info.fft_mask == 4u,
              "ds_swizzle_b32 FFT/Butterfly decode (0x4004 -> fft_mask=4)");
        bool fft_ok = true;
        for (uint32_t l = 0u; l < 32u; ++l) {
            if (ComputeRdna2DsSwizzleSrcLane(l, fft_off) != (l ^ 4u)) fft_ok = false;
        }
        Check(fft_ok, "ds_swizzle_b32 FFT/Butterfly swaps lane ^ 4 across 32 lanes");
    }

    // 1b. End-to-end RDNA2 shader running ALL 4 modes -> CPU-ref + HLSL/DXIL + SPIR-V + live GPU!
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);  // v1 = tid * 4
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);                 // v2 = uav0[tid]
        // Mode 1: BitMode 0x841F (lane ^ 1) -> v3
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 3, 2, 0, 0, 0x841Fu);
        // Mode 2: QuadPermute 0x001B (quad reverse) -> v4
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 4, 2, 0, 0, 0x001Bu);
        // Mode 3: Rotate 0x20E1 (8-lane cluster rotate by +1) -> v5
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 5, 2, 0, 0, static_cast<uint16_t>(0x2000u | (7u << 5u) | 1u));
        // Mode 4: FFT 0x4004 (lane ^ 4) -> v6
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 6, 2, 0, 0, 0x4004u);
        // Store all 4 outputs to uav1[tid + {0, 32, 64, 96}]
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 128);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 384);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Rdna2CpuExecState cpu{};
        cpu.uav0.resize(128u, 0u);
        cpu.uav1.resize(128u, 0u);
        for (uint32_t i = 0u; i < 32u; ++i) cpu.uav0[i] = 1000u + i * 7u;
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "ds_swizzle_b32 4-mode shader executes on CPU-ref");

        bool cpu_exact = true;
        for (uint32_t i = 0u; i < 32u; ++i) {
            if (cpu.uav1[i] != cpu.uav0[i ^ 1u]) cpu_exact = false;
            if (cpu.uav1[32u + i] != cpu.uav0[(i & ~3u) | (3u - (i & 3u))]) cpu_exact = false;
            if (cpu.uav1[64u + i] != cpu.uav0[(i & ~7u) | ((i - 1u) & 7u)]) cpu_exact = false;
            if (cpu.uav1[96u + i] != cpu.uav0[i ^ 4u]) cpu_exact = false;
        }
        Check(cpu_exact, "ds_swizzle_b32 CPU-ref matches BitMode/QuadPermute/Rotate/FFT bit-for-bit");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "ds_swizzle_b32 4-mode shader lifts to HLSL/DXIL + SPIR-V");
        Check(lift.hlsl_source.find("WaveReadLaneAt") != std::string::npos,
              "ds_swizzle_b32 HLSL emits WaveReadLaneAt");
        Check(SpirvContainsOpcode(lift.spirv_words, 345u /*OpGroupNonUniformShuffle*/),
              "ds_swizzle_b32 SPIR-V emits OpGroupNonUniformShuffle");
        Check(ValidateSpirvStrict(lift.spirv_words, "ds_swizzle_b32_4modes"),
              "ds_swizzle_b32 4-mode SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "ds_swizzle_b32_4modes", false, true, true);
    }
}

// ===========================================================================
// Deliverable 2: Direct PC-relative branch lowering (xbox-emulator #185)
// ===========================================================================
void TestDeliverable2PcRelativeBranchLowering() {
    std::printf("\n=== Deliverable 2: Direct PC-Relative Branch Lowering ===\n");

    // 2a. Forward constant-offset PC-relative branch: s_getpc_b64 + s_add_u32 + s_addc_u32 + s_setpc_b64
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);  // v1 = tid * 4
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(11));        // v4 = 11 (initial)

        // s_getpc_b64 s[6:7] returns byte address of the NEXT instruction (after s_getpc_b64)
        a.EmitSop1(Rdna2Opcode::S_GETPC_B64, 6, Rdna2Operand::InlineInt(0));
        const uint32_t pc_after_getpc_words = a.CurrentWordOffset();
        // s_add_u32 s6, s6, <delta_bytes> (64-bit with LiteralU32, 2 words)
        // s_addc_u32 s7, s7, 0 (1 word)
        // s_setpc_b64 s[6:7] (1 word)
        // poison: v_mov_b32 v4, 999 (2 words with LiteralU32)
        // target: v_add_nc_u32 v4, v0, 42 (v_mov_b32 v5, 42 -> 1 word; v_add_nc_u32 v4, v0, v5 -> 1 word)
        // Delta from pc_after_getpc_words to target = (2 + 1 + 1 + 2) * 4 = 24 bytes!
        const uint32_t expected_delta_bytes = 24u;
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 6, Rdna2Operand::Sgpr(6), Rdna2Operand::LiteralU32(expected_delta_bytes));
        a.EmitSop2(Rdna2Opcode::S_ADDC_U32, 7, Rdna2Operand::Sgpr(7), Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_SETPC_B64, 0, Rdna2Operand::Sgpr(6));
        // Dead poison block (must be skipped!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::LiteralU32(999u));
        const uint32_t actual_target_words = a.CurrentWordOffset();
        Check((actual_target_words - pc_after_getpc_words) * 4u == expected_delta_bytes,
              "Forward PC-relative byte delta matches assembled instruction layout");
        // Target block
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::InlineInt(42));
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::Vgpr(0), 5);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Xs2ShaderContainer parsed{};
        std::vector<Rdna2Instruction> dec;
        Check(ParseXs2Container(xs2, parsed).ok() && DisassembleRdna2(parsed.code_words, dec).ok(),
              "PC-relative s_getpc_b64 + s_add_u32 + s_addc_u32 + s_setpc_b64 disassembles cleanly");

        Rdna2IrModule ir{};
        Check(BuildRdna2Ir(parsed, dec, ir).ok(), "PC-relative jump lowers to SSA IR");
        bool has_direct_branch_to_target = false;
        for (const auto& bb : ir.blocks) {
            if (!bb.instructions.empty() && bb.instructions.back().op == IrOp::Branch) {
                const uint32_t tgt_bb = bb.instructions.back().true_block;
                if (tgt_bb < ir.blocks.size() && ir.blocks[tgt_bb].start_word_offset == actual_target_words) {
                    has_direct_branch_to_target = true;
                }
            }
        }
        Check(has_direct_branch_to_target,
              "s_getpc_b64 + s_add_u32 + s_addc_u32 + s_setpc_b64 lowers to direct CFG Branch edge to resolved target");

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(32u, 0u);
        cpu.uav1.assign(32u, 0u);
        Check(ExecuteRdna2Cpu(parsed, dec, cpu).ok(), "Forward PC-relative jump executes on CPU-ref");
        bool cpu_ok = true;
        for (uint32_t i = 0u; i < 32u; ++i) {
            if (cpu.uav1[i] != 42u + i) cpu_ok = false;
        }
        Check(cpu_ok, "Forward PC-relative jump skips poison 999 and computes 42 + tid");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Forward PC-relative jump lifts to HLSL/DXIL + SPIR-V");
        Check(ValidateSpirvStrict(lift.spirv_words, "pc_rel_fwd_jump"),
              "Forward PC-relative jump SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "pc_rel_fwd_jump");
    }

    // 2b. Backward negative-offset PC-relative loop with s_add_u32 + s_addc_u32(0xFFFFFFFF) carry propagation
    {
        Rdna2Assembler a;
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(0));  // s2 = acc = 0
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 3, Rdna2Operand::InlineInt(4));  // s3 = counter = 4
        const uint32_t loop_hdr = a.CurrentWordOffset();
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 2, Rdna2Operand::Sgpr(2), Rdna2Operand::InlineInt(10));  // s2 += 10
        a.EmitSop2(Rdna2Opcode::S_SUB_U32, 3, Rdna2Operand::Sgpr(3), Rdna2Operand::InlineInt(1));   // s3 -= 1
        a.EmitSopc(Rdna2Opcode::S_CMP_EQ_U32, Rdna2Operand::Sgpr(3), Rdna2Operand::InlineInt(0));   // scc = (s3 == 0)
        const uint32_t exit_br = a.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC1, 0);
        a.EmitSop1(Rdna2Opcode::S_GETPC_B64, 6, Rdna2Operand::InlineInt(0));
        const uint32_t pc_after_getpc = a.CurrentWordOffset();
        const int32_t neg_delta_bytes = -static_cast<int32_t>((pc_after_getpc - loop_hdr) * 4u);
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 6, Rdna2Operand::Sgpr(6),
                   Rdna2Operand::LiteralU32(static_cast<uint32_t>(neg_delta_bytes)));
        a.EmitSop2(Rdna2Opcode::S_ADDC_U32, 7, Rdna2Operand::Sgpr(7), Rdna2Operand::LiteralU32(0xFFFFFFFFu));
        a.EmitSop1(Rdna2Opcode::S_SETPC_B64, 0, Rdna2Operand::Sgpr(6));
        const uint32_t exit_lbl = a.CurrentWordOffset();
        a.PatchBranch(exit_br, exit_lbl);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::Sgpr(2));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 0, 4, 0, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 1, 1, 1, 16, 16, 32);
        Xs2ShaderContainer parsed{};
        std::vector<Rdna2Instruction> dec;
        Check(ParseXs2Container(xs2, parsed).ok() && DisassembleRdna2(parsed.code_words, dec).ok(),
              "Backward negative-offset PC-relative loop disassembles");
        Rdna2IrModule ir{};
        Check(BuildRdna2Ir(parsed, dec, ir).ok(), "Backward negative-offset PC-relative loop lowers to SSA IR");
        Rdna2CfgAnalysis cfg{};
        AnalyzeRdna2Cfg(ir, cfg);
        Check(cfg.has_cycles && cfg.natural_loop_count >= 1u,
              "Backward negative-offset PC-relative jump creates natural loop back-edge in CFG");
        Rdna2CpuExecState cpu{};
        cpu.uav1.assign(4u, 0u);
        Check(ExecuteRdna2Cpu(parsed, dec, cpu).ok() && cpu.uav1[0] == 40u,
              "Backward negative-offset PC-relative loop executes 4 iterations -> 40 on CPU-ref");
    }
}

// ===========================================================================
// Deliverable 3: Signed 21-bit SMEM offsets (#206), FLAT/GLOBAL/SCRATCH 12-bit
//                offset pinning vs DLC/GLC/SLC (b2a77f3), LDS D16 loads (#205)
// ===========================================================================
void TestDeliverable3SmemFlatOffsetAndLdsD16() {
    std::printf("\n=== Deliverable 3: Signed SMEM, FLAT/GLOBAL/SCRATCH Pinning & LDS D16 ===\n");

    // 3a. Signed 21-bit SMEM immediate offset (bit 20 sign bit)
    {
        Rdna2Assembler a;
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::InlineInt(16));
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 5, 0, -8, 4u /* soffset = s4 -> 16 + (-8) = 8 = dword[2] */);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::Sgpr(5));
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Xs2ShaderContainer parsed{};
        std::vector<Rdna2Instruction> dec;
        Check(ParseXs2Container(xs2, parsed).ok() && DisassembleRdna2(parsed.code_words, dec).ok(),
              "Signed 21-bit SMEM offset (-8) disassembles cleanly");
        Check(dec.size() >= 2u && dec[1].opcode == Rdna2Opcode::S_BUFFER_LOAD_DWORD &&
                  dec[1].mem_offset == -8 && ((dec[1].raw_words[1] >> 20u) & 1u) == 1u,
              "SMEM 21-bit sign bit (bit 20) decodes exact signed mem_offset == -8");

        Rdna2CpuExecState cpu{};
        cpu.root_constants = {0x11111111u, 0x22222222u, 0xCAFEBABEu, 0x44444444u};
        cpu.uav0.assign(32u, 0u);
        cpu.uav0[2] = 0xCAFEBABEu;
        cpu.uav1.assign(32u, 0u);
        Check(ExecuteRdna2Cpu(parsed, dec, cpu).ok() && cpu.uav1[0] == 0xCAFEBABEu && cpu.uav1[31] == 0xCAFEBABEu,
              "Signed SMEM offset (soffset=16 + offset=-8 -> dword[2]) returns 0xCAFEBABE on CPU-ref");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Signed SMEM shader lifts to HLSL/DXIL + SPIR-V");
        Check(ValidateSpirvStrict(lift.spirv_words, "smem_signed_21bit"),
              "Signed SMEM SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "smem_signed_21bit");

        // Also test scalar-vector pipeline SMEM signed offset + soffset!
        std::vector<rdna2_sv::SvDecodedInst> sv_dec;
        Check(rdna2_sv::DecodeScalarVectorStream(parsed.code_words, sv_dec).ok(),
              "rdna2_sv decodes signed SMEM offset");
        rdna2_sv::SvWaveState sv_st{};
        sv_st.Reset(32u);
        sv_st.root_constants = cpu.root_constants;
        sv_st.uav1.assign(32u, 0u);
        for (uint32_t l = 0u; l < 32u; ++l) sv_st.vgpr[l][0] = l;
        Check(rdna2_sv::ExecuteScalarVectorCpu(sv_dec, sv_st).ok() && sv_st.uav1[0] == 0xCAFEBABEu,
              "rdna2_sv CPU-ref evaluates signed SMEM offset (16 + -8 = 8) accurately");
        Rdna2LiftResult sv_lift{};
        Check(rdna2_sv::LiftScalarVectorShader(parsed, sv_dec, sv_lift, opts).ok() &&
                  ValidateSpirvStrict(sv_lift.spirv_words, "sv_smem_signed_21bit"),
              "rdna2_sv signed SMEM lifts to HLSL/DXIL + spirv-val clean SPIR-V");
    }

    // 3b. FLAT (12-bit unsigned), GLOBAL (12-bit signed), SCRATCH (12-bit signed) pinned against DLC(12)/GLC(16)/SLC(17)
    {
        Rdna2Assembler a;
        a.EmitGlobal(Rdna2Opcode::FLAT_LOAD_DWORD, 2, 0, static_cast<int16_t>(4095), 0x7Fu, true, true, true);
        a.EmitGlobal(Rdna2Opcode::GLOBAL_LOAD_DWORD, 3, 0, -2048, 0x7Fu, true, false, true);
        a.EmitGlobal(Rdna2Opcode::GLOBAL_LOAD_DWORD, 4, 0, 2047, 0x7Fu, false, true, false);
        a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 5, 0, -16, 0x7Fu, true, true, true);
        a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 5, 0, 128, 0x7Fu, false, true, true);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> dec;
        Check(DisassembleRdna2(a.Words(), dec).ok() && dec.size() == 6u,
              "FLAT/GLOBAL/SCRATCH with 12-bit boundary offsets + DLC/GLC/SLC disassembles cleanly");
        // Verify exact bit layout and zero bleed into/out of [11:0] vs dlc(12)/seg(15:14)/glc(16)/slc(17)
        Check(dec[0].opcode == Rdna2Opcode::FLAT_LOAD_DWORD && dec[0].mem_offset == 4095 &&
                  dec[0].dlc && dec[0].glc && dec[0].slc && ((dec[0].raw_words[0] >> 14u) & 3u) == 0u,
              "FLAT_LOAD_DWORD 12-bit unsigned offset=4095 pinned cleanly against DLC/GLC/SLC");
        Check(dec[1].opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD && dec[1].mem_offset == -2048 &&
                  dec[1].dlc && !dec[1].glc && dec[1].slc && ((dec[1].raw_words[0] >> 14u) & 3u) == 2u,
              "GLOBAL_LOAD_DWORD 12-bit signed offset=-2048 pinned cleanly against DLC/GLC/SLC");
        Check(dec[2].opcode == Rdna2Opcode::GLOBAL_LOAD_DWORD && dec[2].mem_offset == 2047 &&
                  !dec[2].dlc && dec[2].glc && !dec[2].slc && ((dec[2].raw_words[0] >> 14u) & 3u) == 2u,
              "GLOBAL_LOAD_DWORD 12-bit signed offset=+2047 pinned cleanly against DLC/GLC/SLC");
        Check(dec[3].opcode == Rdna2Opcode::SCRATCH_LOAD_DWORD && dec[3].mem_offset == -16 &&
                  dec[3].dlc && dec[3].glc && dec[3].slc && ((dec[3].raw_words[0] >> 14u) & 3u) == 1u,
              "SCRATCH_LOAD_DWORD 12-bit signed offset=-16 pinned cleanly against DLC/GLC/SLC");
        Check(dec[4].opcode == Rdna2Opcode::SCRATCH_STORE_DWORD && dec[4].mem_offset == 128 &&
                  !dec[4].dlc && dec[4].glc && dec[4].slc && ((dec[4].raw_words[0] >> 14u) & 3u) == 1u,
              "SCRATCH_STORE_DWORD 12-bit signed offset=+128 pinned cleanly against DLC/GLC/SLC");
    }

    // 3c. All 6 LDS D16 loads (ds_read_u8/i8/u16_d16[_hi]) across CPU-ref, HLSL, SPIR-V, and live GPU!
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);  // v1 = tid * 4
        // Write 0x81FE00ABu to LDS[tid * 4]
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x81FE00ABu));
        a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
        a.EmitSopp(Rdna2Opcode::S_BARRIER);
        // Seed v3..v8 with sentinel 0xDEADBEEFu to prove half-word preservation!
        for (uint8_t r = 3u; r <= 8u; ++r) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, r, Rdna2Operand::LiteralU32(0xDEADBEEFu));
        }
        // byte0 = 0xAB (-85), byte2 = 0xFE (-2), half0 = 0x00AB, half1 = 0x81FE
        a.EmitDs(Rdna2Opcode::DS_READ_U8_D16, 3, 1, 0, 0, 0u);     // -> 0xDEAD00AB
        a.EmitDs(Rdna2Opcode::DS_READ_U8_D16_HI, 4, 1, 0, 0, 2u);  // -> 0x00FEBEEF
        a.EmitDs(Rdna2Opcode::DS_READ_I8_D16, 5, 1, 0, 0, 0u);     // -> 0xDEADFFAB (-85 in i16)
        a.EmitDs(Rdna2Opcode::DS_READ_I8_D16_HI, 6, 1, 0, 0, 3u);  // byte3=0x81 (-127 -> 0xFF81) -> 0xFF81BEEF
        a.EmitDs(Rdna2Opcode::DS_READ_U16_D16, 7, 1, 0, 0, 0u);    // -> 0xDEAD00AB
        a.EmitDs(Rdna2Opcode::DS_READ_U16_D16_HI, 8, 1, 0, 0, 2u); // -> 0x81FEBEEF

        // Store all 6 outputs at uav1[tid + k*32]
        for (uint32_t k = 0u; k < 6u; ++k) {
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, static_cast<uint8_t>(3u + k), 1, 4, static_cast<uint16_t>(k * 128u));
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(192u, 0u);
        cpu.uav1.assign(192u, 0u);
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "LDS D16 6-variant shader executes on CPU-ref");
        const uint32_t expected[6] = {
            0xDEAD00ABu, 0x00FEBEEFu, 0xDEADFFABu, 0xFF81BEEFu, 0xDEAD00ABu, 0x81FEBEEFu
        };
        bool d16_ok = true;
        for (uint32_t k = 0u; k < 6u; ++k) {
            for (uint32_t i = 0u; i < 32u; ++i) {
                if (cpu.uav1[k * 32u + i] != expected[k]) d16_ok = false;
            }
        }
        Check(d16_ok, "All 6 LDS D16 load variants preserve untouched half & extend loaded value accurately");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "LDS D16 6-variant shader lifts to HLSL/DXIL + SPIR-V");
        Check(ValidateSpirvStrict(lift.spirv_words, "lds_d16_loads"),
              "LDS D16 6-variant SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "lds_d16_loads");

        // Also verify rdna2_isa_mem_img LDS D16 CPU + HLSL + SPIR-V path on a pure MemImg stream!
        std::vector<uint32_t> mwords;
        auto add_ds = [&](const char* mnem, uint8_t vdst, uint8_t vaddr, uint8_t d0, uint16_t off) {
            rdna2_mem::MemImgEncodeParams mp{};
            mp.vdst = vdst;
            mp.vaddr = vaddr;
            mp.data0 = d0;
            mp.offset = off;
            return rdna2_mem::EncodeMemImgInst(mnem, mp, mwords);
        };
        Check(add_ds("ds_write_b32", 0u, 1u, 2u, 0u) &&
                  add_ds("ds_read_u8_d16", 3u, 1u, 0u, 0u) &&
                  add_ds("ds_read_u8_d16_hi", 4u, 1u, 0u, 2u) &&
                  add_ds("ds_read_i8_d16", 5u, 1u, 0u, 0u) &&
                  add_ds("ds_read_i8_d16_hi", 6u, 1u, 0u, 3u) &&
                  add_ds("ds_read_u16_d16", 7u, 1u, 0u, 0u) &&
                  add_ds("ds_read_u16_d16_hi", 8u, 1u, 0u, 2u),
              "rdna2_mem encodes all 6 LDS D16 load variants");
        mwords.push_back(0xBF810000u);  // s_endpgm
        std::vector<rdna2_mem::MemImgDecodedInst> mdec;
        Check(rdna2_mem::DecodeMemImgStream(mwords, mdec).ok() && mdec.size() == 8u,
              "rdna2_mem decodes all 6 LDS D16 load variants");
        rdna2_mem::MemImgExecState mst{};
        mst.wave_size = 1u;
        mst.exec_mask = 1ull;
        mst.vgpr[0][1] = 0u;
        mst.vgpr[0][2] = 0x81FE00ABu;
        for (uint8_t r = 3u; r <= 8u; ++r) mst.vgpr[0][r] = 0xDEADBEEFu;
        Check(rdna2_mem::ExecuteMemImgCpu(mdec, mst).ok() &&
                  mst.vgpr[0][3] == 0xDEAD00ABu && mst.vgpr[0][4] == 0x00FEBEEFu &&
                  mst.vgpr[0][5] == 0xDEADFFABu && mst.vgpr[0][6] == 0xFF81BEEFu &&
                  mst.vgpr[0][7] == 0xDEAD00ABu && mst.vgpr[0][8] == 0x81FEBEEFu,
              "rdna2_mem CPU-ref evaluates all 6 LDS D16 load variants accurately");
        Xs2ShaderContainer mc{};
        mc.stage = ShaderStage::Compute;
        mc.threadgroup_x = 32u;
        mc.wave_size = 32u;
        mc.code_words = mwords;
        Rdna2LiftResult mlift{};
        Check(rdna2_mem::LiftMemImgShader(mc, mdec, mlift, opts).ok(),
              "rdna2_mem lifts all 6 LDS D16 loads to HLSL/DXIL + SPIR-V");
        Check(mlift.hlsl_source.find("0xFFFF0000u") != std::string::npos &&
                  mlift.hlsl_source.find("0x0000FFFFu") != std::string::npos,
              "rdna2_mem HLSL preserves untouched 16-bit half for LDS D16 loads");
        Check(ValidateSpirvStrict(mlift.spirv_words, "mem_img_lds_d16"),
              "rdna2_mem LDS D16 SPIR-V passes spirv-val --target-env vulkan1.2");
    }
}

// ===========================================================================
// Deliverable 4: Safe out-of-range s_buffer_load clamping (Leonida #4573)
// ===========================================================================
void TestDeliverable4SafeOobSBufferLoad() {
    std::printf("\n=== Deliverable 4: Safe Out-Of-Range s_buffer_load Clamping ===\n");

    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // Valid read from uav0 (sbase=0, dynamic soffset=s7=0, offset=0 -> uav0[0]): s4
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 7, Rdna2Operand::InlineInt(0));
    a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0, 7u);
    // Out-of-range positive read (> 64KB = 0x20000): s5 -> must clamp to 0!
    a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 5, 0, 0x20000);
    // Negative effective signed offset (soffset=s8=4, offset=-16 -> -12): s6 -> must clamp to 0!
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::InlineInt(4));
    a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 6, 0, -16, 8u);

    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(4));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::Sgpr(5));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::Sgpr(6));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 1, 4, 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 128);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 256);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
    Rdna2CpuExecState cpu{};
    cpu.uav0.assign(96u, 0u);
    cpu.uav0[0] = 0x13579BDFu;
    cpu.uav1.assign(96u, 0xFFFFFFFFu);
    Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "Safe OOB s_buffer_load executes on CPU-ref without faulting");
    Check(cpu.uav1[0] == 0x13579BDFu && cpu.uav1[32] == 0u && cpu.uav1[64] == 0u,
          "OOB positive (+0x20000) and negative (-12) s_buffer_load safely zero-fill to 0u on CPU-ref");

    Rdna2LiftOptions opts{};
    opts.use_sm65 = true;
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    opts.compile_dxil_with_dxc = HasDxc();
    opts.resign_dxil = true;
    Rdna2LiftResult lift{};
    Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Safe OOB s_buffer_load lifts to HLSL/DXIL + SPIR-V");
    Check(lift.hlsl_source.find("GetDimensions") != std::string::npos,
          "OOB s_buffer_load HLSL emits dynamic GetDimensions bounds clamp");
    Check(SpirvContainsOpcode(lift.spirv_words, 68u /*OpArrayLength*/),
          "OOB s_buffer_load SPIR-V emits OpArrayLength bounds clamp");
    Check(ValidateSpirvStrict(lift.spirv_words, "safe_oob_s_buffer_load"),
          "Safe OOB s_buffer_load SPIR-V passes spirv-val --target-env vulkan1.2");
    VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "safe_oob_s_buffer_load");

    // Also verify non-zero sbase CB heap (space1 / DescriptorSet 1) clamps >= 65536u in HLSL & OpArrayLength in SPIR-V
    {
        Rdna2Assembler cb_a;
        cb_a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 4, 0x20000);
        cb_a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(4));
        cb_a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 0, false);
        cb_a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult cb_lift{};
        Check(LiftRdna2Xs2Shader(cb_a.BuildXs2(ShaderStage::Compute, 1, 1, 1), cb_lift, opts).ok() &&
                  cb_lift.hlsl_source.find("_cb_off >= 65536u") != std::string::npos &&
                  SpirvContainsOpcode(cb_lift.spirv_words, 68u /*OpArrayLength*/) &&
                  ValidateSpirvStrict(cb_lift.spirv_words, "safe_oob_cb_heap"),
              "CB heap (sbase!=0) OOB s_buffer_load clamps cleanly in HLSL (>=65536u) & SPIR-V (OpArrayLength)");
    }
}

// ===========================================================================
// Deliverable 5: Wave64 Compute Dual-Half/Native Execution & Folded EXECZ CFG Fix
// ===========================================================================
void TestDeliverable5Wave64AndFoldedExecz() {
    std::printf("\n=== Deliverable 5: Wave64 Compute & Folded EXECZ CFG Fix ===\n");

    // 5a. Wave64 Compute (64 threads): cross-half readlane + ballot + mbcnt + permlanex16
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);  // v1 = tid * 4
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);                 // v2 = uav0[tid]
        // Read lane 45 (upper half) and lane 3 (lower half) across all 64 lanes!
        a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(45));
        a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(3));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::Sgpr(4));
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 7, Rdna2Operand::Sgpr(5), 6);  // v7 = lane[45] + lane[3]
        // Cross-half ballot: lanes with tid >= 30 (spans both lo32 and hi32!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineInt(30));
        a.EmitVopc(Rdna2Opcode::V_CMP_GE_U32, Rdna2Operand::Vgpr(0), 8);
        // Active prefix count across 64 lanes via mbcnt_lo + mbcnt_hi
        a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 9, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
        a.EmitVop3(Rdna2Opcode::V_MBCNT_HI_U32_B32, 10, Rdna2Operand::VccHi(), Rdna2Operand::Vgpr(9));
        // Store v7 at uav1[tid] and v10 at uav1[64 + tid]
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 256);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64 /* wave_size == 64 */);
        Rdna2CpuExecState cpu{};
        cpu.uav0.resize(128u, 0u);
        cpu.uav1.resize(128u, 0u);
        for (uint32_t i = 0u; i < 64u; ++i) cpu.uav0[i] = 100u + i * 3u;
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "Wave64 64-lane cross-half compute shader executes on CPU-ref");

        const uint32_t expected_sum = cpu.uav0[45] + cpu.uav0[3];
        bool w64_ok = true;
        for (uint32_t i = 0u; i < 64u; ++i) {
            const uint32_t exp_pfx = (i <= 30u) ? 0u : (i - 30u);
            if (cpu.uav1[i] != expected_sum || cpu.uav1[64u + i] != exp_pfx) w64_ok = false;
        }
        Check(w64_ok, "Wave64 CPU-ref cross-half readlane (lane 45 + lane 3) and 64-lane mbcnt exact");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Wave64 compute shader lifts to HLSL/DXIL + SPIR-V");
        Check(lift.hlsl_source.find("WaveGetLaneCount() >= 64u") != std::string::npos &&
                  lift.hlsl_source.find("_w64_lanes") != std::string::npos,
              "Wave64 HLSL supports both native Wave64 fast path AND dual-half Wave32 LDS scratchpad");
        Check(ValidateSpirvStrict(lift.spirv_words, "wave64_cross_half"),
              "Wave64 cross-half SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "wave64_cross_half");
    }

    // 5b. Folded EXECZ scalar branch-to-join CFG fix (Leonida #4574)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);  // v1 = tid * 4
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(50));        // v4 = 50
        // Folded EXECZ region: set exec_lo = 0, then s_cbranch_execz join_lbl!
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /* exec_lo */, Rdna2Operand::InlineInt(0));
        const uint32_t br_execz = a.EmitSopp(Rdna2Opcode::S_CBRANCH_EXECZ, 0);
        // Dead region (must be folded/skipped without killing post-join code!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::LiteralU32(9999u));
        const uint32_t join_lbl = a.CurrentWordOffset();
        a.PatchBranch(br_execz, join_lbl);
        // Post-join active region: restore exec_lo and add 727 -> 50 + 727 = 777!
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /* exec_lo */, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(727u));
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::Vgpr(4), 5);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Xs2ShaderContainer parsed{};
        std::vector<Rdna2Instruction> dec;
        Check(ParseXs2Container(xs2, parsed).ok() && DisassembleRdna2(parsed.code_words, dec).ok(),
              "Folded EXECZ branch-to-join shader disassembles");
        Rdna2IrModule ir{};
        Check(BuildRdna2Ir(parsed, dec, ir).ok(), "Folded EXECZ shader lowers to IR");
        Rdna2IrOptStats opt_stats{};
        OptimizeRdna2Ir(ir, &opt_stats);
        Check(opt_stats.folded_execz_branches >= 1u,
              "OptimizeRdna2Ir folds constant EXECZ branch-to-join cleanly (Leonida #4574)");

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(32u, 0u);
        cpu.uav1.assign(32u, 0u);
        Check(ExecuteRdna2Cpu(parsed, dec, cpu).ok() && cpu.uav1[0] == 777u && cpu.uav1[31] == 777u,
              "Folded EXECZ skips dead region (9999) and executes post-join active code (777)");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Folded EXECZ shader lifts to HLSL/DXIL + SPIR-V");
        Check(ValidateSpirvStrict(lift.spirv_words, "folded_execz_join"),
              "Folded EXECZ SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "folded_execz_join");
    }
}

// ===========================================================================
// Deliverable 6: Hardware Float/Int Accuracy, Blackwell Select Guard & ALU/PS Ops
// ===========================================================================
void TestDeliverable6AccuracyAndBlackwellGuard() {
    std::printf("\n=== Deliverable 6: Float/Int Accuracy, Blackwell Guard & ALU/PS Ops ===\n");

    // 6a. v_fract_f32(±inf/NaN) -> NaN (NEVER 0 on NVIDIA!) + NaN-safe ordered/unordered V_CMP_*_F32
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);  // v2 = uav0[tid]
        // v_fract_f32 v3, v2
        a.EmitVop1(Rdna2Opcode::V_FRACT_F32, 3, Rdna2Operand::Vgpr(2));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);

        // Compare v2 against 1.0f using ordered LG (false on NaN), unordered NEQ (true on NaN), O (ordered), U (unordered)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::InlineFloat(1.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(1));
        a.EmitVopc(Rdna2Opcode::V_CMP_LG_F32, Rdna2Operand::Vgpr(2), 9);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 4, Rdna2Operand::InlineInt(0), 10);
        a.EmitVopc(Rdna2Opcode::V_CMP_NEQ_F32, Rdna2Operand::Vgpr(2), 9);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 5, Rdna2Operand::InlineInt(0), 10);
        a.EmitVopc(Rdna2Opcode::V_CMP_O_F32, Rdna2Operand::Vgpr(2), 9);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 6, Rdna2Operand::InlineInt(0), 10);
        a.EmitVopc(Rdna2Opcode::V_CMP_U_F32, Rdna2Operand::Vgpr(2), 9);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 7, Rdna2Operand::InlineInt(0), 10);

        // Pack 4 compare booleans into bitmask v8 = (LG) | (NEQ<<1) | (O<<2) | (U<<3)
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 5, Rdna2Operand::InlineInt(1), 5);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 6);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 7, Rdna2Operand::InlineInt(3), 7);
        a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(4), 5);
        a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 6);
        a.EmitVop2(Rdna2Opcode::V_OR_B32, 8, Rdna2Operand::Vgpr(8), 7);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 128);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(64u, FloatToBits(1.0f));
        cpu.uav0[0] = 0x7F800000u;  // +inf
        cpu.uav0[1] = 0xFF800000u;  // -inf
        cpu.uav0[2] = 0x7FC00000u;  // qNaN
        cpu.uav0[3] = FloatToBits(3.25f);
        cpu.uav0[4] = FloatToBits(-1.25f);
        cpu.uav0[5] = FloatToBits(1.0f);
        cpu.uav1.assign(64u, 0u);
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "v_fract_f32 + NaN-safe V_CMP_*_F32 executes on CPU-ref");
        Check(IsNanBitsF32(cpu.uav1[0]) && IsNanBitsF32(cpu.uav1[1]) && IsNanBitsF32(cpu.uav1[2]) &&
                  std::fabs(BitsToFloat(cpu.uav1[3]) - 0.25f) < 1e-6f &&
                  std::fabs(BitsToFloat(cpu.uav1[4]) - 0.75f) < 1e-6f,
              "v_fract_f32(+inf/-inf/NaN) -> qNaN, v_fract_f32(3.25)=0.25, v_fract_f32(-1.25)=0.75");
        // For NaN (lane 2): LG=0, NEQ=1, O=0, U=1 -> 0b1010 = 10
        // For 1.0f (lane 5): LG=0, NEQ=0, O=1, U=0 -> 0b0100 = 4
        // For 3.25f (lane 3): LG=1, NEQ=1, O=1, U=0 -> 0b0111 = 7
        Check(cpu.uav1[32u + 2u] == 10u && cpu.uav1[32u + 5u] == 4u && cpu.uav1[32u + 3u] == 7u,
              "NaN-safe ordered/unordered V_CMP_LG/NEQ/O/U_F32 exact bitmasks on CPU-ref");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "v_fract_f32 + V_CMP_*_F32 lifts to HLSL/DXIL + SPIR-V");
        Check(lift.hlsl_source.find("_os_fract_f32") != std::string::npos &&
                  lift.hlsl_source.find("_os_cmp_f32_bits") != std::string::npos,
              "HLSL uses _os_fract_f32 (NaN on ±inf) and bit-exact _os_cmp_f32_bits");
        Check(ValidateSpirvStrict(lift.spirv_words, "fract_inf_nan_cmp"),
              "v_fract_f32 + V_CMP_*_F32 SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "fract_inf_nan_cmp", true /* NaN match */);
    }

    // 6b. f64 accuracy past f32/i64 ranges + v_fract_f32/f16/f64 + SDWA integer ALU + v_div_fmas_f32 (vcc=0 & vcc=1)
    {
        using namespace openscarlett::rdna2_sv;
        std::vector<uint32_t> code;
        // v_div_fixup_f64 v[10:11], v[2:3], v[4:5], v[6:7]  (1e200 quotient, -2.0 denom, +4.0 num -> -1e200, overflows f32!)
        EncodeScalarVectorInst("v_div_fixup_f64", 10u,
                               {Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(6)}, 0, code);
        // v_cvt_i32_f64 v12, v[2:3] (1e200 -> saturates to INT32_MAX 0x7FFFFFFF)
        EncodeScalarVectorInst("v_cvt_i32_f64", 12u, {Rdna2Operand::Vgpr(2)}, 0, code);
        // v_cvt_u32_f64 v13, v[2:3] (1e200 -> saturates to UINT32_MAX 0xFFFFFFFF)
        EncodeScalarVectorInst("v_cvt_u32_f64", 13u, {Rdna2Operand::Vgpr(2)}, 0, code);
        // v_fract_f64 v[14:15], v[8:9] (+inf -> qNaN 0x7FF8000000000000)
        EncodeScalarVectorInst("v_fract_f64", 14u, {Rdna2Operand::Vgpr(8)}, 0, code);
        // v_fract_f16 v16, v17 (+inf f16 0x7C00 -> qNaN f16 0x7E00)
        EncodeScalarVectorInst("v_fract_f16", 16u, {Rdna2Operand::Vgpr(17)}, 0, code);
        // v_div_fmas_f32 v18, v19, v20, v21 (with vcc=1 and exp(v21) > 127 -> 2^64 scaling!)
        EncodeScalarVectorInst("v_div_fmas_f32", 18u,
                               {Rdna2Operand::Vgpr(19), Rdna2Operand::Vgpr(20), Rdna2Operand::Vgpr(21)}, 0, code);
        // SDWA integer ALU: v_add_nc_u32_sdwa v22, v23, v24 dst_sel:WORD_1 dst_unused:UNUSED_PRESERVE src0_sel:BYTE_1 src0_sext src1_sel:BYTE_0
        DecodedSvModifier sdwa = DecodedSvModifier::MakeSdwa(23u, kSdwaSelByte1, kSdwaSelByte0, kSdwaSelWord1, kSdwaUnusedPreserve);
        sdwa.src0_sext = true;
        EncodeScalarVectorInst("v_add_nc_u32", 22u, {Rdna2Operand::Vgpr(23), Rdna2Operand::Vgpr(24)}, 0, code, &sdwa);
        EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code);

        std::vector<SvDecodedInst> dec;
        Check(DecodeScalarVectorStream(code, dec).ok(), "f64 + v_div_fmas_f32 + SDWA integer ALU decodes");
        SvWaveState st{};
        st.Reset(32u);
        st.exec = 1ull;
        st.vcc = 1ull;  // lane 0 has vcc=1 for v_div_fmas_f32 scaling!
        const uint64_t big_d = DoubleToBits(1.0e200);
        const uint64_t neg2_d = DoubleToBits(-2.0);
        const uint64_t pos4_d = DoubleToBits(4.0);
        const uint64_t inf_d = DoubleToBits(std::numeric_limits<double>::infinity());
        st.vgpr[0][2] = static_cast<uint32_t>(big_d);
        st.vgpr[0][3] = static_cast<uint32_t>(big_d >> 32u);
        st.vgpr[0][4] = static_cast<uint32_t>(neg2_d);
        st.vgpr[0][5] = static_cast<uint32_t>(neg2_d >> 32u);
        st.vgpr[0][6] = static_cast<uint32_t>(pos4_d);
        st.vgpr[0][7] = static_cast<uint32_t>(pos4_d >> 32u);
        st.vgpr[0][8] = static_cast<uint32_t>(inf_d);
        st.vgpr[0][9] = static_cast<uint32_t>(inf_d >> 32u);
        st.vgpr[0][17] = 0x7C00u;  // +inf f16
        st.vgpr[0][19] = FloatToBits(2.0f);
        st.vgpr[0][20] = FloatToBits(3.0f);
        st.vgpr[0][21] = FloatToBits(4.0f);  // exp = 129 > 127 -> scale by +2^64 -> (2*3+4)*2^64 = 10 * 2^64
        st.vgpr[0][22] = 0x1234ABCDu;
        st.vgpr[0][23] = 0x0000FE00u;  // byte1 = 0xFE -> sext = -2 (0xFFFFFFFE)
        st.vgpr[0][24] = 0x00000010u;  // byte0 = 16 -> -2 + 16 = 14 (0x000E) -> WORD_1 preserving lo = 0x000EABCD
        Check(ExecuteScalarVectorCpu(dec, st).ok(), "f64 + v_div_fmas_f32 + SDWA integer ALU CPU-ref executes");

        const uint64_t out_div64 = static_cast<uint64_t>(st.vgpr[0][10]) | (static_cast<uint64_t>(st.vgpr[0][11]) << 32u);
        const uint64_t out_fr64 = static_cast<uint64_t>(st.vgpr[0][14]) | (static_cast<uint64_t>(st.vgpr[0][15]) << 32u);
        Check(BitsToDouble(out_div64) == -1.0e200,
              "v_div_fixup_f64 preserves double precision past f32 range (-1.0e200)");
        Check(st.vgpr[0][12] == 0x7FFFFFFFu && st.vgpr[0][13] == 0xFFFFFFFFu,
              "v_cvt_i32_f64 & v_cvt_u32_f64 saturate cleanly past i32/u32 ranges");
        Check(out_fr64 == 0x7FF8000000000000ull && st.vgpr[0][16] == 0x7E00u,
              "v_fract_f64(+inf) -> qNaN64 and v_fract_f16(+inf) -> qNaN16");
        const float expected_fmas = std::ldexp(10.0f, 64);
        Check(std::fabs(BitsToFloat(st.vgpr[0][18]) - expected_fmas) <= expected_fmas * 1e-6f,
              "v_div_fmas_f32 applies exact VCC 2^+64 scaling when vcc_bit==1");
        Check(st.vgpr[0][22] == 0x000EABCDu,
              "SDWA integer ALU (BYTE_1 sext + BYTE_0 -> WORD_1 PRESERVE) exact bit result");
    }

    // 6c. Blackwell select-after-shuffle guard (Leonida #4233) + v_alignbit_b32, v_alignbyte_b32, s_absdiff_i32, v_fma_f16, v_div_fmas_f32, v_fma_mix_*
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);  // v2 = uav0[tid]
        // Shuffle followed immediately by v_cndmask_b32 (Blackwell select-after-shuffle guard!)
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 3, 2, 0, 0, 0x841Fu);  // v3 = lane^1
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(16));
        a.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 12);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 4, Rdna2Operand::Vgpr(2), 3);  // lane<16 ? swizzled : self
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);

        // v_alignbit_b32 (shift=0 UB-safe AND shift=8) + v_alignbyte_b32 (byte=1)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(0x11223344u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralU32(0xAABBCCDDu));
        a.EmitVop3(Rdna2Opcode::V_ALIGNBIT_B32, 7, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::InlineInt(0));
        a.EmitVop3(Rdna2Opcode::V_ALIGNBIT_B32, 8, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::InlineInt(8));
        a.EmitVop3(Rdna2Opcode::V_ALIGNBYTE_B32, 9, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::InlineInt(2));
        // s_absdiff_i32 s6, -15, 25 -> 40
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::InlineInt(-15));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 5, Rdna2Operand::InlineInt(25));
        a.EmitSop2(Rdna2Opcode::S_ABSDIFF_I32, 6, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::Sgpr(6));
        // v_fma_f16: 2.0 (0x4000) * 3.0 (0x4200) + 1.5 (0x3E00) = 7.5 (0x4780)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(0x00004000u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralU32(0x00004200u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralU32(0x00003E00u));
        a.EmitVop3(Rdna2Opcode::V_FMA_F16, 14, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12), Rdna2Operand::Vgpr(13));
        // v_fma_mix_f32: src0(f16 lo=2.0) * src1(f16 lo=3.0) + src2(f32=4.5) = 10.5f
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::LiteralF32(4.5f));
        a.EmitVop3p(Rdna2Opcode::V_FMA_MIX_F32, 16, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12),
                    Rdna2Operand::Vgpr(15), 0u /*op_sel*/, 0x3u /*src0&src1 are f16, src2 is f32*/);

        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 128);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 384);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 512);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 1, 4, 640);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 1, 4, 768);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 24, 32);
        Rdna2CpuExecState cpu{};
        cpu.uav0.resize(224u, 0u);
        cpu.uav1.resize(224u, 0u);
        for (uint32_t i = 0u; i < 32u; ++i) cpu.uav0[i] = 500u + i;
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok(), "Blackwell select-after-shuffle + ALU ops execute on CPU-ref");
        Check(cpu.uav1[0] == cpu.uav0[1] && cpu.uav1[16] == cpu.uav0[16] &&
                  cpu.uav1[32] == 0xAABBCCDDu && cpu.uav1[64] == 0x44AABBCCu &&
                  cpu.uav1[96] == 0x3344AABBu && cpu.uav1[128] == 40u &&
                  cpu.uav1[160] == 0x4780u && cpu.uav1[192] == FloatToBits(10.5f),
              "v_alignbit(sh=0/8), v_alignbyte, s_absdiff_i32(-15,25)=40, v_fma_f16=7.5, v_fma_mix_f32=10.5 exact");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "Blackwell guard + ALU ops lift to HLSL/DXIL + SPIR-V");
        Check(lift.hlsl_source.find("_bw_t =") != std::string::npos &&
                  lift.hlsl_source.find("_bw_f =") != std::string::npos,
              "HLSL materializes Blackwell select-after-shuffle distinct temporaries (_bw_t / _bw_f)");
        Check(ValidateSpirvStrict(lift.spirv_words, "blackwell_guard_alu"),
              "Blackwell guard + ALU ops SPIR-V passes spirv-val --target-env vulkan1.2");
        VerifyLiveComputeParity(lift, cpu.uav0, cpu.uav1, {}, "blackwell_guard_alu", false, true, true);
    }

    // 6d. Pixel-stage s_brev_b32 + v_readfirstlane_b32 + v_swap_b32 + SOPK s_waitcnt_vscnt null, 0
    {
        Rdna2Assembler a;
        a.EmitSopk(Rdna2Opcode::S_WAITCNT_VSCNT, 125u /* null */, 0);           // SOPK s_waitcnt_vscnt null, 0
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 8, Rdna2Operand::Vgpr(0)); // s8 = firstlane(v0) (dynamic!)
        a.EmitSop1(Rdna2Opcode::S_BREV_B32, 7, Rdna2Operand::Sgpr(8));          // s7 = bitreverse(s8) (dynamic!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(0.25f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::InlineFloat(0.75f));
        a.EmitVop1(Rdna2Opcode::V_SWAP_B32, 4, Rdna2Operand::Vgpr(5));          // v4=0.75f, v5=0.25f
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 6, Rdna2Operand::Sgpr(7));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(1.0f));
        a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 6, 7, 0xFu, true);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Pixel, 1, 1, 1, 16, 16, 32);
        Rdna2CpuExecState cpu{};
        cpu.stage_inputs = {{BitsToFloat(0x12345678u), 0.5f, 0.0f, 1.0f}};
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok() && !cpu.exports.empty() && cpu.exports[0].wrote_mrt0,
              "Pixel-stage s_brev_b32 / v_readfirstlane_b32 / v_swap_b32 / s_waitcnt_vscnt executes on CPU-ref");
        Check(std::fabs(cpu.exports[0].mrt0[0] - 0.75f) < 1e-6f &&
                  std::fabs(cpu.exports[0].mrt0[1] - 0.25f) < 1e-6f &&
                  std::fabs(cpu.exports[0].mrt0[2] - static_cast<float>(0x1E6A2C48u)) < 128.0f,
              "Pixel-stage v_swap_b32 swapped (0.25,0.75)->(0.75,0.25) & s_brev_b32(0x12345678)==0x1E6A2C48");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(),
              "Pixel-stage s_brev_b32 / v_readfirstlane_b32 / v_swap_b32 / s_waitcnt_vscnt lifts (+ DXC ps_6_5)");
        Check(lift.hlsl_source.find("reversebits(") != std::string::npos &&
                  lift.hlsl_source.find("WaveReadLaneFirst(") != std::string::npos,
              "Pixel-stage HLSL emits reversebits() and WaveReadLaneFirst()");
        Check(SpirvContainsOpcode(lift.spirv_words, 204u /*OpBitReverse*/) &&
                  SpirvContainsOpcode(lift.spirv_words, 338u /*OpGroupNonUniformBroadcastFirst*/),
              "Pixel-stage SPIR-V emits OpBitReverse and OpGroupNonUniformBroadcastFirst");
        Check(ValidateSpirvStrict(lift.spirv_words, "pixel_brev_readfirst_swap_waitcnt"),
              "Pixel-stage SPIR-V passes spirv-val --target-env vulkan1.2");
    }
}

// ===========================================================================
// Deliverable 7: image_sample_c_lz, SPIR-V 1.2 Core Descriptor Indexing,
//                MTBUF Format Table, Truncated DXBC ISGN/ISG1 & DXIL Cache Diagnostics
// ===========================================================================
void TestDeliverable7ImageSampleDescIndexingContainerAndCache() {
    std::printf("\n=== Deliverable 7: image_sample_c_lz, SPIR-V 1.2 Core Indexing, MTBUF, DXBC Sig & Cache ===\n");

    // 7a. image_sample_c_lz -> CPU-ref + HLSL SampleCmpLevelZero + SPIR-V OpImageSampleDrefExplicitLod
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(0.5f));  // dref = 0.5f
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.25f)); // u = 0.25f
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(0.25f)); // v = 0.25f
        a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE_C_LZ, 8, 2, 0, 0, 0x1u, 1u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 0, 4, 0, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        const auto xs2 = a.BuildXs2(ShaderStage::Compute, 1, 1, 1, 16, 16, 32);
        Rdna2CpuExecState cpu{};
        cpu.tex2d_srv.AllocateRgba32f(1u, 4u, 4u, 1u, 1u);
        cpu.tex2d_srv.StoreRgba32f(1u, 1u, 0u, {0.75f, 0.0f, 0.0f, 1.0f});  // depth 0.75 >= dref 0.5 -> 1.0f
        cpu.uav1.assign(4u, 0u);
        Check(ExecuteRdna2BlobCpu(xs2, cpu).ok() && cpu.uav1[0] == FloatToBits(1.0f),
              "image_sample_c_lz CPU-ref returns 1.0f when dref (0.5) <= depth (0.75)");

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = HasDxc();
        opts.resign_dxil = true;
        Rdna2LiftResult lift{};
        Check(LiftRdna2Xs2Shader(xs2, lift, opts).ok(), "image_sample_c_lz lifts to HLSL/DXIL + SPIR-V");
        Check(lift.hlsl_source.find("SampleCmpLevelZero(_samp_cmp0") != std::string::npos,
              "image_sample_c_lz HLSL emits SampleCmpLevelZero");
        Check(SpirvContainsOpcode(lift.spirv_words, 90u /*OpImageSampleDrefExplicitLod*/),
              "image_sample_c_lz SPIR-V emits OpImageSampleDrefExplicitLod");
        Check(ValidateSpirvStrict(lift.spirv_words, "image_sample_c_lz"),
              "image_sample_c_lz SPIR-V passes spirv-val --target-env vulkan1.2");
    }

    // 7b. Vulkan 1.2 Core Dynamic Sampled Descriptor Indexing (ZERO OpExtension, NonUniform decoration, spirv-val --target-env vulkan1.2!)
    {
        Rdna2IrModule ir{};
        ir.stage = ShaderStage::Compute;
        ir.threadgroup_x = 32u;
        ir.wave_size = 32u;
        ir.uses_uav0 = true;
        ir.uses_uav1 = true;
        ir.uses_texture_srv = true;
        ir.uses_texture_sample = true;
        ir.uses_dynamic_sampled_indexing = true;
        ir.sampled_image_array_size = 8u;

        IrBasicBlock bb{};
        bb.id = 0u;
        auto add = [&](IrOp op, IrType ty, std::vector<IrValueId> ops, uint32_t imm = 0u, float fimm = 0.0f) {
            IrInstruction ins{};
            ins.id = (ty == IrType::Void) ? kInvalidIrValue : ir.AllocateValueId();
            ins.op = op;
            ins.type = ty;
            ins.operands = std::move(ops);
            ins.imm_u32 = imm;
            ins.imm_f32 = fimm;
            bb.instructions.push_back(ins);
            return ins.id;
        };
        const IrValueId tid = add(IrOp::ThreadIdX, IrType::U32, {});
        const IrValueId c2 = add(IrOp::ConstU32, IrType::U32, {}, 2u);
        const IrValueId off = add(IrOp::Shl, IrType::U32, {tid, c2});
        const IrValueId desc_idx = add(IrOp::LoadUav, IrType::U32, {off}, 0u);  // dynamically varying per thread!
        const IrValueId u = add(IrOp::ConstF32, IrType::F32, {}, 0u, 0.5f);
        const IrValueId v = add(IrOp::ConstF32, IrType::F32, {}, 0u, 0.5f);
        const IrValueId lod = add(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
        const IrValueId tex_r = add(IrOp::ImageSampleDynamic, IrType::F32, {u, v, lod, desc_idx}, (8u << 8u) | 0u);
        add(IrOp::StoreUav, IrType::Void, {off, tex_r}, 1u);
        add(IrOp::Return, IrType::Void, {});
        ir.blocks.push_back(bb);

        std::vector<uint32_t> spv;
        Check(EmitSpirv(ir, spv).ok(), "Vulkan 1.2 core dynamic sampled descriptor indexing emits SPIR-V 1.5");
        Check(spv.size() >= 5u && spv[1] == 0x00010500u,
              "Dynamic sampled indexing emits SPIR-V 1.5 (Vulkan 1.2 core)");
        Check(!SpirvContainsOpcode(spv, 10u /*OpExtension*/),
              "Dynamic sampled indexing uses ZERO vendor/EXT OpExtension (100% Vulkan 1.2 core)");
        Check(SpirvContainsCapability(spv, 22u /*ShaderSampledImageArrayDynamicIndexing*/) &&
                  SpirvContainsCapability(spv, 5301u /*ShaderNonUniform*/) &&
                  SpirvContainsCapability(spv, 5304u /*SampledImageArrayNonUniformIndexing*/) &&
                  SpirvContainsDecoration(spv, 5300u /*NonUniform*/),
              "Dynamic sampled indexing declares Vulkan 1.2 core NonUniform capabilities & decorations");
        Check(ValidateSpirvStrict(spv, "vk12_core_dynamic_sampled_indexing", "vulkan1.2"),
              "Vulkan 1.2 core dynamic sampled indexing passes spirv-val --target-env vulkan1.2");

        std::string hlsl, target;
        Check(EmitHlsl(ir, hlsl, target, true).ok() &&
                  hlsl.find("NonUniformResourceIndex(") != std::string::npos,
              "Dynamic sampled indexing HLSL emits NonUniformResourceIndex()");
        if (HasDxc()) {
            std::vector<uint8_t> dxil;
            std::string diag;
            Check(CompileHlslWithDxc(hlsl, target, "main", dxil, diag).ok(),
                  "Dynamic sampled indexing HLSL compiles cleanly with DXC");
        }
    }

    // 7c. Single authoritative typed-buffer format byte-size table (Leonida aa2988c)
    {
        const Rdna2MtbufFormat fmts[] = {
            Rdna2MtbufFormat::R8_UNORM, Rdna2MtbufFormat::R16_FLOAT, Rdna2MtbufFormat::R8G8_UINT,
            Rdna2MtbufFormat::R32_FLOAT, Rdna2MtbufFormat::R16G16_SNORM, Rdna2MtbufFormat::R8G8B8A8_UNORM,
            Rdna2MtbufFormat::R32G32_FLOAT, Rdna2MtbufFormat::R16G16B16A16_FLOAT,
            Rdna2MtbufFormat::R32G32B32_FLOAT, Rdna2MtbufFormat::R32G32B32A32_FLOAT
        };
        bool table_consistent = true;
        for (Rdna2MtbufFormat f : fmts) {
            const auto info = GetRdna2MtbufFormatInfo(f);
            if (info.component_bytes != rdna2_mem::MtbufFormatComponentBytes(f) ||
                info.byte_stride != rdna2_mem::MtbufFormatByteStride(f) ||
                info.byte_stride != info.component_bytes * info.component_count) {
                table_consistent = false;
            }
        }
        Check(table_consistent,
              "Authoritative GetRdna2MtbufFormatInfo table matches rdna2_mem & component_bytes*count across all formats");
    }

    // 7d. Reject incomplete/truncated DXBC ISGN/ISG1 input signatures cleanly (Leonida #4572)
    {
        Rdna2Assembler a;
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        XboxFullDxbcBuildDesc bd{};
        bd.stage = ShaderStage::Pixel;
        bd.xs2_blob = a.BuildXs2(ShaderStage::Pixel);
        XboxSignatureParamDesc p0{};
        p0.semantic_name = "TEXCOORD";
        p0.semantic_index = 0u;
        p0.register_index = 0u;
        bd.input_signature.push_back(p0);

        for (bool use_v1 : {true, false}) {
            bd.use_sig_v1 = use_v1;
            const std::vector<uint8_t> valid_dxbc = BuildFullXboxDxbcContainer(bd);
            XboxParsedShaderContainer ok_full{};
            XboxDxbcContainer ok_base{};
            Check(ParseFullXboxShaderContainer(valid_dxbc, ok_full).ok() &&
                      ParseXboxShaderContainer(valid_dxbc, ok_base).ok(),
                  use_v1 ? "Valid ISG1 Xbox DXBC parses cleanly" : "Valid ISGN Xbox DXBC parses cleanly");

            // Corrupt the ISG1/ISGN chunk by truncating its chunk_size to 12 bytes (< header + 1 entry)
            std::vector<uint8_t> trunc_dxbc = valid_dxbc;
            const char* tag = use_v1 ? "ISG1" : "ISGN";
            bool patched = false;
            for (size_t i = 32u; i + 12u < trunc_dxbc.size(); ++i) {
                if (std::memcmp(&trunc_dxbc[i], tag, 4u) == 0) {
                    const uint32_t bad_sz = 12u;  // param_count=1, param_offset=8 -> needs >= 32/40 bytes!
                    std::memcpy(&trunc_dxbc[i + 4u], &bad_sz, 4u);
                    patched = true;
                    break;
                }
            }
            Check(patched, use_v1 ? "Patched truncated ISG1 chunk size" : "Patched truncated ISGN chunk size");

            XboxParsedShaderContainer bad_full{};
            XboxDxbcContainer bad_base{};
            const Status full_st = ParseFullXboxShaderContainer(trunc_dxbc, bad_full);
            const Status base_st = ParseXboxShaderContainer(trunc_dxbc, bad_base);
            Check(!full_st.ok() && full_st.code == StatusCode::ParseError &&
                      !base_st.ok() && base_st.code == StatusCode::ParseError,
                  use_v1 ? "Truncated ISG1 rejected cleanly with ParseError (Leonida #4572)"
                         : "Truncated ISGN rejected cleanly with ParseError (Leonida #4572)");
        }
    }

    // 7e. DXIL cache hit/miss/compile failure counters & diagnostic strings (xcl a3feb68)
    {
        const auto cache_dir = std::filesystem::temp_directory_path() / "openscarlett_s15_cache_diag_test";
        std::error_code ec;
        std::filesystem::remove_all(cache_dir, ec);

        ShaderCache cache;
        cache.SetCacheDirectory(cache_dir.string());
        cache.EnableAsyncPsoCompilation(false);
        cache.ResetStats();

        // 1. Trigger a clean sync compile failure on invalid blob -> increments failure counters + populates diagnostics
        const std::vector<uint8_t> bad_blob = {'B', 'A', 'D', '!', 1, 2, 3, 4};
        ShaderCacheLiftRequest req{};
        req.highest_shader_model = 0x65u;
        CachedShaderResult fail_res{};
        std::string fail_diag;
        const Status fail_st = cache.GetOrLiftXboxShader(bad_blob.data(), bad_blob.size(), req, fail_res, &fail_diag);
        Check(!fail_st.ok() && !fail_diag.empty() && !cache.LastCompileError().empty(),
              "ShaderCache exposes diagnostic string on compile failure (xcl a3feb68)");

        // 2. Miss -> real GetOrLiftXboxShader (or Put valid 64B DXBC) -> Memory Hit -> ClearMemory -> Disk Hit
        Rdna2Assembler ca;
        ca.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineInt(7));
        ca.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 0, false);
        ca.EmitSopp(Rdna2Opcode::S_ENDPGM);
        const std::vector<uint8_t> good_xs2 = ca.BuildXs2(ShaderStage::Compute, 1, 1, 1);

        CachedShaderResult r{};
        std::string good_diag;
        if (HasDxc()) {
            // Miss -> Sync compile + store
            Check(cache.GetOrLiftXboxShader(good_xs2.data(), good_xs2.size(), req, r, &good_diag).ok() &&
                      !r.dxil.empty() && !r.from_memory_cache && !r.from_disk_cache,
                  "ShaderCache GetOrLiftXboxShader records miss and compiles+stores DXIL");
            // Memory hit
            r = {};
            Check(cache.GetOrLiftXboxShader(good_xs2.data(), good_xs2.size(), req, r, &good_diag).ok() &&
                      r.from_memory_cache && !r.diagnostics.empty(),
                  "ShaderCache returns memory hit with diagnostic string");
            // Clear memory -> Disk hit
            cache.ClearMemoryCache();
            r = {};
            Check(cache.GetOrLiftXboxShader(good_xs2.data(), good_xs2.size(), req, r, &good_diag).ok() &&
                      r.from_disk_cache && !r.diagnostics.empty(),
                  "ShaderCache returns disk hit after ClearMemoryCache");
        } else {
            const ShaderCacheKey key = ComputeShaderCacheKey(good_xs2.data(), good_xs2.size(), "cs_6_5", 0x65u);
            std::vector<uint8_t> fake_dxil(64u, 0u);
            fake_dxil[0] = 'D'; fake_dxil[1] = 'X'; fake_dxil[2] = 'B'; fake_dxil[3] = 'C';
            Check(cache.Put(key, ShaderStage::Compute, "cs_6_5", fake_dxil).ok(), "ShaderCache Put stores entry");
            Check(cache.TryGet(key, r) && r.from_memory_cache && r.dxil == fake_dxil,
                  "ShaderCache TryGet returns memory hit");
            cache.ClearMemoryCache();
            r = {};
            Check(cache.TryGet(key, r) && r.from_disk_cache && r.dxil == fake_dxil,
                  "ShaderCache TryGet returns disk hit after ClearMemoryCache");
        }

        const ShaderCacheStats st = cache.Stats();
        const std::string summary = cache.FormatDiagnosticsSummary();
        std::printf("[INFO] %s\n", summary.c_str());
        Check(st.memory_hits >= 1u && st.disk_hits >= 1u &&
                  (!HasDxc() || (st.cache_misses >= 1u && st.sync_compiles_succeeded >= 1u)) &&
                  st.sync_compiles_failed >= 1u && st.total_compile_failures >= 1u,
              "ShaderCacheStats tracks memory_hits, disk_hits, cache_misses & compile failure counters");
        Check(summary.find("mem=") != std::string::npos &&
                  summary.find("disk=") != std::string::npos &&
                  summary.find("misses=") != std::string::npos &&
                  summary.find("total_fail=") != std::string::npos &&
                  summary.find("last_error=") != std::string::npos,
              "ShaderCache::FormatDiagnosticsSummary includes all counters and last_error");

        std::filesystem::remove_all(cache_dir, ec);
    }
}

}  // namespace

int main() {
    TestDeliverable1DsSwizzleAllModes();
    TestDeliverable2PcRelativeBranchLowering();
    TestDeliverable3SmemFlatOffsetAndLdsD16();
    TestDeliverable4SafeOobSBufferLoad();
    TestDeliverable5Wave64AndFoldedExecz();
    TestDeliverable6AccuracyAndBlackwellGuard();
    TestDeliverable7ImageSampleDescIndexingContainerAndCache();

    if (g_failures == 0) {
        std::printf("\nALL SPRINT 15 WORKER 2 RDNA2 SHADER & ISA PEER PARITY TESTS PASSED!\n");
        return 0;
    }
    std::fprintf(stderr, "\nSPRINT 15 WORKER 2 TEST FAILURES: %d\n", g_failures);
    return 1;
}
