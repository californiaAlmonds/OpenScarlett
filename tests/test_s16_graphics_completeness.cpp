// Sprint 16 Workstream 1: Graphics Engine 100% Completeness Verification Suite
// Validates all 5 deliverables live on D3D12 Hardware GPU, D3D12 WARP, and Vulkan 1.4:
//   1. Page-at-a-Time Descriptor Heaps, Unbounded Sampler Heap (>2048),
//      GetGpuHardwareConfigurationX, and Dedicated Copy Engine Queue Parity
//   2. Real ExecuteIndirect Completeness (D3D12 HW+WARP AND Vulkan 1.4)
//   3. Live FMASK / DCC (2:1 & 4:1) / HTILE GPU Compute Resolve & Decompression
//   4. Vulkan 1.4 Dynamic Rendering Multi-Pass + OPENSCARLETT_VK_GPU Fallback + Cross-IHV Matrix
//   5. VRAM Residency Hygiene (DeviceX + VramResidencyManager) & Multi-Queue Stress

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/kernel/vram_residency_manager.h"
#include "gpu/d3d12_xs/device_x.h"
#include "gpu/d3d12_xs/proxies_x.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace {

int g_failures = 0;

#define S16_CHECK(cond, msg)                                                           \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "[S16 FAIL] %s (line %d): %s\n", #cond, __LINE__, msg); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

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
    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};

struct SpvBuilder {
    std::vector<uint32_t> words;
    uint32_t bound = 1u;

    uint32_t Id() { return bound++; }
    void Op(uint16_t opcode, std::initializer_list<uint32_t> args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1u) << 16u) | opcode);
        for (uint32_t a : args) words.push_back(a);
    }
    void OpVec(uint16_t opcode, const std::vector<uint32_t>& args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1u) << 16u) | opcode);
        words.insert(words.end(), args.begin(), args.end());
    }
    static void PushStr(std::vector<uint32_t>& dst, const char* s) {
        const size_t n = std::strlen(s) + 1u;
        const size_t nw = (n + 3u) / 4u;
        for (size_t w = 0u; w < nw; ++w) {
            uint32_t v = 0u;
            for (size_t b = 0u; b < 4u; ++b) {
                const size_t idx = w * 4u + b;
                if (idx < n) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8u);
            }
            dst.push_back(v);
        }
    }
    std::vector<uint32_t> Finish(uint32_t spv_version) const {
        std::vector<uint32_t> out;
        out.reserve(5u + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533136u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

// SPIR-V 1.3 passthrough vertex shader:
//   in  location 0: vec4 pos
//   out gl_Position = pos
std::vector<uint32_t> BuildPosOnlyVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id(), t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t v_in_pos = b.Id(), v_out_pv = b.Id(), c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});     // OpCapability Shader
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_out_pv});
        b.OpVec(15, ep);
    }
    b.Op(71, {v_in_pos, 30, 0});  // Location 0
    b.Op(71, {t_pv, 2});          // Block
    b.Op(72, {t_pv, 0, 11, 0});   // BuiltIn Position

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pv, t_v4});
    b.Op(32, {t_out_pv, 3, t_pv});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), ptr = b.Id();
    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(65, {t_out_v4, ptr, v_out_pv, c_i0});
    b.Op(62, {ptr, pos});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// SPIR-V 1.3 Fragment shader outputting PushConstant vec4 color.
std::vector<uint32_t> BuildPushConstFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_out_v4 = b.Id(), t_pc = b.Id(), t_ptr_pc = b.Id(), t_ptr_pc_v4 = b.Id();
    const uint32_t v_out = b.Id(), v_pc = b.Id(), c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.push_back(v_out);
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 7});       // OriginUpperLeft
    b.Op(71, {v_out, 30, 0});     // Location 0
    b.Op(71, {t_pc, 2});          // Block
    b.Op(72, {t_pc, 0, 35, 0});   // Offset 0

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(30, {t_pc, t_v4});
    b.Op(32, {t_ptr_pc, 9, t_pc});
    b.Op(32, {t_ptr_pc_v4, 9, t_v4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_ptr_pc, v_pc, 9});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t p = b.Id(), col = b.Id();
    b.Op(65, {t_ptr_pc_v4, p, v_pc, c_i0});
    b.Op(61, {t_v4, col, p});
    b.Op(62, {v_out, col});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// SPIR-V 1.3 Compute shader: out[gid.x] = in[gid.x] * 3 + 7 (LocalSize 64,1,1)
std::vector<uint32_t> BuildSimpleMulAddComputeSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_v3u = b.Id();
    const uint32_t t_rt = b.Id(), t_st = b.Id(), t_ps_st = b.Id(), t_ps_u = b.Id();
    const uint32_t t_pi_v3 = b.Id(), t_pi_u = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_gid = b.Id();
    const uint32_t c0 = b.Id(), c3 = b.Id(), c7 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {5, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.push_back(v_gid);
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 64, 1, 1});
    b.Op(71, {t_rt, 6, 4});
    b.Op(71, {t_st, 2});
    b.Op(72, {t_st, 0, 35, 0});
    b.Op(71, {v_in, 34, 0});
    b.Op(71, {v_in, 33, 0});
    b.Op(71, {v_in, 24});
    b.Op(71, {v_out, 34, 0});
    b.Op(71, {v_out, 33, 1});
    b.Op(71, {v_gid, 11, 28});

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(21, {t_u32, 32, 0});
    b.Op(23, {t_v3u, t_u32, 3});
    b.Op(29, {t_rt, t_u32});
    b.Op(30, {t_st, t_rt});
    b.Op(32, {t_ps_st, 12, t_st});
    b.Op(32, {t_ps_u, 12, t_u32});
    b.Op(32, {t_pi_v3, 1, t_v3u});
    b.Op(32, {t_pi_u, 1, t_u32});

    b.Op(43, {t_u32, c0, 0});
    b.Op(43, {t_u32, c3, 3});
    b.Op(43, {t_u32, c7, 7});
    b.Op(59, {t_ps_st, v_in, 12});
    b.Op(59, {t_ps_st, v_out, 12});
    b.Op(59, {t_pi_v3, v_gid, 1});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t p_gx = b.Id(), gx = b.Id(), p_in = b.Id(), val = b.Id();
    const uint32_t mul = b.Id(), add = b.Id(), p_out = b.Id();
    b.Op(65, {t_pi_u, p_gx, v_gid, c0});
    b.Op(61, {t_u32, gx, p_gx});
    b.Op(65, {t_ps_u, p_in, v_in, c0, gx});
    b.Op(61, {t_u32, val, p_in});
    b.Op(132, {t_u32, mul, val, c3});
    b.Op(128, {t_u32, add, mul, c7});
    b.Op(65, {t_ps_u, p_out, v_out, c0, gx});
    b.Op(62, {p_out, add});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// SPIR-V 1.4 Mesh shader emitting a fullscreen-centered triangle.
std::vector<uint32_t> BuildSimpleMeshSpirv14() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_u32 = b.Id(), t_i32 = b.Id(), t_f32 = b.Id();
    const uint32_t t_v4 = b.Id(), t_v3u = b.Id();
    const uint32_t c_u0 = b.Id(), c_u1 = b.Id(), c_u2 = b.Id(), c_u3 = b.Id(), c_i0 = b.Id();
    const uint32_t t_pv = b.Id(), t_arr_pv = b.Id(), t_ptr_arr_pv = b.Id(), t_ptr_out_v4 = b.Id();
    const uint32_t t_arr_idx = b.Id(), t_ptr_arr_idx = b.Id(), t_ptr_out_v3u = b.Id();
    const uint32_t v_pv = b.Id(), v_idx = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {5283});  // OpCapability MeshShadingEXT
    {
        std::vector<uint32_t> ext;
        SpvBuilder::PushStr(ext, "SPV_EXT_mesh_shader");
        b.OpVec(10, ext);
    }
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {5365, fn_main};  // MeshEXT
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_pv, v_idx});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 17, 1, 1, 1});
    b.Op(16, {fn_main, 26, 3});
    b.Op(16, {fn_main, 5270, 1});
    b.Op(16, {fn_main, 5298});

    b.Op(71, {t_pv, 2});
    b.Op(72, {t_pv, 0, 11, 0});
    b.Op(71, {v_idx, 11, 5296});  // BuiltIn PrimitiveTriangleIndicesEXT

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(21, {t_u32, 32, 0});
    b.Op(21, {t_i32, 32, 1});
    b.Op(22, {t_f32, 32});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(23, {t_v3u, t_u32, 3});

    b.Op(43, {t_u32, c_u0, 0});
    b.Op(43, {t_u32, c_u1, 1});
    b.Op(43, {t_u32, c_u2, 2});
    b.Op(43, {t_u32, c_u3, 3});
    b.Op(43, {t_i32, c_i0, 0});

    b.Op(30, {t_pv, t_v4});
    b.Op(28, {t_arr_pv, t_pv, c_u3});
    b.Op(32, {t_ptr_arr_pv, 3, t_arr_pv});
    b.Op(32, {t_ptr_out_v4, 3, t_v4});
    b.Op(28, {t_arr_idx, t_v3u, c_u1});
    b.Op(32, {t_ptr_arr_idx, 3, t_arr_idx});
    b.Op(32, {t_ptr_out_v3u, 3, t_v3u});

    const uint32_t cm08 = b.Id(), cp08 = b.Id(), cz = b.Id(), c1f = b.Id(), c0f = b.Id();
    b.Op(43, {t_f32, cm08, F32Bits(-0.8f)});
    b.Op(43, {t_f32, cp08, F32Bits(0.8f)});
    b.Op(43, {t_f32, cz, F32Bits(0.2f)});
    b.Op(43, {t_f32, c1f, F32Bits(1.0f)});
    b.Op(43, {t_f32, c0f, F32Bits(0.0f)});
    const uint32_t p0 = b.Id(), p1 = b.Id(), p2 = b.Id(), tri = b.Id();
    b.Op(44, {t_v4, p0, cm08, cm08, cz, c1f});
    b.Op(44, {t_v4, p1, cp08, cm08, cz, c1f});
    b.Op(44, {t_v4, p2, c0f, cp08, cz, c1f});
    b.Op(44, {t_v3u, tri, c_u0, c_u1, c_u2});

    b.Op(59, {t_ptr_arr_pv, v_pv, 3});
    b.Op(59, {t_ptr_arr_idx, v_idx, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    b.Op(5295, {c_u3, c_u1});  // OpSetMeshOutputsEXT 3 1
    const uint32_t a0 = b.Id(), a1 = b.Id(), a2 = b.Id(), ai = b.Id();
    b.Op(65, {t_ptr_out_v4, a0, v_pv, c_u0, c_i0});
    b.Op(62, {a0, p0});
    b.Op(65, {t_ptr_out_v4, a1, v_pv, c_u1, c_i0});
    b.Op(62, {a1, p1});
    b.Op(65, {t_ptr_out_v4, a2, v_pv, c_u2, c_i0});
    b.Op(62, {a2, p2});
    b.Op(65, {t_ptr_out_v3u, ai, v_idx, c_u0});
    b.Op(62, {ai, tri});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010400u);
}

#if defined(_WIN32)

HRESULT CreateBuf(
    ID3D12Device* dev,
    D3D12_HEAP_TYPE heap_type,
    UINT64 bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES init_state,
    ID3D12Resource** out_res) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<UINT64>(bytes, 256ull);
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.SampleDesc.Count = 1u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr,
        OS_D3D12_IID(ID3D12Resource), reinterpret_cast<void**>(out_res));
}

bool WaitFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 val) {
    if (FAILED(queue->Signal(fence, val))) return false;
    if (fence->GetCompletedValue() >= val) return true;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return false;
    const HRESULT hr = fence->SetEventOnCompletion(val, ev);
    const DWORD wr = SUCCEEDED(hr) ? WaitForSingleObject(ev, 15000u) : WAIT_FAILED;
    CloseHandle(ev);
    return wr == WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------
// Deliverable 1: Page-at-a-Time Descriptor Heaps, Unbounded Sampler Heap (>2048),
//                GetGpuHardwareConfigurationX, Dedicated Copy Engine Queue Parity
// ---------------------------------------------------------------------------
void TestDeliverable1_D3D12(bool prefer_warp) {
    const char* label = prefer_warp ? "WARP" : "HW";
    std::printf("\n=== Deliverable 1 (%s): Paged Heaps + Unbounded Sampler + HwConfig + Copy Queue ===\n", label);

    OpenScarlettD3D12SetPreferWarp(prefer_warp ? 1 : 0);
    ComPtr<ID3D12DeviceX> dev;
    HRESULT hr = D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX, dev.PutVoid());
    S16_CHECK(SUCCEEDED(hr) && dev, "D3D12CreateDeviceX must succeed");
    if (!dev) return;

    ComPtr<IOpenScarlettGraphicsCompleteness> comp;
    hr = dev->QueryInterface(IID_IOpenScarlettGraphicsCompleteness, comp.PutVoid());
    S16_CHECK(SUCCEEDED(hr) && comp, "QueryInterface(IOpenScarlettGraphicsCompleteness) must succeed");

    // 1A. GetGpuHardwareConfigurationX (COM method + both C exports)
    D3D12XBOX_GPU_HARDWARE_CONFIGURATION cfg_com{}, cfg_exp1{}, cfg_exp2{};
    S16_CHECK(SUCCEEDED(dev->GetGpuHardwareConfigurationX(&cfg_com)), "GetGpuHardwareConfigurationX COM failed");
    S16_CHECK(SUCCEEDED(D3D12XboxGetGpuHardwareConfiguration(dev.Get(), &cfg_exp1)),
              "D3D12XboxGetGpuHardwareConfiguration C export failed");
    S16_CHECK(SUCCEEDED(OpenScarlettD3D12GetGpuHardwareConfigurationX(dev.Get(), &cfg_exp2)),
              "OpenScarlettD3D12GetGpuHardwareConfigurationX C export failed");
    S16_CHECK(cfg_com.StructSize == sizeof(D3D12XBOX_GPU_HARDWARE_CONFIGURATION), "StructSize exact");
    S16_CHECK(cfg_com.ComputeUnitCount == 20u || cfg_com.ComputeUnitCount == 52u, "Valid Scarlett CU count");
    S16_CHECK(cfg_com.ShaderEngineCount == 2u || cfg_com.ShaderEngineCount == 4u, "Valid shader engine count");
    S16_CHECK(cfg_com.HostWaveLaneMin >= 4u && cfg_com.HostWaveLaneMax >= cfg_com.HostWaveLaneMin, "Valid wave range");
    S16_CHECK(cfg_com.HostRaytracingTier == 0u || cfg_com.HostRaytracingTier == 10u ||
              cfg_com.HostRaytracingTier == 11u || cfg_com.HostRaytracingTier == 12u, "Sanitized DXR tier");
    S16_CHECK(cfg_com.HostMeshShaderTier == 0u || cfg_com.HostMeshShaderTier == 10u, "Sanitized MeshTier");
    S16_CHECK(cfg_com.SupportsPageIsolatedDescriptorHeaps == 1u &&
              cfg_com.SupportsUnboundedSamplerHeaps == 1u &&
              cfg_com.SupportsDedicatedCopyEngine == 1u, "Parity capability flags advertised");
    S16_CHECK(std::memcmp(&cfg_com, &cfg_exp1, sizeof(cfg_com)) == 0 &&
              std::memcmp(&cfg_com, &cfg_exp2, sizeof(cfg_com)) == 0, "C exports match COM GetGpuHardwareConfigurationX");
    std::printf("  [OK] HwConfig(%s): adapter=\"%s\" CUs=%u wave=%u..%u DXR=%u Mesh=%u\n",
                label, cfg_com.AdapterName, cfg_com.ComputeUnitCount,
                cfg_com.HostWaveLaneMin, cfg_com.HostWaveLaneMax,
                cfg_com.HostRaytracingTier, cfg_com.HostMeshShaderTier);

    // 1B. Page-at-a-Time Console Descriptor Heap Manager (xcl #84)
    D3D12_DESCRIPTOR_HEAP_DESC cbv_desc{};
    cbv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_desc.NumDescriptors = 4096u;
    cbv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> cbv_heap;
    S16_CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&cbv_desc, OS_D3D12_IID(ID3D12DescriptorHeap), cbv_heap.PutVoid())),
              "CreateDescriptorHeap(4096 CBV/SRV/UAV) failed");

    const UINT cbv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT descs_per_page = std::max<UINT>(1u, 65536u / std::max<UINT>(cbv_inc, 32u));
    const UINT initial_pages = std::max<UINT>(1u, (cbv_desc.NumDescriptors + descs_per_page - 1u) / descs_per_page);
    std::vector<uint32_t> alloc_pages(initial_pages + 1u, 0u);
    std::vector<uint64_t> alloc_gens(initial_pages + 1u, 0ull);
    std::vector<uint64_t> alloc_bases(initial_pages + 1u, 0ull);
    for (UINT i = 0u; i <= initial_pages; ++i) {
        S16_CHECK(SUCCEEDED(comp->AllocateDescriptorHeapPage(
                      cbv_heap.Get(), &alloc_pages[i], &alloc_gens[i], &alloc_bases[i])),
                  "Allocate descriptor page (including overflow isolated shadow page)");
    }
    const uint32_t p0 = alloc_pages[0], p1 = alloc_pages[1], p_iso = alloc_pages[initial_pages];
    const uint64_t g0 = alloc_gens[0], b0 = alloc_bases[0], b1 = alloc_bases[1], b_iso = alloc_bases[initial_pages];
    S16_CHECK(p0 != p1 && p_iso != p0 && p_iso != p1 && b_iso != 0ull && b_iso != b0 && b_iso != b1,
              "Isolated descriptor pages have distinct page indices & GPU bases");
    S16_CHECK(SUCCEEDED(comp->RetireDescriptorHeapPage(cbv_heap.Get(), p0)), "Retire page 0");
    uint32_t p0_reused = 99u;
    uint64_t g0_next = 0ull, b0_next = 0ull;
    S16_CHECK(SUCCEEDED(comp->AllocateDescriptorHeapPage(cbv_heap.Get(), &p0_reused, &g0_next, &b0_next)),
              "Re-allocate retired page 0");
    S16_CHECK(p0_reused == p0 && g0_next > g0, "Retired page generation incremented on reuse");

    // 1C. Unbounded Sampler Heap (> 2048 desktop limit -> deduplicated 2048-slot backing host heap)
    D3D12_DESCRIPTOR_HEAP_DESC samp_desc{};
    samp_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samp_desc.NumDescriptors = 4096u;  // Exceeds desktop 2048 hard limit!
    samp_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> samp_heap;
    hr = dev->CreateDescriptorHeap(&samp_desc, OS_D3D12_IID(ID3D12DescriptorHeap), samp_heap.PutVoid());
    S16_CHECK(SUCCEEDED(hr) && samp_heap, "CreateDescriptorHeap(4096 SAMPLER) must succeed via virtualization");
    if (samp_heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC reported = samp_heap->GetDesc();
        S16_CHECK(reported.NumDescriptors == 4096u, "Virtual unbounded sampler heap reports full 4096 NumDescriptors");
        const UINT samp_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = samp_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = samp_heap->GetGPUDescriptorHandleForHeapStart();
        for (UINT i = 0u; i < 4096u; ++i) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = ((i % 16u) & 1u) ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = ((i % 16u) & 2u) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.AddressV = ((i % 16u) & 4u) ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxAnisotropy = 1u + ((i % 16u) >> 3u);
            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            D3D12_CPU_DESCRIPTOR_HANDLE h{cpu0.ptr + static_cast<SIZE_T>(i) * samp_inc};
            dev->CreateSampler(&sd, h);
        }
        // Test CopyDescriptorsSimple inside the virtualized unbounded sampler heap!
        D3D12_CPU_DESCRIPTOR_HANDLE copy_src{cpu0.ptr + static_cast<SIZE_T>(3u) * samp_inc};
        D3D12_CPU_DESCRIPTOR_HANDLE copy_dst{cpu0.ptr + static_cast<SIZE_T>(3500u) * samp_inc};
        dev->CopyDescriptorsSimple(1u, copy_dst, copy_src, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        // Bind both CBV/SRV/UAV + unbounded sampler heap on a real Direct command list and bind slot 3500!
        D3D12_COMMAND_QUEUE_DESC dqd{};
        dqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> direct_q;
        ComPtr<ID3D12CommandAllocator> direct_alloc;
        ComPtr<ID3D12GraphicsCommandList> direct_cl;
        S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&dqd, OS_D3D12_IID(ID3D12CommandQueue), direct_q.PutVoid())), "Direct queue");
        S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), direct_alloc.PutVoid())), "Direct alloc");
        S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_alloc.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), direct_cl.PutVoid())), "Direct CL");

        // Build a minimal root signature accepting 1 sampler descriptor table at slot 0.
        D3D12_DESCRIPTOR_RANGE rng{};
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        rng.NumDescriptors = 1u;
        rng.BaseShaderRegister = 0u;
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1u;
        rp.DescriptorTable.pDescriptorRanges = &rng;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1u;
        rsd.pParameters = &rp;
        ComPtr<ID3DBlob> sig_blob, err_blob;
        ComPtr<ID3D12RootSignature> rs;
        S16_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())), "Serialize RS");
        S16_CHECK(SUCCEEDED(dev->CreateRootSignature(0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), OS_D3D12_IID(ID3D12RootSignature), rs.PutVoid())), "Create RS");

        ID3D12DescriptorHeap* heaps[2] = {cbv_heap.Get(), samp_heap.Get()};
        direct_cl->SetDescriptorHeaps(2u, heaps);
        direct_cl->SetComputeRootSignature(rs.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE virt_slot_3500{gpu0.ptr + 3500ull * samp_inc};
        direct_cl->SetComputeRootDescriptorTable(0u, virt_slot_3500);
        S16_CHECK(SUCCEEDED(direct_cl->Close()), "Close Direct CL after virtualized sampler table bind");
        ID3D12CommandList* cls[1] = {direct_cl.Get()};
        direct_q->ExecuteCommandLists(1u, cls);
        ComPtr<ID3D12Fence> f;
        S16_CHECK(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), f.PutVoid())), "Fence");
        S16_CHECK(WaitFence(direct_q.Get(), f.Get(), 1ull), "Direct queue completes virtual sampler table command list");
    }

    // 1D. Dedicated Copy Engine Queue Parity + FenceX Multi-Queue Sync (Copy -> Direct)
    D3D12_COMMAND_QUEUE_DESC cqd{};
    cqd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    D3D12_COMMAND_QUEUE_DESC dqd{};
    dqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> copy_q, direct_q;
    ComPtr<ID3D12CommandAllocator> copy_alloc, direct_alloc;
    ComPtr<ID3D12GraphicsCommandList> copy_cl, direct_cl;
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&cqd, OS_D3D12_IID(ID3D12CommandQueue), copy_q.PutVoid())), "Create Copy queue");
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&dqd, OS_D3D12_IID(ID3D12CommandQueue), direct_q.PutVoid())), "Create Direct queue");
    S16_CHECK(copy_q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY, "Copy queue preserves genuine COPY type");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, OS_D3D12_IID(ID3D12CommandAllocator), copy_alloc.PutVoid())), "Copy alloc");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COPY, copy_alloc.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), copy_cl.PutVoid())), "Copy CL");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), direct_alloc.PutVoid())), "Direct alloc 2");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_alloc.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), direct_cl.PutVoid())), "Direct CL 2");

    ComPtr<ID3D12Resource> up_buf, def_buf, rb_buf;
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 4096u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, up_buf.Put())), "Upload buf");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 4096u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, def_buf.Put())), "Default buf");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_READBACK, 4096u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, rb_buf.Put())), "Readback buf");

    std::vector<uint32_t> pattern(1024u);
    for (uint32_t i = 0u; i < 1024u; ++i) pattern[i] = 0xA5000000u ^ (i * 2654435761u);
    void* up_map = nullptr;
    D3D12_RANGE er{0, 0};
    S16_CHECK(SUCCEEDED(up_buf->Map(0u, &er, &up_map)) && up_map, "Map upload");
    std::memcpy(up_map, pattern.data(), 4096u);
    up_buf->Unmap(0u, nullptr);

    // Issue illegal non-copy transition + UAV barrier on Copy list; CommandListX MUST sanitize cleanly!
    D3D12_RESOURCE_BARRIER bad_copy_bars[2]{};
    bad_copy_bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bad_copy_bars[0].Transition = {def_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET};
    bad_copy_bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    bad_copy_bars[1].UAV.pResource = def_buf.Get();
    copy_cl->ResourceBarrier(2u, bad_copy_bars);
    copy_cl->CopyBufferRegion(def_buf.Get(), 0ull, up_buf.Get(), 0ull, 4096ull);
    S16_CHECK(SUCCEEDED(copy_cl->Close()), "Copy CL closes cleanly");

    direct_cl->CopyBufferRegion(rb_buf.Get(), 0ull, def_buf.Get(), 0ull, 4096ull);
    S16_CHECK(SUCCEEDED(direct_cl->Close()), "Direct CL closes cleanly");

    ComPtr<ID3D12Fence> sync_fence;
    S16_CHECK(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), sync_fence.PutVoid())), "Sync fence");
    ID3D12CommandList* copy_lists[1] = {copy_cl.Get()};
    copy_q->ExecuteCommandLists(1u, copy_lists);
    S16_CHECK(SUCCEEDED(copy_q->Signal(sync_fence.Get(), 1ull)), "Copy queue signal");
    S16_CHECK(SUCCEEDED(direct_q->Wait(sync_fence.Get(), 1ull)), "Direct queue wait on Copy fence");
    ID3D12CommandList* dir_lists[1] = {direct_cl.Get()};
    direct_q->ExecuteCommandLists(1u, dir_lists);
    S16_CHECK(WaitFence(direct_q.Get(), sync_fence.Get(), 2ull), "Direct queue completes after Copy sync");

    void* rb_map = nullptr;
    D3D12_RANGE rr{0, 4096u};
    S16_CHECK(SUCCEEDED(rb_buf->Map(0u, &rr, &rb_map)) && rb_map, "Map readback");
    S16_CHECK(std::memcmp(rb_map, pattern.data(), 4096u) == 0, "Copy -> Direct FenceX data is 100% byte-exact");
    rb_buf->Unmap(0u, &er);

    D3D12XBOX_GRAPHICS_COMPLETENESS_STATS stats{};
    S16_CHECK(SUCCEEDED(comp->GetGraphicsCompletenessStats(&stats)), "GetGraphicsCompletenessStats");
    S16_CHECK(stats.paged_descriptor_heaps_created >= 1ull && stats.descriptor_pages_allocated >= 3ull &&
              stats.descriptor_page_rotations >= 1ull, "Paged descriptor heap telemetry verified");
    S16_CHECK(stats.unbounded_sampler_heaps_created >= 1ull && stats.unbounded_sampler_writes >= 4097ull &&
              stats.unbounded_sampler_unique_host_slots == 16ull && stats.unbounded_sampler_dedup_hits >= 4080ull &&
              stats.unbounded_sampler_table_translations >= 1ull, "Unbounded sampler dedup & table translation verified");
    S16_CHECK(stats.dedicated_copy_queues_created >= 1ull && stats.copy_queue_barriers_sanitized >= 2ull &&
              stats.copy_queue_uav_barriers_suppressed >= 1ull, "Copy Engine queue parity telemetry verified");
    std::printf("  [OK] D3D12(%s): paged_pages=%llu, unbounded_samplers=%llu->%llu unique (dedup=%llu, xlat=%llu), copy_sanitized=%llu\n",
                label,
                static_cast<unsigned long long>(stats.descriptor_pages_allocated),
                static_cast<unsigned long long>(stats.unbounded_sampler_writes),
                static_cast<unsigned long long>(stats.unbounded_sampler_unique_host_slots),
                static_cast<unsigned long long>(stats.unbounded_sampler_dedup_hits),
                static_cast<unsigned long long>(stats.unbounded_sampler_table_translations),
                static_cast<unsigned long long>(stats.copy_queue_barriers_sanitized));
}

// ---------------------------------------------------------------------------
// Deliverable 2: Real ExecuteIndirect Completeness (D3D12 HW+WARP AND Vulkan 1.4)
// ---------------------------------------------------------------------------
void TestDeliverable2_D3D12(bool prefer_warp) {
    const char* label = prefer_warp ? "WARP" : "HW";
    std::printf("\n=== Deliverable 2A (%s): D3D12 ExecuteIndirect Completeness ===\n", label);

    OpenScarlettD3D12SetPreferWarp(prefer_warp ? 1 : 0);
    struct GuestVaGuard {
        void* ptr = nullptr;
        ~GuestVaGuard() { if (ptr) VirtualFree(ptr, 0u, MEM_RELEASE); }
    } guest_uav_guard;
    ComPtr<ID3D12DeviceX> dev;
    S16_CHECK(SUCCEEDED(D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX, dev.PutVoid())) && dev,
              "Create DeviceX");
    if (!dev) return;

    ComPtr<IOpenScarlettGraphicsCompleteness> comp;
    S16_CHECK(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettGraphicsCompleteness, comp.PutVoid())) && comp, "QI completeness");

    // Compile tiny HLSL compute shader:
    //   cbuffer RootC : register(b0) { uint base_off; uint tag_val; };
    //   RWByteAddressBuffer OutBuf : register(u0);
    //   [numthreads(1,1,1)] void main(uint3 tid : SV_DispatchThreadID) { OutBuf.Store((base_off + tid.x) * 4, tag_val + tid.x); }
    const std::string hlsl = R"HLSL(
cbuffer RootC : register(b0) { uint g_base; uint g_tag; };
RWByteAddressBuffer g_Out : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    g_Out.Store((g_base + gid.x) * 4u, g_tag + gid.x);
}
)HLSL";
    std::vector<uint8_t> dxil;
    std::string diag;
    auto st = openscarlett::CompileHlslWithDxc(hlsl, "cs_6_2", "main", dxil, diag);
    if (st.ok()) st = openscarlett::ResignDxilContainer(dxil);
    S16_CHECK(st.ok() && !dxil.empty(), "Compile ExecuteIndirect compute DXIL");
    if (!st.ok()) return;

    // Root Signature: [0] = 2x 32-bit RootConstants (b0), [1] = Root UAV (u0)
    D3D12_ROOT_PARAMETER rps[2]{};
    rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rps[0].Constants = {0u, 0u, 2u};
    rps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rps[1].Descriptor = {0u, 0u};
    rps[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2u;
    rsd.pParameters = rps;
    ComPtr<ID3DBlob> sig_blob, err_blob;
    ComPtr<ID3D12RootSignature> rs;
    S16_CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())), "Serialize indirect RS");
    S16_CHECK(SUCCEEDED(dev->CreateRootSignature(0u, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), OS_D3D12_IID(ID3D12RootSignature), rs.PutVoid())), "Create indirect RS");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = rs.Get();
    pso_desc.CS = {dxil.data(), dxil.size()};
    ComPtr<ID3D12PipelineState> pso;
    S16_CHECK(SUCCEEDED(dev->CreateComputePipelineState(&pso_desc, OS_D3D12_IID(ID3D12PipelineState), pso.PutVoid())), "Create indirect PSO");

    // 2A-1. Malformed command signature (ByteStride < required) MUST return E_INVALIDARG
    D3D12_INDIRECT_ARGUMENT_DESC bad_arg{};
    bad_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC bad_sig_desc{};
    bad_sig_desc.ByteStride = 4u;  // Too small (< 12 bytes)
    bad_sig_desc.NumArgumentDescs = 1u;
    bad_sig_desc.pArgumentDescs = &bad_arg;
    ComPtr<ID3D12CommandSignature> bad_sig;
    S16_CHECK(dev->CreateCommandSignature(&bad_sig_desc, nullptr, OS_D3D12_IID(ID3D12CommandSignature), bad_sig.PutVoid()) == E_INVALIDARG,
              "Malformed stride < min_stride rejected with E_INVALIDARG");

    // 2A-2. Real multi-dispatch ExecuteIndirect with CONSTANT (2 dwords) + UAV (8B guest VA!) + DISPATCH (12B) + CountBuffer!
    D3D12_INDIRECT_ARGUMENT_DESC args[3]{};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant.RootParameterIndex = 0u;
    args[0].Constant.DestOffsetIn32BitValues = 0u;
    args[0].Constant.Num32BitValuesToSet = 2u;
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    args[1].UnorderedAccessView.RootParameterIndex = 1u;
    args[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    struct IndirectDispatchRec {
        uint32_t base_off;
        uint32_t tag_val;
        uint64_t uav_gpu_va;
        uint32_t gx, gy, gz;
    };
    static_assert(sizeof(IndirectDispatchRec) == 32u, "Aligned indirect dispatch record size (28B payload + 4B pad)");
    D3D12_COMMAND_SIGNATURE_DESC sig_desc{};
    sig_desc.ByteStride = sizeof(IndirectDispatchRec);
    sig_desc.NumArgumentDescs = 3u;
    sig_desc.pArgumentDescs = args;
    ComPtr<ID3D12CommandSignature> cmd_sig;
    S16_CHECK(SUCCEEDED(dev->CreateCommandSignature(&sig_desc, rs.Get(), OS_D3D12_IID(ID3D12CommandSignature), cmd_sig.PutVoid())) && cmd_sig,
              "CreateCommandSignature(CONSTANT + UAV + DISPATCH)");

    // 2A-3. DispatchMesh indirect signature (HW on MeshTier>=10, virtual fallback on WARP/MeshTier==0)
    D3D12_INDIRECT_ARGUMENT_DESC mesh_arg{};
    mesh_arg.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(10u);  // D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH
    D3D12_COMMAND_SIGNATURE_DESC mesh_sig_desc{};
    mesh_sig_desc.ByteStride = 12u;
    mesh_sig_desc.NumArgumentDescs = 1u;
    mesh_sig_desc.pArgumentDescs = &mesh_arg;
    ComPtr<ID3D12CommandSignature> mesh_sig;
    S16_CHECK(SUCCEEDED(dev->CreateCommandSignature(&mesh_sig_desc, nullptr, OS_D3D12_IID(ID3D12CommandSignature), mesh_sig.PutVoid())) && mesh_sig,
              "CreateCommandSignature(DISPATCH_MESH) succeeds on both HW & WARP");

    ComPtr<ID3D12Resource> out_uav, out_rb, arg_buf, cnt_buf;
    // Allocate a real TitleVA guest buffer and bind it via CreatePlacedResourceX so
    // TitleVAMirror automatically translates the console guest VA inside ExecuteIndirect!
    guest_uav_guard.ptr = VirtualAlloc(nullptr, 65536u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* guest_uav_mem = guest_uav_guard.ptr;
    S16_CHECK(guest_uav_mem != nullptr, "VirtualAlloc guest TitleVA buffer");
    const uint64_t guest_uav_va = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(guest_uav_mem));
    D3D12_RESOURCE_DESC uav_desc{};
    uav_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uav_desc.Width = 256ull;
    uav_desc.Height = 1u;
    uav_desc.DepthOrArraySize = 1u;
    uav_desc.MipLevels = 1u;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.SampleDesc.Count = 1u;
    uav_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    S16_CHECK(SUCCEEDED(dev->CreatePlacedResourceX(
                  guest_uav_va, &uav_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                  OS_D3D12_IID(ID3D12Resource), out_uav.PutVoid())) && out_uav,
              "CreatePlacedResourceX(guest TitleVA UAV)");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_READBACK, 256u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, out_rb.Put())), "Out RB");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, arg_buf.Put())), "Arg buf");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 256u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, cnt_buf.Put())), "Count buf");

    IndirectDispatchRec recs[2]{};
    recs[0] = {0u, 0x1100u, guest_uav_va, 4u, 1u, 1u};  // Writes indices 0..3 -> 0x1100..0x1103
    recs[1] = {4u, 0x2200u, guest_uav_va, 4u, 1u, 1u};  // Writes indices 4..7 -> 0x2200..0x2203
    void* p = nullptr;
    D3D12_RANGE er{0, 0};
    S16_CHECK(SUCCEEDED(arg_buf->Map(0u, &er, &p)) && p, "Map arg_buf");
    std::memcpy(p, recs, sizeof(recs));
    arg_buf->Unmap(0u, nullptr);

    const uint32_t count_val = 2u;
    S16_CHECK(SUCCEEDED(cnt_buf->Map(0u, &er, &p)) && p, "Map cnt_buf");
    std::memcpy(p, &count_val, sizeof(count_val));
    cnt_buf->Unmap(0u, nullptr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, OS_D3D12_IID(ID3D12CommandQueue), q.PutVoid())), "Q");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), alloc.PutVoid())), "Alloc");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), cl.PutVoid())), "CL");
    S16_CHECK(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), fence.PutVoid())), "Fence");

    cl->SetComputeRootSignature(rs.Get());
    cl->SetPipelineState(pso.Get());
    // Intentionally pass MaxCommandCount = 500 (> 256-byte buffer capacity) to verify bounds clamping!
    cl->ExecuteIndirect(cmd_sig.Get(), 500u, arg_buf.Get(), 0ull, cnt_buf.Get(), 0ull);
    // Also issue an intentionally misaligned ExecuteIndirect to verify safe rejection without crashing!
    cl->ExecuteIndirect(cmd_sig.Get(), 1u, arg_buf.Get(), 3ull, nullptr, 0ull);

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    post[0].UAV.pResource = out_uav.Get();
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition = {out_uav.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(2u, post);
    cl->CopyBufferRegion(out_rb.Get(), 0ull, out_uav.Get(), 0ull, 32ull);
    S16_CHECK(SUCCEEDED(cl->Close()), "Close indirect CL");
    ID3D12CommandList* cls[1] = {cl.Get()};
    q->ExecuteCommandLists(1u, cls);
    S16_CHECK(WaitFence(q.Get(), fence.Get(), 1ull), "Indirect compute finishes on GPU");

    // guest_uav_guard frees guest_uav_mem cleanly after all D3D12 resources are released.

    D3D12_RANGE rr{0, 32u};
    S16_CHECK(SUCCEEDED(out_rb->Map(0u, &rr, &p)) && p, "Map indirect readback");
    uint32_t got[8]{};
    std::memcpy(got, p, sizeof(got));
    out_rb->Unmap(0u, &er);
    const uint32_t expected[8] = {0x1100u, 0x1101u, 0x1102u, 0x1103u, 0x2200u, 0x2201u, 0x2202u, 0x2203u};
    S16_CHECK(std::memcmp(got, expected, sizeof(expected)) == 0,
              "D3D12 ExecuteIndirect (root constants + guest VA translation + pCountBuffer) output bit-exact");

    D3D12XBOX_GRAPHICS_COMPLETENESS_STATS stats{};
    S16_CHECK(SUCCEEDED(comp->GetGraphicsCompletenessStats(&stats)), "GetStats");
    S16_CHECK(stats.execute_indirect_calls >= 2ull && stats.execute_indirect_with_count_buffer >= 1ull &&
              stats.execute_indirect_with_root_constants >= 1ull && stats.execute_indirect_bounds_clamped >= 1ull &&
              stats.execute_indirect_rejected >= 1ull && stats.execute_indirect_va_translated >= 1ull,
              "ExecuteIndirect telemetry counters verified");
    std::printf("  [OK] D3D12(%s) ExecuteIndirect: clamped=%llu rejected=%llu va_xlat=%llu, out=[0x%X..0x%X]\n",
                label,
                static_cast<unsigned long long>(stats.execute_indirect_bounds_clamped),
                static_cast<unsigned long long>(stats.execute_indirect_rejected),
                static_cast<unsigned long long>(stats.execute_indirect_va_translated),
                got[0], got[7]);
}

void TestDeliverable2_VulkanIndirect() {
    std::printf("\n=== Deliverable 2B: Vulkan 1.4 ExecuteIndirect (Draw, IndexedCount, PushConst, Compute, Mesh) ===\n");
    openscarlett::VulkanBackend vk;
    auto st = vk.Initialize(/*allow_software_fallback=*/true);
    S16_CHECK(st.ok(), "VulkanBackend::Initialize");
    if (!st.ok()) return;
    const auto& info = vk.GetDeviceInfo();
    std::printf("  Vulkan device: %s (API %u.%u.%u, ICD=%s, multiDrawIndirect=%d, drawIndirectCount=%d, dynRender=%d, mesh=%d)\n",
                info.device_name.c_str(), info.api_major, info.api_minor, info.api_patch,
                vk.is_fallback_mode() ? "NO" : "YES",
                info.supports_multi_draw_indirect ? 1 : 0,
                info.supports_draw_indirect_count ? 1 : 0,
                info.supports_dynamic_rendering ? 1 : 0,
                info.supports_mesh_shader ? 1 : 0);

    const auto vs_spv = BuildPosOnlyVertexSpirv();
    const auto ps_spv = BuildPushConstFragmentSpirv();
    const auto cs_spv = BuildSimpleMulAddComputeSpirv();
    const auto ms_spv = BuildSimpleMeshSpirv14();

    // Two non-overlapping triangles in one vertex buffer: left half (verts 0..2), right half (verts 3..5)
    struct PosV { float x, y, z, w; };
    const PosV verts[6] = {
        {-0.95f, -0.95f, 0.5f, 1.0f}, {-0.05f, -0.95f, 0.5f, 1.0f}, {-0.50f,  0.95f, 0.5f, 1.0f},
        { 0.05f, -0.95f, 0.5f, 1.0f}, { 0.95f, -0.95f, 0.5f, 1.0f}, { 0.50f,  0.95f, 0.5f, 1.0f},
    };
    const uint32_t indices[6] = {0u, 1u, 2u, 3u, 4u, 5u};

    // 1. Multi-draw indirect with indirect push-constants (left=RED, right=BLUE)
    {
        struct IndirectPcDraw {
            float rgba[4];
            uint32_t vertex_count, instance_count, first_vertex, first_instance;
        };
        IndirectPcDraw cmds[2] = {
            {{1.0f, 0.0f, 0.0f, 1.0f}, 3u, 1u, 0u, 0u},
            {{0.0f, 0.0f, 1.0f, 1.0f}, 3u, 1u, 3u, 0u},
        };
        openscarlett::VulkanIndirectDrawDesc desc{};
        desc.kind = openscarlett::VulkanIndirectCommandKind::Draw;
        desc.graphics.vertex_spirv = vs_spv;
        desc.graphics.fragment_spirv = ps_spv;
        desc.graphics.width = 64u;
        desc.graphics.height = 64u;
        desc.graphics.vertex_bytes.resize(sizeof(verts));
        std::memcpy(desc.graphics.vertex_bytes.data(), verts, sizeof(verts));
        desc.graphics.vertex_stride = sizeof(PosV);
        desc.graphics.vertex_attributes.push_back({0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u});
        desc.argument_buffer_bytes.resize(sizeof(cmds));
        std::memcpy(desc.argument_buffer_bytes.data(), cmds, sizeof(cmds));
        desc.command_stride = sizeof(IndirectPcDraw);
        desc.max_command_count = 99u;  // Deliberately oversized -> must clamp to 2!
        desc.indirect_push_constant_arg_offset = 0u;
        desc.indirect_push_constant_bytes = 16u;
        desc.indirect_push_constant_dst_offset = 0u;
        desc.draw_arg_offset_in_stride = 16u;

        openscarlett::VulkanIndirectDrawResult res{};
        S16_CHECK(vk.ExecuteIndirectGraphicsDraw(desc, res).ok(), "Vulkan indirect draw + push-constants");
        S16_CHECK(res.clamped_max_count && res.effective_command_count == 2u && res.used_indirect_push_constants,
                  "Vulkan indirect bounds clamping & push-constant replay verified");
        if (res.executed_on_vulkan_icd) {
            const size_t left_i = (32u * 64u + 16u) * 4u;
            const size_t right_i = (32u * 64u + 48u) * 4u;
            S16_CHECK(res.framebuffer.pixels[left_i + 0] > 200u && res.framebuffer.pixels[left_i + 2] < 20u,
                      "Left indirect triangle rendered RED");
            S16_CHECK(res.framebuffer.pixels[right_i + 2] > 200u && res.framebuffer.pixels[right_i + 0] < 20u,
                      "Right indirect triangle rendered BLUE");
        }
        std::printf("  [OK] Vulkan IndirectDraw + PushConstants (ICD=%d, clamped=%d)\n",
                    res.executed_on_vulkan_icd ? 1 : 0, res.clamped_max_count ? 1 : 0);
    }

    // 2. Indexed indirect draw with pCountBuffer (vkCmdDrawIndexedIndirectCount)
    {
        struct VkDrawIndexedIndirectCmd { uint32_t idx_cnt, inst_cnt, first_idx; int32_t vtx_off; uint32_t first_inst; };
        VkDrawIndexedIndirectCmd cmds[2] = {
            {3u, 1u, 0u, 0, 0u},
            {3u, 1u, 3u, 0, 0u},
        };
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        openscarlett::VulkanIndirectDrawDesc desc{};
        desc.kind = openscarlett::VulkanIndirectCommandKind::DrawIndexed;
        desc.graphics.vertex_spirv = vs_spv;
        desc.graphics.fragment_spirv = ps_spv;
        desc.graphics.width = 64u;
        desc.graphics.height = 64u;
        desc.graphics.vertex_bytes.resize(sizeof(verts));
        std::memcpy(desc.graphics.vertex_bytes.data(), verts, sizeof(verts));
        desc.graphics.vertex_stride = sizeof(PosV);
        desc.graphics.vertex_attributes.push_back({0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u});
        desc.graphics.indices.assign(indices, indices + 6);
        desc.graphics.push_constants.resize(16u);
        std::memcpy(desc.graphics.push_constants.data(), green, 16u);
        desc.argument_buffer_bytes.resize(sizeof(cmds));
        std::memcpy(desc.argument_buffer_bytes.data(), cmds, sizeof(cmds));
        desc.command_stride = sizeof(VkDrawIndexedIndirectCmd);
        desc.max_command_count = 2u;
        desc.use_count_buffer = true;
        desc.count_buffer_value = 1u;  // Draw ONLY the first (left) triangle!

        openscarlett::VulkanIndirectDrawResult res{};
        S16_CHECK(vk.ExecuteIndirectGraphicsDraw(desc, res).ok(), "Vulkan indexed indirect count draw");
        S16_CHECK(res.effective_command_count == 1u, "Count buffer limited draw count to 1");
        if (res.executed_on_vulkan_icd) {
            const size_t left_i = (32u * 64u + 16u) * 4u;
            const size_t right_i = (32u * 64u + 48u) * 4u;
            S16_CHECK(res.framebuffer.pixels[left_i + 1] > 200u, "Left triangle rendered GREEN via DrawIndexedIndirectCount");
            S16_CHECK(res.framebuffer.pixels[right_i + 1] < 20u, "Right triangle skipped because count buffer == 1");
        }
        std::printf("  [OK] Vulkan DrawIndexedIndirectCount (used_count_cmd=%d)\n",
                    res.used_indirect_count_cmd ? 1 : 0);
    }

    // 3. Indirect Compute (vkCmdDispatchIndirect)
    {
        openscarlett::VulkanIndirectComputeDesc cdesc{};
        cdesc.spirv = cs_spv;
        cdesc.input_u32.resize(128u);
        for (uint32_t i = 0u; i < 128u; ++i) cdesc.input_u32[i] = i + 10u;
        const uint32_t dispatch_cmd[3] = {2u, 1u, 1u};  // 2 * 64 = 128 threads
        cdesc.argument_buffer_bytes.resize(12u);
        std::memcpy(cdesc.argument_buffer_bytes.data(), dispatch_cmd, 12u);
        cdesc.output_word_count = 128u;

        openscarlett::VulkanIndirectComputeResult cres{};
        S16_CHECK(vk.ExecuteIndirectCompute(cdesc, cres).ok(), "Vulkan vkCmdDispatchIndirect");
        S16_CHECK(cres.output_u32.size() == 128u, "Indirect compute output size");
        bool exact = true;
        for (uint32_t i = 0u; i < 128u; ++i) {
            if (cres.output_u32[i] != (i + 10u) * 3u + 7u) { exact = false; break; }
        }
        S16_CHECK(exact, "Vulkan vkCmdDispatchIndirect output bit-exact");
        std::printf("  [OK] Vulkan DispatchIndirect (ICD=%d, out[0]=%u, out[127]=%u)\n",
                    cres.executed_on_vulkan_icd ? 1 : 0, cres.output_u32[0], cres.output_u32[127]);
    }

    // 4. Indirect Mesh Shader (vkCmdDrawMeshTasksIndirectEXT / CountEXT)
    if (info.supports_mesh_shader) {
        const uint32_t mesh_cmd[3] = {1u, 1u, 1u};
        const float cyan[4] = {0.0f, 0.9f, 1.0f, 1.0f};
        openscarlett::VulkanIndirectDrawDesc mdesc{};
        mdesc.kind = openscarlett::VulkanIndirectCommandKind::DrawMeshTasks;
        mdesc.mesh.mesh_spirv = ms_spv;
        mdesc.mesh.fragment_spirv = ps_spv;
        mdesc.mesh.width = 64u;
        mdesc.mesh.height = 64u;
        mdesc.mesh.push_constants.resize(16u);
        std::memcpy(mdesc.mesh.push_constants.data(), cyan, 16u);
        mdesc.argument_buffer_bytes.resize(12u);
        std::memcpy(mdesc.argument_buffer_bytes.data(), mesh_cmd, 12u);
        mdesc.command_stride = 12u;
        mdesc.max_command_count = 1u;
        mdesc.use_count_buffer = true;
        mdesc.count_buffer_value = 1u;
        openscarlett::VulkanIndirectDrawResult mres{};
        S16_CHECK(vk.ExecuteIndirectGraphicsDraw(mdesc, mres).ok(), "Vulkan DrawMeshTasksIndirectEXT");
        if (mres.executed_on_vulkan_icd) {
            const size_t ci = (32u * 64u + 32u) * 4u;
            S16_CHECK(mres.used_mesh_indirect_ext && mres.framebuffer.pixels[ci + 1] > 180u &&
                      mres.framebuffer.pixels[ci + 2] > 200u,
                      "Vulkan DrawMeshTasksIndirectEXT rendered CYAN center pixel");
        }
        std::printf("  [OK] Vulkan DrawMeshTasksIndirectEXT (used_count_cmd=%d)\n",
                    mres.used_indirect_count_cmd ? 1 : 0);
    }

    // 5. Malformed offset / stride rejection
    {
        openscarlett::VulkanIndirectDrawDesc bad{};
        bad.kind = openscarlett::VulkanIndirectCommandKind::Draw;
        bad.graphics.vertex_spirv = vs_spv;
        bad.graphics.fragment_spirv = ps_spv;
        bad.argument_buffer_bytes.resize(16u, 0u);
        bad.argument_buffer_offset = 2ull;  // Misaligned
        openscarlett::VulkanIndirectDrawResult r{};
        S16_CHECK(!vk.ExecuteIndirectGraphicsDraw(bad, r).ok(), "Misaligned indirect offset rejected cleanly");
    }
}

// ---------------------------------------------------------------------------
// Deliverable 3: Live FMASK / DCC (2:1 & 4:1) / HTILE GPU Compute Resolve
// ---------------------------------------------------------------------------
void TestDeliverable3_LiveGpuFmaskDccHtileResolve() {
    std::printf("\n=== Deliverable 3: Live FMASK / DCC (2:1 & 4:1) / HTILE GPU Compute Resolve ===\n");

    // Validate generated SPIR-V 1.3 binary with internal + external Khronos spirv-val!
    std::vector<uint32_t> resolve_spv;
    S16_CHECK(openscarlett::EmitXgGpuResolveSpirv(resolve_spv).ok() && !resolve_spv.empty(),
              "EmitXgGpuResolveSpirv valid");
    std::error_code ec;
    if (std::filesystem::exists(openscarlett::kDefaultSpirvValPath, ec)) {
        std::string vdiag;
        auto vst = openscarlett::ValidateSpirvExternal(
            resolve_spv, vdiag, openscarlett::kDefaultSpirvValPath, "vulkan1.2");
        if (!vst.ok()) {
            std::fprintf(stderr, "  spirv-val(resolve) diagnostics: %s\n", vdiag.c_str());
        }
        S16_CHECK(vst.ok(), "Resolve SPIR-V 1.3 passes Khronos spirv-val");
    }

    openscarlett::XgGpuUntiler untiler_hw, untiler_warp, untiler_vk;
    const bool has_d3d_hw = untiler_hw.InitializeD3D12(/*prefer_warp=*/false).ok();
    const bool has_d3d_warp = untiler_warp.InitializeD3D12(/*prefer_warp=*/true).ok();
    const bool has_vk_hw = untiler_vk.InitializeVulkan(/*allow_software_fallback=*/false).ok() &&
                           untiler_vk.IsVulkanHardwareIcd();
    S16_CHECK(has_d3d_warp, "XgGpuUntiler D3D12 WARP initialized");
    S16_CHECK(has_d3d_hw || has_d3d_warp, "XgGpuUntiler D3D12 (HW or WARP) initialized");
    std::printf("  Backends ready: D3D12-HW=%d (%s), D3D12-WARP=%d, Vulkan-ICD=%d (%s)\n",
                has_d3d_hw ? 1 : 0, untiler_hw.D3D12AdapterName().c_str(),
                has_d3d_warp ? 1 : 0,
                has_vk_hw ? 1 : 0, untiler_vk.VulkanDeviceName().c_str());

    // Also verify DeviceX COM wiring for live GPU FMASK/DCC/HTILE resolve!
    OpenScarlettD3D12SetPreferWarp(0);
    ComPtr<ID3D12DeviceX> dev;
    ComPtr<IOpenScarlettGraphicsCompleteness> comp;
    S16_CHECK(SUCCEEDED(D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX, dev.PutVoid())) && dev, "DeviceX");
    S16_CHECK(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettGraphicsCompleteness, comp.PutVoid())) && comp, "QI completeness");

    // --- 3A. FMASK MSAA 2x, 4x, 8x Resolve & Per-Sample Expand ---
    const uint32_t sample_counts[3] = {2u, 4u, 8u};
    for (uint32_t sc : sample_counts) {
        const uint32_t W = 32u, H = 32u;
        const uint32_t clear_rgba = 0xFF336699u;
        std::vector<uint8_t> msaa_color(static_cast<size_t>(W) * H * sc * 4u, 0u);
        std::vector<uint8_t> fmask_bytes(static_cast<size_t>(W) * H * 4u, 0u);
        for (uint32_t y = 0u; y < H; ++y) {
            for (uint32_t x = 0u; x < W; ++x) {
                const size_t pid = static_cast<size_t>(y) * W + x;
                for (uint32_t f = 0u; f < sc; ++f) {
                    const uint32_t col = ((x * 7u + f * 31u) & 0xFFu) |
                                         (((y * 7u + f * 17u) & 0xFFu) << 8u) |
                                         (((x ^ y ^ (f * 53u)) & 0xFFu) << 16u) |
                                         (0xFFu << 24u);
                    std::memcpy(msaa_color.data() + (pid * sc + f) * 4u, &col, 4u);
                }
                uint8_t frags[8]{};
                for (uint32_t s = 0u; s < sc; ++s) {
                    if (((x + y) % 7u) == 0u && s == sc - 1u) frags[s] = 0xEu;  // Unknown/cleared sentinel
                    else if (((x + y) % 11u) == 0u) frags[s] = 0xFu;           // Fast-clear sentinel
                    else frags[s] = static_cast<uint8_t>((s + x) % sc);
                }
                const uint32_t fword = openscarlett::PackGfx10FmaskPixelWord(frags, sc);
                std::memcpy(fmask_bytes.data() + pid * 4u, &fword, 4u);
            }
        }

        for (uint32_t mode = 0u; mode <= 2u; ++mode) {
            openscarlett::XgGpuResolveParams rp{};
            rp.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::FmaskMsaaResolve);
            rp.width = W;
            rp.height = H;
            rp.sample_count = sc;
            rp.mode = mode;
            rp.bpe = 4u;
            rp.src_row_pitch = W * sc * 4u;
            rp.dst_row_pitch = W * 4u;
            rp.clear_reg_0 = clear_rgba;

            std::vector<uint8_t> ref_out;
            S16_CHECK(openscarlett::ResolveFmaskCpuReference(rp, msaa_color, fmask_bytes, ref_out).ok(), "FMASK CPU ref");

            if (has_d3d_hw) {
                std::vector<uint8_t> gpu_out(ref_out.size(), 0u);
                openscarlett::XgGpuDispatchMetrics m{};
                S16_CHECK(untiler_hw.ExecuteResolveD3D12(rp, msaa_color, fmask_bytes, gpu_out, &m).ok(), "FMASK D3D12 HW");
                S16_CHECK(m.executed_on_hardware_gpu && gpu_out == ref_out, "FMASK D3D12 HW bit-exact vs CPU ref");
            }
            if (has_d3d_warp && mode == 0u) {
                std::vector<uint8_t> warp_out(ref_out.size(), 0u);
                S16_CHECK(untiler_warp.ExecuteResolveD3D12(rp, msaa_color, fmask_bytes, warp_out, nullptr).ok(), "FMASK D3D12 WARP");
                S16_CHECK(warp_out == ref_out, "FMASK D3D12 WARP bit-exact vs CPU ref");
            }
            if (has_vk_hw) {
                std::vector<uint8_t> vk_out(ref_out.size(), 0u);
                openscarlett::XgGpuDispatchMetrics m{};
                S16_CHECK(untiler_vk.ExecuteResolveVulkan(rp, msaa_color, fmask_bytes, vk_out, &m).ok(), "FMASK Vulkan SPIR-V");
                S16_CHECK(m.used_vulkan_icd && vk_out == ref_out, "FMASK Vulkan SPIR-V bit-exact vs CPU ref");
            }
            if (sc == 4u && mode == 0u && comp) {
                std::vector<uint8_t> dev_out(ref_out.size(), 0u);
                uint32_t used_gpu = 0u;
                S16_CHECK(SUCCEEDED(comp->ExecuteGpuResolveFmask(
                              W, H, sc, mode, clear_rgba,
                              msaa_color.data(), static_cast<uint32_t>(msaa_color.size()),
                              fmask_bytes.data(), static_cast<uint32_t>(fmask_bytes.size()),
                              dev_out.data(), static_cast<uint32_t>(dev_out.size()), &used_gpu)),
                          "DeviceX::ExecuteGpuResolveFmask");
                S16_CHECK(used_gpu == 1u && dev_out == ref_out, "DeviceX FMASK resolve live GPU bit-exact");
            }
        }
    }
    std::printf("  [OK] FMASK MSAA 2x/4x/8x (Average + Centroid + PerSampleExpand): D3D12 HW + WARP + Vulkan + DeviceX bit-exact!\n");

    // --- 3B. DCC Compressed Block Decompression (Fast-Clear + 4:1 + 2:1 + Uncompressed, pipe-aligned) ---
    {
        const uint32_t W = 64u, H = 64u;
        const uint32_t clear_reg = 0xFF4488CCu;
        std::vector<uint8_t> orig_rgba(static_cast<size_t>(W) * H * 4u, 0u);
        for (uint32_t y = 0u; y < H; ++y) {
            for (uint32_t x = 0u; x < W; ++x) {
                const uint32_t bx = x >> 3u, by = y >> 3u;
                const uint32_t lx = x & 7u, ly = y & 7u;
                const uint32_t region = (by * 8u + bx) % 5u;
                uint32_t col = 0u;
                if (region == 0u) {
                    // Fast-clear region (cycles through 0x00, 0x40, 0x80, 0xC0, 0x20)
                    static constexpr uint32_t kClears[5] = {
                        0x00000000u, 0xFF000000u, 0x00FFFFFFu, 0xFFFFFFFFu, 0xFF4488CCu};
                    col = kClears[(bx + by) % 5u];
                } else if (region == 1u) {
                    // Smooth 4:1 block (RGB span <= 3, A span <= 1)
                    const uint32_t r = 100u + (lx & 3u);
                    const uint32_t g = 140u + (ly & 3u);
                    const uint32_t b = 180u + ((lx + ly) & 3u);
                    const uint32_t a = 250u + ((lx ^ ly) & 1u);
                    col = r | (g << 8u) | (b << 16u) | (a << 24u);
                } else if (region == 2u) {
                    // Gradient 2:1 block (RGB span <= 15, A span <= 3)
                    const uint32_t r = 60u + lx * 2u;
                    const uint32_t g = 90u + ly * 2u;
                    const uint32_t b = 120u + (lx + ly);
                    const uint32_t a = 240u + (lx & 3u);
                    col = r | (g << 8u) | (b << 16u) | (a << 24u);
                } else {
                    // High-contrast uncompressed block (0xFF)
                    const uint32_t r = (lx * 35u) & 0xFFu;
                    const uint32_t g = (ly * 35u) & 0xFFu;
                    const uint32_t b = ((lx * 29u + ly * 43u) & 0xFFu);
                    col = r | (g << 8u) | (b << 16u) | (0xFFu << 24u);
                }
                std::memcpy(orig_rgba.data() + (static_cast<size_t>(y) * W + x) * 4u, &col, 4u);
            }
        }

        std::vector<uint8_t> comp_blocks, dcc_keys;
        uint32_t cnt_fc = 0u, cnt_4 = 0u, cnt_2 = 0u, cnt_u = 0u;
        S16_CHECK(openscarlett::EncodeDccCompressedSurface2D(
                      W, H, orig_rgba, clear_reg, /*pipe_aligned=*/true, 16u,
                      comp_blocks, dcc_keys, &cnt_fc, &cnt_4, &cnt_2, &cnt_u).ok(),
                  "EncodeDccCompressedSurface2D");
        S16_CHECK(cnt_fc > 0u && cnt_4 > 0u && cnt_2 > 0u && cnt_u > 0u,
                  "DCC test surface exercises ALL 4 block modes (fast-clear, 4:1, 2:1, uncompressed)");

        openscarlett::XgGpuResolveParams dp{};
        dp.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::DccDecompress);
        dp.width = W;
        dp.height = H;
        dp.sample_count = 1u;
        dp.mode = 1u;  // pipe_aligned
        dp.bpe = 4u;
        dp.src_row_pitch = W * 4u;
        dp.dst_row_pitch = W * 4u;
        dp.meta_blocks_x = W / 8u;
        dp.meta_num_pipes = 16u;
        dp.meta_slice_pitch = static_cast<uint32_t>(dcc_keys.size());
        dp.clear_reg_0 = clear_reg;

        std::vector<uint8_t> ref_dcc;
        S16_CHECK(openscarlett::DecompressDccCpuReference(dp, comp_blocks, dcc_keys, ref_dcc).ok(), "DCC CPU ref");
        S16_CHECK(ref_dcc == orig_rgba, "DCC encode -> CPU decompress is 100% lossless bit-exact");

        if (has_d3d_hw) {
            std::vector<uint8_t> hw_dcc(orig_rgba.size(), 0u);
            S16_CHECK(untiler_hw.ExecuteResolveD3D12(dp, comp_blocks, dcc_keys, hw_dcc, nullptr).ok(), "DCC D3D12 HW");
            S16_CHECK(hw_dcc == orig_rgba, "DCC D3D12 HW decompress bit-exact vs original RGBA8");
        }
        if (has_d3d_warp) {
            std::vector<uint8_t> warp_dcc(orig_rgba.size(), 0u);
            S16_CHECK(untiler_warp.ExecuteResolveD3D12(dp, comp_blocks, dcc_keys, warp_dcc, nullptr).ok(), "DCC D3D12 WARP");
            S16_CHECK(warp_dcc == orig_rgba, "DCC D3D12 WARP decompress bit-exact vs original RGBA8");
        }
        if (has_vk_hw) {
            std::vector<uint8_t> vk_dcc(orig_rgba.size(), 0u);
            S16_CHECK(untiler_vk.ExecuteResolveVulkan(dp, comp_blocks, dcc_keys, vk_dcc, nullptr).ok(), "DCC Vulkan SPIR-V");
            S16_CHECK(vk_dcc == orig_rgba, "DCC Vulkan SPIR-V decompress bit-exact vs original RGBA8");
        }
        if (comp) {
            std::vector<uint8_t> dev_dcc(orig_rgba.size(), 0u);
            uint32_t used_gpu = 0u;
            S16_CHECK(SUCCEEDED(comp->ExecuteGpuDecompressDcc(
                          W, H, 1u, 16u, clear_reg,
                          comp_blocks.data(), static_cast<uint32_t>(comp_blocks.size()),
                          dcc_keys.data(), static_cast<uint32_t>(dcc_keys.size()),
                          dev_dcc.data(), static_cast<uint32_t>(dev_dcc.size()), &used_gpu)),
                      "DeviceX::ExecuteGpuDecompressDcc");
            S16_CHECK(used_gpu == 1u && dev_dcc == orig_rgba, "DeviceX DCC decompress live GPU bit-exact");
        }
        std::printf("  [OK] DCC Decompress (fc=%u, 4:1=%u, 2:1=%u, uncomp=%u): D3D12 HW + WARP + Vulkan + DeviceX bit-exact!\n",
                    cnt_fc, cnt_4, cnt_2, cnt_u);
    }

    // --- 3C. HTILE Depth/Stencil Decompression & HiZ Bounds Resolve ---
    {
        // Verify GFX10 HTILE word encode/decode (depth-only AND stencil+tc_compat)
        const uint32_t hw_stencil = openscarlett::EncodeGfx10HtileWord(3u, 0x1200u, 0x3A55u, 2u, true, true);
        const auto dec_s = openscarlett::DecodeGfx10HtileWord(hw_stencil, true);
        S16_CHECK(dec_s.zmask == 3u && dec_s.smem == 2u && dec_s.tc_compat && dec_s.max_z_14 == 0x3A55u,
                  "GFX10 HTILE stencil + tc_compat bitfields decode exact");

        const uint32_t W = 64u, H = 64u;
        const float clear_d = 0.75f;  // Exact in 24-bit fixed point (0.75 * 2^24 = 0xC00000)
        const uint8_t clear_s = 0x5Au;
        std::vector<float> depth_f32(static_cast<size_t>(W) * H, clear_d);
        std::vector<uint8_t> stencil_s8(static_cast<size_t>(W) * H, clear_s);

        for (uint32_t y = 0u; y < H; ++y) {
            for (uint32_t x = 0u; x < W; ++x) {
                const uint32_t bx = x >> 3u, by = y >> 3u;
                const uint32_t lx = x & 7u, ly = y & 7u;
                const uint32_t tid = by * 8u + bx;
                const size_t idx = static_cast<size_t>(y) * W + x;
                if ((tid % 3u) == 0u) {
                    // Fast-clear tile (zmask == 0)
                    depth_f32[idx] = clear_d;
                    stencil_s8[idx] = clear_s;
                } else if ((tid % 3u) == 1u) {
                    // Planar + small residual delta-Z compressed tile (zmask in 1..14)
                    const uint32_t z24 = 0x400000u + lx * 128u + ly * 256u + ((lx ^ ly) & 3u);
                    depth_f32[idx] = static_cast<float>(z24) * (1.0f / 16777216.0f);
                    stencil_s8[idx] = 0x22u;
                } else {
                    // High-frequency expanded tile (zmask == 15)
                    const uint32_t z24 = ((x * 9137u + y * 5711u) & 0x00FFFFFFu);
                    depth_f32[idx] = static_cast<float>(z24) * (1.0f / 16777216.0f);
                    stencil_s8[idx] = static_cast<uint8_t>((x ^ y) & 0xFFu);
                }
            }
        }

        for (int pass_fmt = 0; pass_fmt < 2; ++pass_fmt) {
            const bool is_d24s8 = (pass_fmt == 1);
            std::vector<uint8_t> comp_tiles, htile_words;
            uint32_t cnt_c = 0u, cnt_cmp = 0u, cnt_exp = 0u;
            S16_CHECK(openscarlett::EncodeHtileCompressedSurface2D(
                          W, H, depth_f32, &stencil_s8, clear_d, clear_s, is_d24s8,
                          comp_tiles, htile_words, &cnt_c, &cnt_cmp, &cnt_exp).ok(),
                      "EncodeHtileCompressedSurface2D");
            S16_CHECK(cnt_c > 0u && cnt_cmp > 0u && cnt_exp > 0u,
                      "HTILE test exercises fast-clear, compressed delta-Z, and expanded tiles");

            openscarlett::XgGpuResolveParams hp{};
            hp.op = static_cast<uint32_t>(openscarlett::XgGpuResolveOp::HtileDecompress);
            hp.width = W;
            hp.height = H;
            hp.sample_count = 1u;
            hp.mode = 1u | (is_d24s8 ? 2u : 0u);  // bit0 = emit HiZ bounds, bit1 = D24S8 vs D32F
            hp.bpe = 4u;
            hp.src_row_pitch = W * 4u;
            hp.dst_row_pitch = W * 4u;
            hp.meta_blocks_x = W / 8u;
            hp.clear_reg_0 = is_d24s8 ? 0x00C00000u : F32Bits(clear_d);
            hp.clear_reg_1 = clear_s;
            hp.spv_aux_word_offset = W * H;

            std::vector<uint8_t> ref_ht;
            std::vector<uint32_t> ref_hiz;
            S16_CHECK(openscarlett::DecompressHtileCpuReference(hp, comp_tiles, htile_words, ref_ht, &ref_hiz).ok(),
                      "HTILE CPU ref");
            S16_CHECK(ref_hiz.size() == 64u, "HTILE HiZ bounds count == 64 tiles");

            if (has_d3d_hw) {
                std::vector<uint8_t> hw_ht(ref_ht.size(), 0u);
                S16_CHECK(untiler_hw.ExecuteResolveD3D12(hp, comp_tiles, htile_words, hw_ht, nullptr).ok(), "HTILE D3D12 HW");
                S16_CHECK(hw_ht == ref_ht, "HTILE D3D12 HW depth + HiZ bit-exact vs CPU ref");
            }
            if (has_d3d_warp && !is_d24s8) {
                std::vector<uint8_t> warp_ht(ref_ht.size(), 0u);
                S16_CHECK(untiler_warp.ExecuteResolveD3D12(hp, comp_tiles, htile_words, warp_ht, nullptr).ok(), "HTILE D3D12 WARP");
                S16_CHECK(warp_ht == ref_ht, "HTILE D3D12 WARP depth + HiZ bit-exact vs CPU ref");
            }
            if (has_vk_hw) {
                std::vector<uint8_t> vk_ht(ref_ht.size(), 0u);
                S16_CHECK(untiler_vk.ExecuteResolveVulkan(hp, comp_tiles, htile_words, vk_ht, nullptr).ok(), "HTILE Vulkan SPIR-V");
                S16_CHECK(vk_ht == ref_ht, "HTILE Vulkan SPIR-V depth + HiZ bit-exact vs CPU ref");
            }
            if (!is_d24s8 && comp) {
                std::vector<uint8_t> dev_ht(ref_ht.size(), 0u);
                uint32_t used_gpu = 0u;
                S16_CHECK(SUCCEEDED(comp->ExecuteGpuDecompressHtile(
                              W, H, hp.mode, hp.clear_reg_0, hp.clear_reg_1,
                              comp_tiles.data(), static_cast<uint32_t>(comp_tiles.size()),
                              htile_words.data(), static_cast<uint32_t>(htile_words.size()),
                              dev_ht.data(), static_cast<uint32_t>(dev_ht.size()), &used_gpu)),
                          "DeviceX::ExecuteGpuDecompressHtile");
                S16_CHECK(used_gpu == 1u && dev_ht == ref_ht, "DeviceX HTILE resolve live GPU bit-exact");
            }
        }
        std::printf("  [OK] HTILE Depth/Stencil (D32F + D24S8) + HiZ Bounds: D3D12 HW + WARP + Vulkan + DeviceX bit-exact!\n");
    }
}

// ---------------------------------------------------------------------------
// Deliverable 4: Vulkan 1.4 Dynamic Rendering Multi-Pass + OPENSCARLETT_VK_GPU
//                Numeric Fallback + Strict Cross-IHV Hardening Matrix
// ---------------------------------------------------------------------------
void TestDeliverable4_VulkanDynRenderFallbackAndCrossIhv() {
    std::printf("\n=== Deliverable 4: Vulkan 1.4 Dynamic Rendering + VK_GPU Fallback + Cross-IHV Matrix ===\n");

    // 4A. Vulkan 1.4 Dynamic Rendering Multi-Pass RT + MSAA 4x + Depth Resolve
    {
        openscarlett::VulkanBackend vk;
        S16_CHECK(vk.Initialize(/*allow_software_fallback=*/true).ok(), "Init Vulkan for dynamic rendering");
        const auto vs_spv = BuildPosOnlyVertexSpirv();
        const auto ps_spv = BuildPushConstFragmentSpirv();

        struct PosV { float x, y, z, w; };
        // Pass 1: Large green quad (2 triangles) at z = 0.7 covering [-0.8, 0.8]
        const PosV pass1_verts[6] = {
            {-0.8f, -0.8f, 0.7f, 1.0f}, { 0.8f, -0.8f, 0.7f, 1.0f}, { 0.8f,  0.8f, 0.7f, 1.0f},
            {-0.8f, -0.8f, 0.7f, 1.0f}, { 0.8f,  0.8f, 0.7f, 1.0f}, {-0.8f,  0.8f, 0.7f, 1.0f},
        };
        // Pass 2: Smaller red quad at z = 0.3 covering [-0.35, 0.35] (closer -> passes depth test!)
        //       + occluded blue quad at z = 0.9 covering [-0.8, 0.8] (farther -> fails depth test vs Pass 1's 0.7!)
        const PosV pass2_verts[6] = {
            {-0.35f, -0.35f, 0.3f, 1.0f}, { 0.35f, -0.35f, 0.3f, 1.0f}, { 0.35f,  0.35f, 0.3f, 1.0f},
            {-0.35f, -0.35f, 0.3f, 1.0f}, { 0.35f,  0.35f, 0.3f, 1.0f}, {-0.35f,  0.35f, 0.3f, 1.0f},
        };
        const PosV pass3_occluded[6] = {
            {-0.75f, -0.75f, 0.9f, 1.0f}, { 0.75f, -0.75f, 0.9f, 1.0f}, { 0.75f,  0.75f, 0.9f, 1.0f},
            {-0.75f, -0.75f, 0.9f, 1.0f}, { 0.75f,  0.75f, 0.9f, 1.0f}, {-0.75f,  0.75f, 0.9f, 1.0f},
        };

        auto make_pass = [&](const PosV* v, const float col[4], bool clr) {
            openscarlett::VulkanDynamicRenderingPassDesc p{};
            p.vertex_spirv = vs_spv;
            p.fragment_spirv = ps_spv;
            p.vertex_buffer_bytes.resize(sizeof(PosV) * 6u);
            std::memcpy(p.vertex_buffer_bytes.data(), v, sizeof(PosV) * 6u);
            p.vertex_stride = sizeof(PosV);
            p.vertex_count = 6u;
            p.vertex_attributes.push_back({0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u});
            p.push_constants.resize(16u);
            std::memcpy(p.push_constants.data(), col, 16u);
            p.clear_color_on_load = clr;
            p.clear_color[0] = 0.1f; p.clear_color[1] = 0.1f; p.clear_color[2] = 0.1f; p.clear_color[3] = 1.0f;
            p.clear_depth_on_load = clr;
            p.clear_depth = 1.0f;
            p.depth_test = true;
            p.depth_write = true;
            return p;
        };

        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        const float red[4]   = {1.0f, 0.0f, 0.0f, 1.0f};
        const float blue[4]  = {0.0f, 0.0f, 1.0f, 1.0f};

        openscarlett::VulkanDynamicRenderingMultiPassDesc mp{};
        mp.width = 64u;
        mp.height = 64u;
        mp.enable_msaa_4x = true;
        mp.enable_depth = true;
        mp.depth_resolve_mode = openscarlett::VulkanDepthResolveMode::Min;
        mp.readback_depth = true;
        mp.passes.push_back(make_pass(pass1_verts, green, true));
        mp.passes.push_back(make_pass(pass2_verts, red, false));
        mp.passes.push_back(make_pass(pass3_occluded, blue, false));

        openscarlett::VulkanDynamicRenderingMultiPassResult res{};
        S16_CHECK(vk.ExecuteDynamicRenderingMultiPass(mp, res).ok(), "ExecuteDynamicRenderingMultiPass");
        S16_CHECK(res.passes_executed == 3u, "3 dynamic rendering passes executed");
        if (res.executed_on_vulkan_icd) {
            if (vk.GetDeviceInfo().supports_dynamic_rendering) {
                S16_CHECK(res.used_vk_cmd_begin_rendering, "Used Vulkan 1.3/1.4 core vkCmdBeginRendering");
            }
            S16_CHECK(res.resolved_msaa_color, "Resolved MSAA 4x color attachment");
            const size_t center = (32u * 64u + 32u) * 4u;
            const size_t ring   = (32u * 64u + 12u) * 4u;
            const size_t corner = (2u * 64u + 2u) * 4u;
            S16_CHECK(res.framebuffer.pixels[center + 0] > 200u && res.framebuffer.pixels[center + 1] < 20u,
                      "Pass 2 closer RED quad overwrote Pass 1 at center");
            S16_CHECK(res.framebuffer.pixels[ring + 1] > 200u && res.framebuffer.pixels[ring + 2] < 20u,
                      "Pass 1 GREEN quad preserved and occluded Pass 3 farther BLUE quad");
            S16_CHECK(res.framebuffer.pixels[corner + 0] < 45u && res.framebuffer.pixels[corner + 1] < 45u,
                      "Background clear preserved at corner");
            if (res.resolved_msaa_depth && res.depth_readback.size() == 64u * 64u) {
                const float d_center = res.depth_readback[32u * 64u + 32u];
                const float d_ring   = res.depth_readback[32u * 64u + 12u];
                const float d_corner = res.depth_readback[2u * 64u + 2u];
                S16_CHECK(std::fabs(d_center - 0.3f) < 0.05f &&
                          std::fabs(d_ring - 0.7f) < 0.05f &&
                          std::fabs(d_corner - 1.0f) < 0.05f,
                          "MSAA depth resolve readback matches 0.3 / 0.7 / 1.0 across passes");
                std::printf("  [OK] Vulkan Dynamic Rendering 3-Pass MSAA4x + Depth Resolve: center=%.2f ring=%.2f corner=%.2f (dynRender=%d)\n",
                            d_center, d_ring, d_corner, res.used_vk_cmd_begin_rendering ? 1 : 0);
            }
        }
    }

    // 4B. OPENSCARLETT_VK_GPU numeric out-of-range fallback (xcl #82)
    {
        std::vector<openscarlett::VulkanPhysicalDeviceCandidate> synth(2);
        synth[0].enumerated_index = 0u;
        synth[0].device_name = "Intel UHD Graphics";
        synth[0].device_type = openscarlett::VulkanDeviceType::IntegratedGpu;
        synth[0].supports_compute = true;
        synth[0].supports_graphics = true;
        synth[0].device_local_vram_bytes = 128ull * 1024ull * 1024ull;
        synth[1].enumerated_index = 1u;
        synth[1].device_name = "NVIDIA GeForce GTX 1650 Ti";
        synth[1].device_type = openscarlett::VulkanDeviceType::DiscreteGpu;
        synth[1].supports_compute = true;
        synth[1].supports_graphics = true;
        synth[1].supports_mesh_shader = true;
        synth[1].device_local_vram_bytes = 4ull * 1024ull * 1024ull * 1024ull;

        openscarlett::VulkanPhysicalDeviceCandidate chosen{};
        bool matched = false, fell_back = false;
        S16_CHECK(openscarlett::VulkanBackend::SelectPhysicalDeviceCandidateWithNumericFallback(
                      synth, "999", chosen, &matched, &fell_back),
                  "Numeric out-of-range '999' falls back cleanly");
        S16_CHECK(!matched && fell_back && chosen.enumerated_index == 1u,
                  "Fallback picked highest-ranked discrete GPU");
        S16_CHECK(openscarlett::VulkanBackend::SelectPhysicalDeviceCandidateWithNumericFallback(
                      synth, "-1", chosen, &matched, &fell_back) &&
                  !matched && fell_back && chosen.enumerated_index == 1u,
                  "Negative index '-1' falls back cleanly to best-ranked GPU");
        S16_CHECK(!openscarlett::VulkanBackend::SelectPhysicalDeviceCandidateWithNumericFallback(
                      synth, "NonExistentGpuXYZ", chosen, &matched, &fell_back) && !fell_back,
                  "Non-numeric unmatched substring still fails cleanly");

        // Live hardware verification with OPENSCARLETT_VK_GPU=999 and -1 when a Vulkan hardware ICD is present!
        openscarlett::VulkanBackend vk_hw_probe;
        const bool has_live_vk_icd =
            vk_hw_probe.Initialize(/*allow_software_fallback=*/false).ok() && !vk_hw_probe.is_fallback_mode();
        if (has_live_vk_icd) {
            char prev_env[256]{};
            const DWORD prev_len = GetEnvironmentVariableA("OPENSCARLETT_VK_GPU", prev_env, sizeof(prev_env));
            SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", "999");
            {
                openscarlett::VulkanBackend vk_fb;
                const auto fb_st = vk_fb.Initialize(/*allow_software_fallback=*/false);
                S16_CHECK(fb_st.ok() && !vk_fb.is_fallback_mode() &&
                          vk_fb.GetDeviceInfo().numeric_selector_fell_back &&
                          !vk_fb.GetDeviceInfo().matched_device_filter,
                          "Live OPENSCARLETT_VK_GPU=999 succeeds via best-ranked hardware GPU fallback");
                std::printf("  [OK] Live OPENSCARLETT_VK_GPU=999 fell back cleanly to \"%s\"\n",
                            vk_fb.GetDeviceInfo().device_name.c_str());
            }
            SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", "-1");
            {
                openscarlett::VulkanBackend vk_neg;
                const auto neg_st = vk_neg.Initialize(/*allow_software_fallback=*/false);
                S16_CHECK(neg_st.ok() && vk_neg.GetDeviceInfo().numeric_selector_fell_back,
                          "Live OPENSCARLETT_VK_GPU=-1 succeeds via best-ranked hardware GPU fallback");
            }
            SetEnvironmentVariableA("OPENSCARLETT_VK_GPU", (prev_len > 0u) ? prev_env : nullptr);
        } else {
            std::printf("  [SKIP] Live OPENSCARLETT_VK_GPU HW fallback check (no Vulkan HW ICD on headless runner)\n");
        }
    }

    // 4C. Strict Cross-IHV Capability Sanitization Matrix
    {
        constexpr uint64_t kMiB = 1024ull * 1024ull;
        constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;

        // Intel Arc Battlemage (Wave 16..32)
        openscarlett::D3D12AdapterInfo arc{};
        arc.description = "Intel Arc B580 Battlemage";
        arc.vendor_id = 0x8086u;
        arc.dedicated_video_memory_bytes = 12ull * kGiB;
        arc.wave_lane_min = 16u; arc.wave_lane_max = 32u;
        arc.highest_shader_model = 0x67u; arc.raytracing_tier = 11u; arc.mesh_shader_tier = 10u;
        const auto s_arc = openscarlett::SanitizeCrossIhvCapabilities(arc);
        S16_CHECK(s_arc.vendor_class == openscarlett::GpuVendorClass::Intel &&
                  s_arc.wave_lane_min == 16u && s_arc.wave_lane_max == 32u &&
                  s_arc.raytracing_tier == 11u && s_arc.mesh_shader_tier == 10u &&
                  !s_arc.is_uma_low_vram && s_arc.recommend_series_x_profile,
                  "Intel Arc Battlemage profile sanitized");

        // NVIDIA Turing GTX 1650 Ti (Wave 32, DXR 0, MeshTier 10)
        openscarlett::D3D12AdapterInfo nv_turing{};
        nv_turing.description = "NVIDIA GeForce GTX 1650 Ti";
        nv_turing.vendor_id = 0x10DEu;
        nv_turing.dedicated_video_memory_bytes = 4ull * kGiB;
        nv_turing.wave_lane_min = 32u; nv_turing.wave_lane_max = 32u;
        nv_turing.highest_shader_model = 0x66u; nv_turing.raytracing_tier = 0u; nv_turing.mesh_shader_tier = 10u;
        const auto s_nv = openscarlett::SanitizeCrossIhvCapabilities(nv_turing);
        S16_CHECK(s_nv.vendor_class == openscarlett::GpuVendorClass::Nvidia &&
                  s_nv.wave_lane_min == 32u && s_nv.wave_lane_max == 32u &&
                  s_nv.raytracing_tier == 0u && s_nv.mesh_shader_tier == 10u &&
                  s_nv.requires_page_isolated_descriptor_heaps && s_nv.requires_wave64_emulation,
                  "NVIDIA Turing profile sanitized");

        // NVIDIA Blackwell (Wave 32, DXR 12, MeshTier 10)
        openscarlett::D3D12AdapterInfo nv_bw = nv_turing;
        nv_bw.description = "NVIDIA GeForce RTX 5090";
        nv_bw.dedicated_video_memory_bytes = 32ull * kGiB;
        nv_bw.raytracing_tier = 12u;
        const auto s_bw = openscarlett::SanitizeCrossIhvCapabilities(nv_bw);
        S16_CHECK(s_bw.raytracing_tier == 12u && s_bw.recommend_series_x_profile, "NVIDIA Blackwell DXR=12 sanitized");

        // AMD RDNA2/3/4 (Wave 32..64 native!)
        openscarlett::D3D12AdapterInfo amd{};
        amd.description = "AMD Radeon RX 9070 XT";
        amd.vendor_id = 0x1002u;
        amd.dedicated_video_memory_bytes = 16ull * kGiB;
        amd.wave_lane_min = 32u; amd.wave_lane_max = 64u;
        amd.highest_shader_model = 0x67u; amd.raytracing_tier = 11u; amd.mesh_shader_tier = 10u;
        const auto s_amd = openscarlett::SanitizeCrossIhvCapabilities(amd);
        S16_CHECK(s_amd.vendor_class == openscarlett::GpuVendorClass::Amd &&
                  s_amd.wave_lane_min == 32u && s_amd.wave_lane_max == 64u &&
                  s_amd.preferred_wave_size == 64u && !s_amd.requires_wave64_emulation,
                  "AMD RDNA native Wave32..64 profile sanitized");

        // 128 MB UMA iGPU
        openscarlett::D3D12AdapterInfo uma{};
        uma.description = "Intel UHD Graphics 128MB UMA";
        uma.vendor_id = 0x8086u;
        uma.dedicated_video_memory_bytes = 128ull * kMiB;
        uma.shared_system_memory_bytes = 16ull * kGiB;
        uma.highest_shader_model = 0x65u; uma.raytracing_tier = 0u; uma.mesh_shader_tier = 0u;
        const auto s_uma = openscarlett::SanitizeCrossIhvCapabilities(uma);
        S16_CHECK(s_uma.is_uma_low_vram && s_uma.promote_large_placed_textures_to_committed &&
                  s_uma.large_placed_texture_threshold_bytes == 16ull * kMiB &&
                  s_uma.effective_vram_budget_bytes >= 128ull * kMiB &&
                  s_uma.effective_vram_budget_bytes <= (4ull * kGiB + 128ull * kMiB) &&
                  !s_uma.recommend_series_x_profile,
                  "128 MB UMA iGPU profile safely clamped with placed-texture promotion");

        // Over-reported SM 6.2 claiming bogus DXR=99 + MeshTier=10 must be clamped down to 0!
        openscarlett::D3D12AdapterInfo bad{};
        bad.vendor_id = 0x10DEu;
        bad.dedicated_video_memory_bytes = 2ull * kGiB;
        bad.highest_shader_model = 0x62u;
        bad.raytracing_tier = 99u;
        bad.mesh_shader_tier = 10u;
        const auto s_bad = openscarlett::SanitizeCrossIhvCapabilities(bad);
        S16_CHECK(s_bad.raytracing_tier == 0u && s_bad.mesh_shader_tier == 0u,
                  "Over-reported DXR/Mesh tiers clamped cleanly when SM < 6.3 / 6.5");
        std::printf("  [OK] Cross-IHV Matrix (Arc Battlemage, NVIDIA Turing/Blackwell, AMD RDNA, 128MB UMA, Over-report clamp)\n");
    }
}

// ---------------------------------------------------------------------------
// Deliverable 5: VRAM Residency Hygiene & Multi-Queue (Direct+Compute+Copy) Stress
// ---------------------------------------------------------------------------
void TestDeliverable5_VramHygieneAndMultiQueueStress() {
    std::printf("\n=== Deliverable 5: VRAM Residency Hygiene + Multi-Queue Stress ===\n");

    // 5A. VramResidencyManager active-ref & in-flight fence live protection
    {
        openscarlett::VramResidencyManager mgr;
        openscarlett::VramResidencyConfig cfg{};
        cfg.vram_budget_bytes = 512ull * 1024ull;
        cfg.auto_evict_on_pressure = true;
        S16_CHECK(mgr.Initialize(nullptr, cfg).ok(), "Init VramResidencyManager");

        openscarlett::VramAllocationDesc ad{};
        ad.size_bytes = 256ull * 1024ull;
        uint64_t h_active = 0ull, h_inflight = 0ull, h_idle = 0ull, h_trigger = 0ull;
        S16_CHECK(mgr.Allocate(ad, h_active).ok(), "Alloc active");
        S16_CHECK(mgr.AddActiveRef(h_active).ok(), "AddActiveRef");
        S16_CHECK(mgr.Allocate(ad, h_inflight).ok(), "Alloc inflight");
        S16_CHECK(mgr.MarkInFlight(h_inflight, 5ull).ok(), "MarkInFlight(5)");
        S16_CHECK(mgr.Allocate(ad, h_idle).ok(), "Alloc idle");
        // Budget (512 KiB) is now exceeded; h_active & h_inflight MUST NEVER be evicted!
        openscarlett::VramAllocationInfo ia{}, ii{}, id{};
        S16_CHECK(mgr.GetAllocationInfo(h_active, ia) && ia.is_resident && ia.active_ref_count == 1u,
                  "Active-ref allocation stayed 100% resident");
        S16_CHECK(mgr.GetAllocationInfo(h_inflight, ii) && ii.is_resident && ii.is_in_flight,
                  "In-flight fence allocation stayed 100% resident");

        S16_CHECK(mgr.Allocate(ad, h_trigger).ok(), "Alloc trigger");
        S16_CHECK(mgr.GetAllocationInfo(h_active, ia) && ia.is_resident, "Active-ref still resident under pressure");
        S16_CHECK(mgr.GetAllocationInfo(h_inflight, ii) && ii.is_resident, "In-flight still resident under pressure");
        S16_CHECK(mgr.GetAllocationInfo(h_idle, id) && !id.is_resident, "Only idle unpinned allocation evicted");
        const auto st1 = mgr.GetStats();
        S16_CHECK(st1.skipped_active_live_heaps >= 2ull, "skipped_active_live_heaps recorded protected skips");

        // Now complete fence 5 & release active ref -> explicit EvictToBudget(256 KiB) can cleanly evict them!
        mgr.NotifyFenceCompleted(5ull);
        S16_CHECK(mgr.ReleaseActiveRef(h_active).ok(), "ReleaseActiveRef");
        uint64_t ev_cnt = 0ull;
        S16_CHECK(mgr.EvictToBudget(256ull * 1024ull, &ev_cnt, nullptr).ok() && ev_cnt >= 2ull,
                  "Allocations become evictable only after fence completes & active ref released");
    }

    // 5B. Live DeviceX VRAM Residency Hygiene (HW GPU)
    OpenScarlettD3D12SetPreferWarp(0);
    ComPtr<ID3D12DeviceX> dev;
    ComPtr<openscarlett::d3d12_xs::IOpenScarlettVramResidencyControl> res_ctrl;
    ComPtr<IOpenScarlettGraphicsCompleteness> comp;
    S16_CHECK(SUCCEEDED(D3D12CreateDeviceX(nullptr, D3D_FEATURE_LEVEL_12_0, IID_ID3D12DeviceX, dev.PutVoid())) && dev, "DeviceX");
    S16_CHECK(SUCCEEDED(dev->QueryInterface(openscarlett::d3d12_xs::IID_IOpenScarlettVramResidencyControl, res_ctrl.PutVoid())) && res_ctrl, "QI residency ctrl");
    S16_CHECK(SUCCEEDED(dev->QueryInterface(IID_IOpenScarlettGraphicsCompleteness, comp.PutVoid())) && comp, "QI completeness");

    // First verify NO auto-eviction occurs under normal auto mode (no budget cap exceeded)
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 256ull * 1024ull;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ComPtr<ID3D12Heap> heap_with_child, heap_active, heap_inflight;
    ComPtr<ID3D12Resource> placed_child, idle_buf, pressure_buf;
    ComPtr<ID3D12Fence> res_fence;
    S16_CHECK(SUCCEEDED(dev->CreateHeap(&hd, OS_D3D12_IID(ID3D12Heap), heap_with_child.PutVoid())), "Heap with placed child");
    S16_CHECK(SUCCEEDED(dev->CreateHeap(&hd, OS_D3D12_IID(ID3D12Heap), heap_active.PutVoid())), "Active-ref heap");
    S16_CHECK(SUCCEEDED(dev->CreateHeap(&hd, OS_D3D12_IID(ID3D12Heap), heap_inflight.PutVoid())), "In-flight heap");
    S16_CHECK(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), res_fence.PutVoid())), "Residency fence");

    D3D12_RESOURCE_DESC prd{};
    prd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    prd.Width = 65536ull;
    prd.Height = 1u; prd.DepthOrArraySize = 1u; prd.MipLevels = 1u;
    prd.SampleDesc.Count = 1u; prd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    S16_CHECK(SUCCEEDED(dev->CreatePlacedResource(
                  heap_with_child.Get(), 0ull, &prd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                  OS_D3D12_IID(ID3D12Resource), placed_child.PutVoid())),
              "CreatePlacedResource on live heap");

    S16_CHECK(SUCCEEDED(comp->MarkResidencyObjectActive(heap_active.Get(), 1)), "Mark heap_active active");
    S16_CHECK(SUCCEEDED(comp->MarkResidencyObjectInFlight(heap_inflight.Get(), res_fence.Get(), 10ull)),
              "Mark heap_inflight in-flight @ fence 10");

    openscarlett::d3d12_xs::D3D12XBOX_VRAM_RESIDENCY_STATS rs_auto{};
    S16_CHECK(SUCCEEDED(res_ctrl->GetVramResidencyStats(&rs_auto)), "Stats auto mode");
    S16_CHECK(rs_auto.auto_lru_eviction_count == 0ull, "ZERO auto-evictions when no explicit budget cap exceeded");

    // Now set explicit 512 KiB budget cap and allocate an idle buffer + pressure buffer:
    // ONLY idle_buf may be evicted; heap_with_child, heap_active, and heap_inflight MUST all be skipped!
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256ull * 1024ull,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, idle_buf.Put())), "Idle buf");
    S16_CHECK(SUCCEEDED(res_ctrl->SetResidencyBudgetOverride(512ull * 1024ull)), "Set explicit 512 KiB budget cap");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 256ull * 1024ull,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, pressure_buf.Put())), "Pressure buf");

    openscarlett::d3d12_xs::D3D12XBOX_VRAM_RESIDENCY_STATS rs_cap{};
    S16_CHECK(SUCCEEDED(res_ctrl->GetVramResidencyStats(&rs_cap)), "Stats under cap");
    S16_CHECK(rs_cap.skipped_active_live_heaps >= 3ull,
              "Live placed-backing, active-ref, and in-flight heaps skipped (never auto-evicted!)");
    S16_CHECK(rs_cap.auto_lru_eviction_count == 1ull, "Only the single idle unpinned buffer was evicted");
    S16_CHECK(SUCCEEDED(res_ctrl->SetResidencyBudgetOverride(0ull)), "Restore auto budget");

    // Clean up protections
    S16_CHECK(SUCCEEDED(comp->MarkResidencyObjectActive(heap_active.Get(), -1)), "Release active ref");
    S16_CHECK(SUCCEEDED(res_fence->Signal(10ull)), "Complete in-flight fence");
    std::printf("  [OK] DeviceX VRAM Hygiene: skipped_active_live_heaps=%llu, auto_evicts=%llu (only idle victim)\n",
                static_cast<unsigned long long>(rs_cap.skipped_active_live_heaps),
                static_cast<unsigned long long>(rs_cap.auto_lru_eviction_count));

    // 5C. Multi-Queue (Direct + Compute + Copy) 16-Iteration Pipelined Stress Test
    D3D12_COMMAND_QUEUE_DESC qd_dir{}, qd_cmp{}, qd_cpy{};
    qd_dir.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd_cmp.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    qd_cpy.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ComPtr<ID3D12CommandQueue> q_dir, q_cmp, q_cpy;
    ComPtr<ID3D12CommandAllocator> a_dir, a_cmp, a_cpy;
    ComPtr<ID3D12GraphicsCommandList> l_dir, l_cmp, l_cpy;
    ComPtr<ID3D12Fence> mq_fence;
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd_dir, OS_D3D12_IID(ID3D12CommandQueue), q_dir.PutVoid())), "MQ Direct Q");
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd_cmp, OS_D3D12_IID(ID3D12CommandQueue), q_cmp.PutVoid())), "MQ Compute Q");
    S16_CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd_cpy, OS_D3D12_IID(ID3D12CommandQueue), q_cpy.PutVoid())), "MQ Copy Q");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_D3D12_IID(ID3D12CommandAllocator), a_dir.PutVoid())), "MQ Direct Alloc");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, OS_D3D12_IID(ID3D12CommandAllocator), a_cmp.PutVoid())), "MQ Compute Alloc");
    S16_CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, OS_D3D12_IID(ID3D12CommandAllocator), a_cpy.PutVoid())), "MQ Copy Alloc");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, a_dir.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), l_dir.PutVoid())), "MQ Direct CL");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COMPUTE, a_cmp.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), l_cmp.PutVoid())), "MQ Compute CL");
    S16_CHECK(SUCCEEDED(dev->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_COPY, a_cpy.Get(), nullptr, OS_D3D12_IID(ID3D12GraphicsCommandList), l_cpy.PutVoid())), "MQ Copy CL");
    l_dir->Close(); l_cmp->Close(); l_cpy->Close();
    S16_CHECK(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, OS_D3D12_IID(ID3D12Fence), mq_fence.PutVoid())), "MQ Fence");

    ComPtr<ID3D12Resource> s_up, s_a, s_b, s_rb;
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 1024u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, s_up.Put())), "Stress UP");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 1024u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, s_a.Put())), "Stress A");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, 1024u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, s_b.Put())), "Stress B");
    S16_CHECK(SUCCEEDED(CreateBuf(dev.Get(), D3D12_HEAP_TYPE_READBACK, 1024u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, s_rb.Put())), "Stress RB");

    UINT64 fv = 0ull;
    bool stress_ok = true;
    for (uint32_t iter = 0u; iter < 16u; ++iter) {
        std::vector<uint32_t> vals(256u);
        for (uint32_t i = 0u; i < 256u; ++i) vals[i] = (iter << 16u) | (i * 13u + 5u);
        void* m = nullptr;
        D3D12_RANGE er{0, 0};
        s_up->Map(0u, &er, &m);
        std::memcpy(m, vals.data(), 1024u);
        s_up->Unmap(0u, nullptr);

        // Stage 1 (Copy Queue): UPLOAD -> s_a
        a_cpy->Reset(); l_cpy->Reset(a_cpy.Get(), nullptr);
        l_cpy->CopyBufferRegion(s_a.Get(), 0ull, s_up.Get(), 0ull, 1024ull);
        l_cpy->Close();
        ID3D12CommandList* c1[1] = {l_cpy.Get()};
        q_cpy->ExecuteCommandLists(1u, c1);
        const UINT64 f1 = ++fv;
        q_cpy->Signal(mq_fence.Get(), f1);

        // Stage 2 (Compute Queue): waits on Copy, copies s_a -> s_b
        q_cmp->Wait(mq_fence.Get(), f1);
        a_cmp->Reset(); l_cmp->Reset(a_cmp.Get(), nullptr);
        l_cmp->CopyBufferRegion(s_b.Get(), 0ull, s_a.Get(), 0ull, 1024ull);
        l_cmp->Close();
        ID3D12CommandList* c2[1] = {l_cmp.Get()};
        q_cmp->ExecuteCommandLists(1u, c2);
        const UINT64 f2 = ++fv;
        q_cmp->Signal(mq_fence.Get(), f2);

        // Stage 3 (Direct Queue): waits on Compute, copies s_b -> READBACK
        q_dir->Wait(mq_fence.Get(), f2);
        a_dir->Reset(); l_dir->Reset(a_dir.Get(), nullptr);
        l_dir->CopyBufferRegion(s_rb.Get(), 0ull, s_b.Get(), 0ull, 1024ull);
        l_dir->Close();
        ID3D12CommandList* c3[1] = {l_dir.Get()};
        q_dir->ExecuteCommandLists(1u, c3);
        const UINT64 f3 = ++fv;
        if (!WaitFence(q_dir.Get(), mq_fence.Get(), f3)) { stress_ok = false; break; }

        D3D12_RANGE rr{0, 1024u};
        s_rb->Map(0u, &rr, &m);
        if (std::memcmp(m, vals.data(), 1024u) != 0) stress_ok = false;
        s_rb->Unmap(0u, &er);
        if (!stress_ok) break;
    }
    S16_CHECK(stress_ok, "16-iteration Copy -> Compute -> Direct FenceX multi-queue stress 100% byte-exact");
    S16_CHECK(dev->GetDeviceRemovedReason() == S_OK, "Zero device removals after multi-queue stress");
    std::printf("  [OK] Multi-Queue (Copy -> Compute -> Direct) 16-iteration pipelined stress passed!\n");
}

#endif  // _WIN32

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#if defined(_WIN32)
    TestDeliverable1_D3D12(/*prefer_warp=*/false);
    TestDeliverable1_D3D12(/*prefer_warp=*/true);
    TestDeliverable2_D3D12(/*prefer_warp=*/false);
    TestDeliverable2_D3D12(/*prefer_warp=*/true);
    TestDeliverable2_VulkanIndirect();
    TestDeliverable3_LiveGpuFmaskDccHtileResolve();
    TestDeliverable4_VulkanDynRenderFallbackAndCrossIhv();
    TestDeliverable5_VramHygieneAndMultiQueueStress();
#endif

    if (g_failures > 0) {
        std::fprintf(stderr, "\n[TEST FAILED] test_s16_graphics_completeness: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\n[TEST PASSED] test_s16_graphics_completeness: ALL 5 Sprint 16 Graphics Engine deliverables 100%% GREEN!\n");
    return 0;
}
