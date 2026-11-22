#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "openscarlett/gpu/vulkan_backend.h"
#include "openscarlett/gpu/xg_xs_abi.h"
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

uint32_t F32Bits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16u;
    const uint32_t exp = (static_cast<uint32_t>(h) >> 10u) & 0x1Fu;
    const uint32_t mant = static_cast<uint32_t>(h) & 0x03FFu;
    uint32_t bits = 0u;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;
        } else {
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x0400u) == 0u) {
                m <<= 1u;
                --e;
            }
            m &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(e + 127) << 23u) | (m << 13u);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13u);
    } else {
        bits = sign | ((exp + 112u) << 23u) | (mant << 13u);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void WaitForQueueIdle(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    TestComPtr<ID3D12Fence> fence;
    OS_REQUIRE(SUCCEEDED(dev->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.Put()))));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    OS_REQUIRE(ev != nullptr);
    OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), 1ull)));
    if (fence->GetCompletedValue() < 1ull) {
        OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1ull, ev)));
        OS_REQUIRE(WaitForSingleObject(ev, 15000u) == WAIT_OBJECT_0);
    }
    CloseHandle(ev);
}

bool CompileHlslFx(const char* src, const char* entry, const char* target,
                   TestComPtr<ID3DBlob>& out_blob) {
    TestComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(
        src, std::strlen(src), nullptr, nullptr, nullptr,
        entry, target, 0u, 0u, out_blob.Put(), err.Put());
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "D3DCompile error (%s/%s): %.*s\n",
                         entry, target,
                         static_cast<int>(err->GetBufferSize()),
                         static_cast<const char*>(err->GetBufferPointer()));
        }
        return false;
    }
    return out_blob.Get() != nullptr;
}

bool HasHardwareD3D12Gpu(openscarlett::D3D12AdapterInfo& out_info) {
    openscarlett::D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    TestComPtr<ID3D12Device> dev;
    TestComPtr<IDXGIAdapter1> adapter;
    const auto st = openscarlett::SelectD3D12AdapterAndCreateDevice(
        opts, dev.Put(), adapter.Put(), &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

// ---------------------------------------------------------------------------
// Minimal SPIR-V 1.3 builder for separate T# (SampledImage) + S# (Sampler)
// mip-level selection verification on Vulkan ICD & SW fallback.
// ---------------------------------------------------------------------------
struct SpvBuilder {
    std::vector<uint32_t> words;
    uint32_t bound = 1;

    uint32_t Id() { return bound++; }

    void Op(uint16_t opcode, std::initializer_list<uint32_t> args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | opcode);
        for (uint32_t a : args) words.push_back(a);
    }

    void OpVec(uint16_t opcode, const std::vector<uint32_t>& args) {
        words.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | opcode);
        words.insert(words.end(), args.begin(), args.end());
    }

    static void PushStr(std::vector<uint32_t>& dst, const char* s) {
        const size_t n = std::strlen(s) + 1;
        const size_t nw = (n + 3) / 4;
        for (size_t w = 0; w < nw; ++w) {
            uint32_t v = 0;
            for (size_t b = 0; b < 4; ++b) {
                const size_t idx = w * 4 + b;
                if (idx < n) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[idx])) << (b * 8);
            }
            dst.push_back(v);
        }
    }

    std::vector<uint32_t> Finish(uint32_t spv_version) const {
        std::vector<uint32_t> out;
        out.reserve(5 + words.size());
        out.insert(out.end(), {0x07230203u, spv_version, 0x4F533200u, bound, 0u});
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

// Fullscreen triangle VS (no VB needed; passes UV = (0.5, 0.5) to PS).
std::vector<uint32_t> BuildFullscreenPassVertexSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id(), t_v4 = b.Id();
    const uint32_t t_in_v4 = b.Id(), t_out_v4 = b.Id(), t_pv = b.Id(), t_out_pv = b.Id();
    const uint32_t v_in_pos = b.Id(), v_out_pv = b.Id(), v_out_uv = b.Id();
    const uint32_t c_i0 = b.Id(), c_half = b.Id(), c_one = b.Id(), c_uv = b.Id();
    const uint32_t fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});     // OpCapability Shader
    b.Op(14, {0, 1});  // OpMemoryModel Logical GLSL450
    {
        std::vector<uint32_t> ep = {0, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in_pos, v_out_pv, v_out_uv});
        b.OpVec(15, ep);
    }
    b.Op(71, {v_in_pos, 30, 0});
    b.Op(71, {v_out_uv, 30, 0});
    b.Op(71, {t_pv, 2});
    b.Op(72, {t_pv, 0, 11, 0});

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
    b.Op(43, {t_f32, c_half, F32Bits(0.5f)});
    b.Op(43, {t_f32, c_one, F32Bits(1.0f)});
    b.Op(44, {t_v4, c_uv, c_half, c_half, c_one, c_one});
    b.Op(59, {t_in_v4, v_in_pos, 1});
    b.Op(59, {t_out_pv, v_out_pv, 3});
    b.Op(59, {t_out_v4, v_out_uv, 3});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t pos = b.Id(), ptr_pos = b.Id();
    b.Op(61, {t_v4, pos, v_in_pos});
    b.Op(65, {t_out_v4, ptr_pos, v_out_pv, c_i0});
    b.Op(62, {ptr_pos, pos});
    b.Op(62, {v_out_uv, c_uv});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// Separate T# (SampledImage binding 0) + S# (Sampler binding 1) + PushConstant { vec4 lod_param }
// Fragment shader: samples texture(sampler2D(tex, samp), uv, lod = lod_param.x).
std::vector<uint32_t> BuildSeparateSamplerMipFragmentSpirv() {
    SpvBuilder b;
    const uint32_t t_void = b.Id(), t_fn = b.Id(), t_f32 = b.Id(), t_i32 = b.Id();
    const uint32_t t_v2 = b.Id(), t_v4 = b.Id(), t_in_v4 = b.Id(), t_out_v4 = b.Id();
    const uint32_t t_img = b.Id(), t_samp = b.Id(), t_samp_img = b.Id();
    const uint32_t t_uc_img = b.Id(), t_uc_samp = b.Id();
    const uint32_t t_pc = b.Id(), t_ptr_pc = b.Id(), t_ptr_pc_v4 = b.Id();
    const uint32_t v_in = b.Id(), v_out = b.Id(), v_img = b.Id(), v_samp = b.Id(), v_pc = b.Id();
    const uint32_t c_i0 = b.Id(), fn_main = b.Id(), lbl = b.Id();

    b.Op(17, {1});
    b.Op(14, {0, 1});
    {
        std::vector<uint32_t> ep = {4, fn_main};
        SpvBuilder::PushStr(ep, "main");
        ep.insert(ep.end(), {v_in, v_out});
        b.OpVec(15, ep);
    }
    b.Op(16, {fn_main, 7});       // OriginUpperLeft
    b.Op(71, {v_in, 30, 0});      // Location 0
    b.Op(71, {v_out, 30, 0});     // Location 0
    b.Op(71, {v_img, 34, 0});     // Set 0
    b.Op(71, {v_img, 33, 0});     // Binding 0 = T# SampledImage
    b.Op(71, {v_samp, 34, 0});    // Set 0
    b.Op(71, {v_samp, 33, 1});    // Binding 1 = S# Sampler
    b.Op(71, {t_pc, 2});          // Block
    b.Op(72, {t_pc, 0, 35, 0});   // Offset 0

    b.Op(19, {t_void});
    b.Op(33, {t_fn, t_void});
    b.Op(22, {t_f32, 32});
    b.Op(21, {t_i32, 32, 1});
    b.Op(23, {t_v2, t_f32, 2});
    b.Op(23, {t_v4, t_f32, 4});
    b.Op(32, {t_in_v4, 1, t_v4});
    b.Op(32, {t_out_v4, 3, t_v4});
    b.Op(25, {t_img, t_f32, 1, 0, 0, 0, 1, 0});  // 2D sampled float image
    b.Op(26, {t_samp});                          // OpTypeSampler
    b.Op(27, {t_samp_img, t_img});               // OpTypeSampledImage
    b.Op(32, {t_uc_img, 0, t_img});
    b.Op(32, {t_uc_samp, 0, t_samp});
    b.Op(30, {t_pc, t_v4});
    b.Op(32, {t_ptr_pc, 9, t_pc});
    b.Op(32, {t_ptr_pc_v4, 9, t_v4});

    b.Op(43, {t_i32, c_i0, 0});
    b.Op(59, {t_in_v4, v_in, 1});
    b.Op(59, {t_out_v4, v_out, 3});
    b.Op(59, {t_uc_img, v_img, 0});
    b.Op(59, {t_uc_samp, v_samp, 0});
    b.Op(59, {t_ptr_pc, v_pc, 9});

    b.Op(54, {t_void, fn_main, 0, t_fn});
    b.Op(248, {lbl});
    const uint32_t in_v = b.Id(), u = b.Id(), v = b.Id(), uv = b.Id();
    const uint32_t img_val = b.Id(), samp_val = b.Id(), combined = b.Id();
    const uint32_t p_pc = b.Id(), pc_val = b.Id(), lod = b.Id(), texel = b.Id();

    b.Op(61, {t_v4, in_v, v_in});
    b.Op(81, {t_f32, u, in_v, 0});
    b.Op(81, {t_f32, v, in_v, 1});
    b.Op(80, {t_v2, uv, u, v});
    b.Op(61, {t_img, img_val, v_img});
    b.Op(61, {t_samp, samp_val, v_samp});
    b.Op(86, {t_samp_img, combined, img_val, samp_val});   // OpSampledImage (T# + S#)
    b.Op(65, {t_ptr_pc_v4, p_pc, v_pc, c_i0});
    b.Op(61, {t_v4, pc_val, p_pc});
    b.Op(81, {t_f32, lod, pc_val, 0});
    b.Op(88, {t_v4, texel, combined, uv, 2, lod});         // OpImageSampleExplicitLod Lod=%lod
    b.Op(62, {v_out, texel});
    b.Op(253, {});
    b.Op(56, {});
    return b.Finish(0x00010300u);
}

// ---------------------------------------------------------------------------
// Test 1: R9G9B9E5_SHAREDEXP UAV Texture Fallback to R16G16B16A16_FLOAT
// ---------------------------------------------------------------------------
void TestR9G9B9E5UavFallback(ID3D12DeviceX* dev, ID3D12CommandQueueX* queue,
                             const char* mode_label, bool force_fallback) {
    std::printf("[Test 1 - %s] R9G9B9E5_SHAREDEXP UAV -> R16G16B16A16_FLOAT fallback (force=%d)...\n",
                mode_label, force_fallback ? 1 : 0);

    OpenScarlettD3D12ResetTelemetry();
    OpenScarlettD3D12SetForceR9G9B9E5UavFallback(force_fallback ? 1 : 0);

    int fallback_active = 0;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12GetR9G9B9E5UavFallbackActive(dev, &fallback_active)));
    D3D12_FEATURE_DATA_FORMAT_SUPPORT host_sup{};
    host_sup.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    OS_REQUIRE(SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &host_sup, sizeof(host_sup))));
    const bool native_uav =
        ((host_sup.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0) &&
        ((host_sup.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0);
    std::printf("  Host native R9G9B9E5 typed UAV=%s | FallbackActive=%d\n",
                native_uav ? "yes" : "no", fallback_active);
    if (force_fallback || !native_uav) {
        OS_REQUIRE(fallback_active == 1);
    }

    constexpr uint32_t kW = 16u;
    constexpr uint32_t kH = 16u;
    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kW;
    tex_desc.Height = kH;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 1u;
    tex_desc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    TestComPtr<ID3D12Resource> uav_tex;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(uav_tex.Put()))));
    OS_REQUIRE(uav_tex.Get() != nullptr);

    const D3D12_RESOURCE_DESC created_desc = uav_tex->GetDesc();
    if (fallback_active) {
        OS_REQUIRE(created_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        int32_t slot = -1;
        uint64_t calls = 0;
        OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry(
                       "ID3D12DeviceX::R9G9B9E5UavFallback", &slot, &calls) == 1);
        OS_REQUIRE(calls >= 1ull);
    }

    // Create explicit R9G9B9E5 UAV and SRV descriptors (DeviceX must rewrite both cleanly!)
    D3D12_DESCRIPTOR_HEAP_DESC dh_desc{};
    dh_desc.NumDescriptors = 2u;
    dh_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    TestComPtr<ID3D12DescriptorHeap> dh;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dh_desc, IID_PPV_ARGS(dh.Put()))));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = dh->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu1{cpu0.ptr + inc};

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(uav_tex.Get(), nullptr, &uav_desc, cpu0);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1u;
    dev->CreateShaderResourceView(uav_tex.Get(), &srv_desc, cpu1);

    // Execute live compute shader writing HDR values (1.5, 0.5, 2.0, 1.0) into the UAV!
    static const char* kCsWriteHdr = R"(
        RWTexture2D<float4> g_out : register(u0);
        [numthreads(8, 8, 1)]
        void main(uint3 tid : SV_DispatchThreadID) {
            g_out[tid.xy] = float4(1.5, 0.5, 2.0, 1.0);
        }
    )";
    TestComPtr<ID3DBlob> cs_blob;
    OS_REQUIRE(CompileHlslFx(kCsWriteHdr, "main", "cs_5_0", cs_blob));

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1u;
    range.BaseShaderRegister = 0u;
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1u;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1u;
    rs_desc.pParameters = &param;
    TestComPtr<ID3DBlob> rs_blob, rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    TestComPtr<ID3D12RootSignature> root_sig;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(root_sig.Put()))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_sig.Get();
    pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
    TestComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso.Put()))));

    // Readback using DeviceX::GetCopyableFootprints (also tests R9G9B9E5 footprint rewrite!)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0u;
    UINT64 row_bytes = 0ull, total_bytes = 0ull;
    dev->GetCopyableFootprints(&tex_desc, 0u, 1u, 0ull, &fp, &num_rows, &row_bytes, &total_bytes);
    OS_REQUIRE(total_bytes > 0ull);
    if (fallback_active) {
        OS_REQUIRE(fp.Footprint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = total_bytes;
    rb_desc.Height = 1u;
    rb_desc.DepthOrArraySize = 1u;
    rb_desc.MipLevels = 1u;
    rb_desc.SampleDesc.Count = 1u;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    TestComPtr<ID3D12Resource> rb_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb_buf.Put()))));

    TestComPtr<ID3D12CommandAllocator> alloc;
    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(),
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));

    ID3D12DescriptorHeap* heaps[] = {dh.Get()};
    cmd->SetDescriptorHeaps(1u, heaps);
    cmd->SetComputeRootSignature(root_sig.Get());
    cmd->SetComputeRootDescriptorTable(0u, dh->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch(kW / 8u, kH / 8u, 1u);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = uav_tex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1u, &b);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = rb_buf.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = uav_tex.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0u;
    cmd->CopyTextureRegion(&dst_loc, 0u, 0u, 0u, &src_loc, nullptr);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1u, lists);
    WaitForQueueIdle(dev, queue);

    void* mapped = nullptr;
    D3D12_RANGE rr{0u, static_cast<SIZE_T>(total_bytes)};
    OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rr, &mapped)));
    if (created_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto* px = reinterpret_cast<const uint16_t*>(
            static_cast<const uint8_t*>(mapped) + static_cast<size_t>(4u) * fp.Footprint.RowPitch + 4u * 8u);
        const float r = HalfToFloat(px[0]);
        const float g = HalfToFloat(px[1]);
        const float bl = HalfToFloat(px[2]);
        const float a = HalfToFloat(px[3]);
        OS_REQUIRE(std::fabs(r - 1.5f) < 0.02f);
        OS_REQUIRE(std::fabs(g - 0.5f) < 0.02f);
        OS_REQUIRE(std::fabs(bl - 2.0f) < 0.02f);
        OS_REQUIRE(std::fabs(a - 1.0f) < 0.02f);
        std::printf("  Readback HDR pixel (4,4) = (%.2f, %.2f, %.2f, %.2f) [RGBA16F fallback OK]\n", r, g, bl, a);
    }
    D3D12_RANGE wn{0u, 0u};
    rb_buf->Unmap(0u, &wn);
    OpenScarlettD3D12SetForceR9G9B9E5UavFallback(0);
}

// ---------------------------------------------------------------------------
// Test 2: CommandQueueX / CommandListX Xbox methods
//         (KickoffX, Wait32BitValueX, Wait64BitValueX, RSSetDepthBiasX)
// ---------------------------------------------------------------------------
void TestCommandQueueAndListXboxMethods(ID3D12DeviceX* dev, ID3D12CommandQueueX* queue,
                                        const char* mode_label) {
    std::printf("[Test 2 - %s] CommandQueueX & CommandListX Xbox methods...\n", mode_label);
    OpenScarlettD3D12ResetTelemetry();

    TestComPtr<ID3D12CommandAllocator> alloc;
    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));

    // RSSetDepthBiasX via COM interface & C helper + reset clearing check
    cmd->RSSetDepthBiasX(16.0f, 0.125f, 2.5f);
    float db = 0.0f, dbc = 0.0f, sdb = 0.0f;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListGetDepthBiasX(cmd.Get(), &db, &dbc, &sdb)));
    OS_REQUIRE(db == 16.0f && dbc == 0.125f && sdb == 2.5f);

    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListRSSetDepthBiasX(cmd.Get(), -4.0f, 0.5f, 1.25f)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListGetDepthBiasX(cmd.Get(), &db, &dbc, &sdb)));
    OS_REQUIRE(db == -4.0f && dbc == 0.5f && sdb == 1.25f);

    // CommandListX::KickoffX / Wait32BitValueX / Wait64BitValueX inside live command recording
    cmd->KickoffX();
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListKickoffX(cmd.Get())));
    alignas(8) std::atomic<uint32_t> sync32{0u};
    alignas(8) std::atomic<uint64_t> sync64{0ull};
    cmd->Wait32BitValueX(
        static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(&sync32)),
        0x1234u, 0xFFFFu, D3D12XBOX_WAIT_VALUE_OP_EQUAL);
    cmd->Wait64BitValueX(
        static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(&sync64)),
        0xCAFEBABE00000001ull, ~0ull, D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1u, lists);
    OS_REQUIRE(SUCCEEDED(queue->KickoffX()));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12QueueKickoffX(queue)));
    WaitForQueueIdle(dev, queue);

    // Reset clears dynamic depth bias back to 0
    OS_REQUIRE(SUCCEEDED(alloc->Reset()));
    OS_REQUIRE(SUCCEEDED(cmd->Reset(alloc.Get(), nullptr)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12CommandListGetDepthBiasX(cmd.Get(), &db, &dbc, &sdb)));
    OS_REQUIRE(db == 0.0f && dbc == 0.0f && sdb == 0.0f);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    // CommandQueueX::Wait32BitValueX & Wait64BitValueX on live async CPU memory writes
    std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        sync32.store(0x00FF1234u, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        sync64.store(0xCAFEBABE00000005ull, std::memory_order_release);
    });
    OS_REQUIRE(SUCCEEDED(queue->Wait32BitValueX(
        static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(&sync32)),
        0x00001234u, 0x0000FFFFu, D3D12XBOX_WAIT_VALUE_OP_EQUAL)));
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12QueueWait64BitValueX(
        queue,
        static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(reinterpret_cast<uintptr_t>(&sync64)),
        0xCAFEBABE00000001ull, ~0ull, D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL)));
    writer.join();

    // Also verify GPU-only virtual address (non-CPU pointer) synchronizes cleanly via fence
    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = 4096ull;
    buf_desc.Height = 1u;
    buf_desc.DepthOrArraySize = 1u;
    buf_desc.MipLevels = 1u;
    buf_desc.SampleDesc.Count = 1u;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    TestComPtr<ID3D12Resource> gpu_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(gpu_buf.Put()))));
    const D3D12_GPU_VIRTUAL_ADDRESS gpu_va = gpu_buf->GetGPUVirtualAddress();
    OS_REQUIRE(gpu_va != 0ull);
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12QueueWait32BitValueX(
        queue, gpu_va, 1u, ~0u, D3D12XBOX_WAIT_VALUE_OP_EQUAL)));
    OS_REQUIRE(SUCCEEDED(queue->Wait64BitValueX(
        gpu_va, 1ull, ~0ull, D3D12XBOX_WAIT_VALUE_OP_GREATER_EQUAL)));

    // Null address must fail cleanly with E_INVALIDARG
    OS_REQUIRE(queue->Wait32BitValueX(0ull, 0u, ~0u, D3D12XBOX_WAIT_VALUE_OP_EQUAL) == E_INVALIDARG);
    OS_REQUIRE(queue->Wait64BitValueX(0ull, 0ull, ~0ull, D3D12XBOX_WAIT_VALUE_OP_EQUAL) == E_INVALIDARG);

    // Telemetry slot verification
    int32_t slot = -1;
    uint64_t calls = 0;
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12CommandQueueX::KickoffX", &slot, &calls) == 1 && slot == 22 && calls >= 2ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12CommandQueueX::Wait32BitValueX", &slot, &calls) == 1 && slot == 23 && calls >= 2ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12CommandQueueX::Wait64BitValueX", &slot, &calls) == 1 && slot == 24 && calls >= 2ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12GraphicsCommandListX::KickoffX", &slot, &calls) == 1 && slot == 82 && calls >= 2ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12GraphicsCommandListX::Wait32BitValueX", &slot, &calls) == 1 && slot == 83 && calls >= 1ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12GraphicsCommandListX::Wait64BitValueX", &slot, &calls) == 1 && slot == 84 && calls >= 1ull);
    OS_REQUIRE(OpenScarlettD3D12GetTelemetryEntry("ID3D12GraphicsCommandListX::RSSetDepthBiasX", &slot, &calls) == 1 && slot == 85 && calls >= 2ull);
    std::printf("  Verified KickoffX, Wait32BitValueX, Wait64BitValueX, RSSetDepthBiasX + telemetry OK\n");
}

// ---------------------------------------------------------------------------
// Test 3: XMem / Placed Graphics Allocations -> Valid Desktop GPU Virtual Addresses
// ---------------------------------------------------------------------------
void TestXMemPlacedGpuVirtualAddresses(ID3D12DeviceX* dev, ID3D12CommandQueueX* queue,
                                       const char* mode_label) {
    std::printf("[Test 3 - %s] XMem Attach + CreatePlacedResourceX desktop GPU VA binding...\n", mode_label);

    constexpr uint64_t kGuestBaseVa = 0x0000010010000000ull;
    constexpr uint64_t kAllocBytes = 256ull * 1024ull;

    uint64_t desktop_base_va = 0ull;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12AttachXMemAllocation(
        dev, kGuestBaseVa, kAllocBytes, &desktop_base_va)));
    OS_REQUIRE(desktop_base_va != 0ull);
    OS_REQUIRE((desktop_base_va % 65536ull) == 0ull);

    uint64_t resolved_va = 0ull;
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12ResolveXMemGpuVa(
        dev, kGuestBaseVa + 65536ull, &resolved_va)));
    OS_REQUIRE(resolved_va == desktop_base_va + 65536ull);

    // Place two buffers at offsets 0 and 64 KiB + a 2D texture at 128 KiB via CreatePlacedResourceX
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Alignment = 65536ull;
    buf_desc.Width = 65536ull;
    buf_desc.Height = 1u;
    buf_desc.DepthOrArraySize = 1u;
    buf_desc.MipLevels = 1u;
    buf_desc.SampleDesc.Count = 1u;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    TestComPtr<ID3D12Resource> placed_buf0, placed_buf1;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
        kGuestBaseVa, &buf_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(placed_buf0.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
        kGuestBaseVa + 65536ull, &buf_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(placed_buf1.Put()))));

    const D3D12_GPU_VIRTUAL_ADDRESS va0 = placed_buf0->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS va1 = placed_buf1->GetGPUVirtualAddress();
    OS_REQUIRE(va0 != 0ull && va1 != 0ull);
    OS_REQUIRE(va0 == desktop_base_va);
    OS_REQUIRE(va1 == desktop_base_va + 65536ull);

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 64u;
    tex_desc.Height = 64u;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 1u;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    TestComPtr<ID3D12Resource> placed_tex;
    OS_REQUIRE(SUCCEEDED(dev->CreatePlacedResourceX(
        kGuestBaseVa + 131072ull, &tex_desc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(placed_tex.Put()))));
    OS_REQUIRE(placed_tex.Get() != nullptr);

    // Live GPU copy into placed_buf1 and readback to prove the placed desktop VA is real GPU memory!
    D3D12_HEAP_PROPERTIES up_heap{}, rb_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC xfer_desc = buf_desc;
    xfer_desc.Alignment = 0ull;
    xfer_desc.Width = 256ull;
    xfer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    TestComPtr<ID3D12Resource> up_buf, rb_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &up_heap, D3D12_HEAP_FLAG_NONE, &xfer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up_buf.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &xfer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb_buf.Put()))));

    void* ptr = nullptr;
    D3D12_RANGE rn{0u, 0u};
    OS_REQUIRE(SUCCEEDED(up_buf->Map(0u, &rn, &ptr)));
    auto* u32 = static_cast<uint32_t*>(ptr);
    for (uint32_t i = 0; i < 64u; ++i) u32[i] = 0xA5000000u | i;
    D3D12_RANGE wr{0u, 256u};
    up_buf->Unmap(0u, &wr);

    TestComPtr<ID3D12CommandAllocator> alloc;
    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));

    cmd->CopyBufferRegion(placed_buf1.Get(), 0ull, up_buf.Get(), 0ull, 256ull);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = placed_buf1.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1u, &b);
    cmd->CopyBufferRegion(rb_buf.Get(), 0ull, placed_buf1.Get(), 0ull, 256ull);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1u, lists);
    WaitForQueueIdle(dev, queue);

    D3D12_RANGE rr{0u, 256u};
    OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rr, &ptr)));
    const auto* out_u32 = static_cast<const uint32_t*>(ptr);
    for (uint32_t i = 0; i < 64u; ++i) {
        OS_REQUIRE(out_u32[i] == (0xA5000000u | i));
    }
    rb_buf->Unmap(0u, &rn);

    // Release placed resources before detaching xmem heap
    placed_tex.Reset();
    placed_buf1.Reset();
    placed_buf0.Reset();
    OS_REQUIRE(SUCCEEDED(OpenScarlettD3D12DetachXMemAllocation(dev, kGuestBaseVa)));
    std::printf("  XMem GuestVA=0x%llX -> DesktopVA=0x%llX (placed buf0/buf1/tex + live GPU copy OK)\n",
                static_cast<unsigned long long>(kGuestBaseVa),
                static_cast<unsigned long long>(desktop_base_va));
}

// ---------------------------------------------------------------------------
// Test 4: Explicit MSVC x64 Virtual Hidden Struct-Return (sret) ABI Regression Test
// Verifies exact RCX=this, RDX=sret*, RAX=sret* register convention for:
//   - ID3D12CommandQueue::GetDesc (slot 18)
//   - ID3D12Resource::GetDesc (slot 10)
//   - ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart (slot 9) [8-byte struct!]
//   - ID3D12Device::GetResourceAllocationInfo (slot 25)
//   - ID3D12Device::GetAdapterLuid (slot 43) [8-byte struct!]
// ---------------------------------------------------------------------------
void TestMsvcX64VirtualSretAbi(ID3D12DeviceX* dev, ID3D12CommandQueueX* queue,
                               const char* mode_label) {
    std::printf("[Test 4 - %s] MSVC x64 virtual COM hidden struct-return (sret) ABI check...\n",
                mode_label);

    // 4a. ID3D12CommandQueue::GetDesc (slot 18) on CommandQueueX
    {
        auto** vtbl = *reinterpret_cast<void***>(static_cast<ID3D12CommandQueue*>(queue));
        using PfnQueueGetDescSret = D3D12_COMMAND_QUEUE_DESC*(STDMETHODCALLTYPE*)(
            ID3D12CommandQueue*, D3D12_COMMAND_QUEUE_DESC*);
        auto fn = reinterpret_cast<PfnQueueGetDescSret>(vtbl[18]);
        D3D12_COMMAND_QUEUE_DESC sret_desc{};
        D3D12_COMMAND_QUEUE_DESC* ret_ptr = fn(queue, &sret_desc);
        OS_REQUIRE(ret_ptr == &sret_desc);
        const D3D12_COMMAND_QUEUE_DESC cpp_desc = queue->GetDesc();
        OS_REQUIRE(sret_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT);
        OS_REQUIRE(sret_desc.Type == cpp_desc.Type &&
                   sret_desc.Priority == cpp_desc.Priority &&
                   sret_desc.Flags == cpp_desc.Flags &&
                   sret_desc.NodeMask == cpp_desc.NodeMask);
    }

    // 4b. ID3D12Resource::GetDesc (slot 10)
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = 128u;
    tex_desc.Height = 64u;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 4u;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        TestComPtr<ID3D12Resource> res;
        OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(res.Put()))));
        auto** vtbl = *reinterpret_cast<void***>(res.Get());
        using PfnResGetDescSret = D3D12_RESOURCE_DESC*(STDMETHODCALLTYPE*)(
            ID3D12Resource*, D3D12_RESOURCE_DESC*);
        auto fn = reinterpret_cast<PfnResGetDescSret>(vtbl[10]);
        D3D12_RESOURCE_DESC sret_desc{};
        D3D12_RESOURCE_DESC* ret_ptr = fn(res.Get(), &sret_desc);
        OS_REQUIRE(ret_ptr == &sret_desc);
        const D3D12_RESOURCE_DESC cpp_desc = res->GetDesc();
        OS_REQUIRE(sret_desc.Dimension == cpp_desc.Dimension &&
                   sret_desc.Width == 128ull &&
                   sret_desc.Height == 64u &&
                   sret_desc.MipLevels == 4u &&
                   sret_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    // 4c. ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart (slot 9) [8-byte struct sret!]
    {
        D3D12_DESCRIPTOR_HEAP_DESC dh_desc{};
        dh_desc.NumDescriptors = 4u;
        dh_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        dh_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        TestComPtr<ID3D12DescriptorHeap> dh;
        OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dh_desc, IID_PPV_ARGS(dh.Put()))));

        auto** vtbl = *reinterpret_cast<void***>(dh.Get());
        using PfnGetCpuStartSret = D3D12_CPU_DESCRIPTOR_HANDLE*(STDMETHODCALLTYPE*)(
            ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
        auto fn = reinterpret_cast<PfnGetCpuStartSret>(vtbl[9]);
        D3D12_CPU_DESCRIPTOR_HANDLE sret_handle{0};
        D3D12_CPU_DESCRIPTOR_HANDLE* ret_ptr = fn(dh.Get(), &sret_handle);
        OS_REQUIRE(ret_ptr == &sret_handle);
        OS_REQUIRE(sret_handle.ptr != 0u);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpp_handle = dh->GetCPUDescriptorHandleForHeapStart();
        OS_REQUIRE(sret_handle.ptr == cpp_handle.ptr);
    }

    // 4d. ID3D12Device::GetResourceAllocationInfo (slot 25) on DeviceX
    {
        auto** vtbl = *reinterpret_cast<void***>(static_cast<ID3D12Device*>(dev));
        using PfnGetAllocInfoSret = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(
            ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*);
        auto fn = reinterpret_cast<PfnGetAllocInfoSret>(vtbl[25]);
        D3D12_RESOURCE_ALLOCATION_INFO sret_info{};
        D3D12_RESOURCE_ALLOCATION_INFO* ret_ptr = fn(dev, &sret_info, 0u, 1u, &tex_desc);
        OS_REQUIRE(ret_ptr == &sret_info);
        OS_REQUIRE(sret_info.SizeInBytes > 0ull && sret_info.Alignment >= 65536ull);
        const D3D12_RESOURCE_ALLOCATION_INFO cpp_info = dev->GetResourceAllocationInfo(0u, 1u, &tex_desc);
        OS_REQUIRE(sret_info.SizeInBytes == cpp_info.SizeInBytes &&
                   sret_info.Alignment == cpp_info.Alignment);
    }

    // 4e. ID3D12Device::GetAdapterLuid (slot 43) on DeviceX [8-byte struct sret!]
    {
        auto** vtbl = *reinterpret_cast<void***>(static_cast<ID3D12Device*>(dev));
        using PfnGetLuidSret = LUID*(STDMETHODCALLTYPE*)(ID3D12Device*, LUID*);
        auto fn = reinterpret_cast<PfnGetLuidSret>(vtbl[43]);
        LUID sret_luid{};
        LUID* ret_ptr = fn(dev, &sret_luid);
        OS_REQUIRE(ret_ptr == &sret_luid);
        const LUID cpp_luid = dev->GetAdapterLuid();
        OS_REQUIRE(sret_luid.LowPart == cpp_luid.LowPart &&
                   sret_luid.HighPart == cpp_luid.HighPart);
    }

    std::printf("  Verified RCX=this, RDX=sret*, RAX=sret* across all 5 COM virtual methods OK\n");
}

// ---------------------------------------------------------------------------
// Test 5: Vulkan Sparse/Reserved Resources + D3D12 GetResourceTiling Parity
// ---------------------------------------------------------------------------
void TestVulkanSparseReservedAndD3D12TilingParity(ID3D12DeviceX* d3d_dev,
                                                  openscarlett::VulkanBackend& vk_hw,
                                                  openscarlett::VulkanBackend& vk_sw) {
    std::printf("[Test 5] Vulkan Sparse/Reserved Resources + D3D12 GetResourceTiling Parity...\n");

    // 5a. Query D3D12 CreateReservedResource + GetResourceTiling for 512x512 RGBA8, 5 mips
    D3D12_RESOURCE_DESC d3d_res_desc{};
    d3d_res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d3d_res_desc.Width = 512u;
    d3d_res_desc.Height = 512u;
    d3d_res_desc.DepthOrArraySize = 1u;
    d3d_res_desc.MipLevels = 5u;  // 512, 256, 128, 64, 32
    d3d_res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d3d_res_desc.SampleDesc.Count = 1u;
    d3d_res_desc.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;

    TestComPtr<ID3D12Resource> d3d_reserved;
    OS_REQUIRE(SUCCEEDED(d3d_dev->CreateReservedResource(
        &d3d_res_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(d3d_reserved.Put()))));

    UINT d3d_total_tiles = 0u;
    D3D12_PACKED_MIP_INFO d3d_packed{};
    D3D12_TILE_SHAPE d3d_tile_shape{};
    UINT d3d_num_sub = 5u;
    std::array<D3D12_SUBRESOURCE_TILING, 5> d3d_sub_tilings{};
    d3d_dev->GetResourceTiling(
        d3d_reserved.Get(), &d3d_total_tiles, &d3d_packed, &d3d_tile_shape,
        &d3d_num_sub, 0u, d3d_sub_tilings.data());
    OS_REQUIRE(d3d_total_tiles > 0u);
    OS_REQUIRE(d3d_tile_shape.WidthInTexels == 128u && d3d_tile_shape.HeightInTexels == 128u);
    OS_REQUIRE(d3d_packed.NumStandardMips + d3d_packed.NumPackedMips == 5u);
    OS_REQUIRE(d3d_packed.NumStandardMips >= 1u);

    auto run_vk_sparse_test = [&](openscarlett::VulkanBackend& vk, const char* label) {
        openscarlett::VulkanReservedResourceDesc vk_desc{};
        vk_desc.width = 512u;
        vk_desc.height = 512u;
        vk_desc.depth = 1u;
        vk_desc.mip_levels = 5u;
        vk_desc.array_layers = 1u;
        vk_desc.is_3d = false;
        vk_desc.format = openscarlett::VulkanImageFormat::Rgba8Unorm;

        openscarlett::VulkanReservedResourceHandle res_h{};
        const auto st = vk.CreateReservedImage(vk_desc, res_h);
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res_h.handle != 0ull);

        openscarlett::VulkanResourceTilingInfo tiling{};
        OS_REQUIRE(vk.GetReservedImageTiling(res_h.handle, tiling).ok());

        // Verify parity with D3D12 GetResourceTiling!
        OS_REQUIRE(tiling.tile_width_in_texels == d3d_tile_shape.WidthInTexels);
        OS_REQUIRE(tiling.tile_height_in_texels == d3d_tile_shape.HeightInTexels);
        OS_REQUIRE(tiling.tile_depth_in_texels == d3d_tile_shape.DepthInTexels);
        OS_REQUIRE(tiling.num_standard_mips == 3u && tiling.num_packed_mips == 2u);
        OS_REQUIRE(tiling.subresource_tilings.size() == tiling.num_standard_mips);
        const uint32_t common_std_mips =
            std::min<uint32_t>(tiling.num_standard_mips, d3d_packed.NumStandardMips);
        for (uint32_t m = 0u; m < common_std_mips; ++m) {
            OS_REQUIRE(tiling.subresource_tilings[m].width_in_tiles == d3d_sub_tilings[m].WidthInTiles);
            OS_REQUIRE(tiling.subresource_tilings[m].height_in_tiles == d3d_sub_tilings[m].HeightInTiles);
            OS_REQUIRE(tiling.subresource_tilings[m].depth_in_tiles == d3d_sub_tilings[m].DepthInTiles);
            OS_REQUIRE(tiling.subresource_tilings[m].start_tile_index_in_overall_resource ==
                       d3d_sub_tilings[m].StartTileIndexInOverallResource);
        }
        OS_REQUIRE(tiling.num_tiles_for_entire_resource >= 21u);

        // Bind tiles (0,0) and (2,1) in mip 0 + optional mip tail via vkQueueBindSparse!
        std::vector<openscarlett::VulkanSparseTileMapping> binds;
        openscarlett::VulkanSparseTileMapping b0{};
        b0.mip_level = 0u; b0.tile_x = 0u; b0.tile_y = 0u; b0.heap_tile_offset = 0u;
        binds.push_back(b0);
        openscarlett::VulkanSparseTileMapping b1{};
        b1.mip_level = 0u; b1.tile_x = 2u; b1.tile_y = 1u; b1.heap_tile_offset = 1u;
        binds.push_back(b1);
        if (tiling.num_packed_mips > 0u) {
            openscarlett::VulkanSparseTileMapping btail{};
            btail.bind_mip_tail = true;
            btail.heap_tile_offset = 2u;
            binds.push_back(btail);
        }
        OS_REQUIRE(vk.BindReservedImageTiles(res_h.handle, binds).ok());

        // Upload distinct deterministic patterns to both bound tiles and verify bit-exact GPU readback!
        const size_t tile_bytes =
            static_cast<size_t>(tiling.tile_width_in_texels) * tiling.tile_height_in_texels * 4u;
        std::vector<uint8_t> pat0(tile_bytes), pat1(tile_bytes), rb0, rb1;
        for (size_t i = 0; i < tile_bytes; ++i) {
            pat0[i] = static_cast<uint8_t>((i * 7u + 13u) & 0xFFu);
            pat1[i] = static_cast<uint8_t>((i * 19u + 91u) & 0xFFu);
        }
        OS_REQUIRE(vk.UploadAndReadbackReservedImageTile(res_h.handle, 0u, 0u, 0u, pat0, rb0).ok());
        OS_REQUIRE(vk.UploadAndReadbackReservedImageTile(res_h.handle, 0u, 2u, 1u, pat1, rb1).ok());
        OS_REQUIRE(rb0 == pat0);
        OS_REQUIRE(rb1 == pat1);

        // Unbind tile (2,1) via vkQueueBindSparse (map_null = true) and verify clean destroy
        b1.map_null = true;
        OS_REQUIRE(vk.BindReservedImageTiles(res_h.handle, {b1}).ok());
        OS_REQUIRE(vk.DestroyReservedImage(res_h.handle).ok());

        std::printf(
            "  [%s] Sparse 512x512x5m | icd=%d | tile=%ux%ux%u | stdMips=%u packedMips=%u totalTiles=%u | bit-exact tile readback OK\n",
            label, res_h.executed_on_vulkan_icd ? 1 : 0,
            tiling.tile_width_in_texels, tiling.tile_height_in_texels, tiling.tile_depth_in_texels,
            tiling.num_standard_mips, tiling.num_packed_mips, tiling.num_tiles_for_entire_resource);
    };

    run_vk_sparse_test(vk_hw, "Vulkan-HW");
    run_vk_sparse_test(vk_sw, "Vulkan-SW");
}

// ---------------------------------------------------------------------------
// Test 6: Live GPU S# Sampler + T# View-Range Mip-Level Selection Parity
//         (Vulkan ICD + Vulkan SW + D3D12 HW + D3D12 WARP)
// ---------------------------------------------------------------------------
struct MipSelectionCase {
    const char* name;
    uint32_t base_mip;
    uint32_t mip_count;
    float min_lod;
    float max_lod;
    float mip_lod_bias;
    float shader_lod;
    uint8_t expected_rgba[4];
};

std::array<uint8_t, 4> SampleD3D12MipSelectionCase(
    ID3D12DeviceX* dev,
    ID3D12CommandQueueX* queue,
    const std::array<std::array<uint8_t, 4>, 4>& mip_colors,
    const MipSelectionCase& tc) {
    constexpr uint32_t kBaseW = 16u;
    constexpr uint32_t kBaseH = 16u;
    constexpr uint32_t kMips = 4u;

    D3D12_HEAP_PROPERTIES def_heap{}, up_heap{}, rb_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kBaseW;
    tex_desc.Height = kBaseH;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = kMips;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1u;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    TestComPtr<ID3D12Resource> src_tex;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(src_tex.Put()))));

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kMips> fps{};
    std::array<UINT, kMips> num_rows{};
    std::array<UINT64, kMips> row_sizes{};
    UINT64 total_up = 0ull;
    dev->GetCopyableFootprints(
        &tex_desc, 0u, kMips, 0ull, fps.data(), num_rows.data(), row_sizes.data(), &total_up);

    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = total_up;
    up_desc.Height = 1u;
    up_desc.DepthOrArraySize = 1u;
    up_desc.MipLevels = 1u;
    up_desc.SampleDesc.Count = 1u;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    TestComPtr<ID3D12Resource> up_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up_buf.Put()))));

    void* ptr = nullptr;
    D3D12_RANGE rn{0u, 0u};
    OS_REQUIRE(SUCCEEDED(up_buf->Map(0u, &rn, &ptr)));
    auto* up_bytes = static_cast<uint8_t*>(ptr);
    for (uint32_t m = 0u; m < kMips; ++m) {
        const uint32_t mw = std::max(1u, kBaseW >> m);
        const uint32_t mh = std::max(1u, kBaseH >> m);
        for (uint32_t y = 0u; y < mh; ++y) {
            uint8_t* dst_row = up_bytes + fps[m].Offset + static_cast<size_t>(y) * fps[m].Footprint.RowPitch;
            for (uint32_t x = 0u; x < mw; ++x) {
                std::memcpy(dst_row + x * 4u, mip_colors[m].data(), 4u);
            }
        }
    }
    D3D12_RANGE wr{0u, static_cast<SIZE_T>(total_up)};
    up_buf->Unmap(0u, &wr);

    D3D12_RESOURCE_DESC out_desc{};
    out_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    out_desc.Width = 256ull;
    out_desc.Height = 1u;
    out_desc.DepthOrArraySize = 1u;
    out_desc.MipLevels = 1u;
    out_desc.SampleDesc.Count = 1u;
    out_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    out_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    TestComPtr<ID3D12Resource> out_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(out_buf.Put()))));

    D3D12_RESOURCE_DESC rb_desc = out_desc;
    rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    TestComPtr<ID3D12Resource> rb_buf;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb_buf.Put()))));

    // Descriptor heaps: CBV_SRV_UAV (T# SRV + output UAV) + SAMPLER (S#)
    D3D12_DESCRIPTOR_HEAP_DESC srv_hd{};
    srv_hd.NumDescriptors = 2u;
    srv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    TestComPtr<ID3D12DescriptorHeap> srv_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&srv_hd, IID_PPV_ARGS(srv_heap.Put()))));

    D3D12_DESCRIPTOR_HEAP_DESC samp_hd{};
    samp_hd.NumDescriptors = 1u;
    samp_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samp_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    TestComPtr<ID3D12DescriptorHeap> samp_heap;
    OS_REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&samp_hd, IID_PPV_ARGS(samp_heap.Put()))));

    const UINT srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{srv_cpu.ptr + srv_inc};

    // T# view range
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_d{};
    srv_d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_d.Texture2D.MostDetailedMip = tc.base_mip;
    srv_d.Texture2D.MipLevels = tc.mip_count;
    dev->CreateShaderResourceView(src_tex.Get(), &srv_d, srv_cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_d{};
    uav_d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    uav_d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav_d.Buffer.NumElements = 1u;
    dev->CreateUnorderedAccessView(out_buf.Get(), nullptr, &uav_d, uav_cpu);

    // S# sampler state
    D3D12_SAMPLER_DESC samp_d{};
    samp_d.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp_d.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_d.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp_d.MipLODBias = tc.mip_lod_bias;
    samp_d.MinLOD = tc.min_lod;
    samp_d.MaxLOD = tc.max_lod;
    dev->CreateSampler(&samp_d, samp_heap->GetCPUDescriptorHandleForHeapStart());

    static const char* kCsSampleMip = R"(
        Texture2D<float4> g_tex : register(t0);
        SamplerState g_samp : register(s0);
        RWBuffer<float4> g_out : register(u0);
        cbuffer Params : register(b0) { float g_lod; };
        [numthreads(1, 1, 1)]
        void main() {
            g_out[0] = g_tex.SampleLevel(g_samp, float2(0.5, 0.5), g_lod);
        }
    )";
    TestComPtr<ID3DBlob> cs_blob;
    OS_REQUIRE(CompileHlslFx(kCsSampleMip, "main", "cs_5_0", cs_blob));

    D3D12_DESCRIPTOR_RANGE r_srv_uav[2]{};
    r_srv_uav[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r_srv_uav[0].NumDescriptors = 1u;
    r_srv_uav[0].BaseShaderRegister = 0u;
    r_srv_uav[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r_srv_uav[1].NumDescriptors = 1u;
    r_srv_uav[1].BaseShaderRegister = 0u;
    r_srv_uav[1].OffsetInDescriptorsFromTableStart = 1u;

    D3D12_DESCRIPTOR_RANGE r_samp{};
    r_samp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    r_samp.NumDescriptors = 1u;
    r_samp.BaseShaderRegister = 0u;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2u, r_srv_uav};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1u, &r_samp};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = {0u, 0u, 1u};

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3u;
    rs_desc.pParameters = params;
    TestComPtr<ID3DBlob> rs_blob, rs_err;
    OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, rs_blob.Put(), rs_err.Put())));
    TestComPtr<ID3D12RootSignature> rs;
    OS_REQUIRE(SUCCEEDED(dev->CreateRootSignature(
        0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(rs.Put()))));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_d{};
    pso_d.pRootSignature = rs.Get();
    pso_d.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
    TestComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pso_d, IID_PPV_ARGS(pso.Put()))));

    TestComPtr<ID3D12CommandAllocator> alloc;
    TestComPtr<ID3D12GraphicsCommandListX> cmd;
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.Put()))));
    OS_REQUIRE(SUCCEEDED(dev->CreateCommandList(
        0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(),
        IID_ID3D12GraphicsCommandListX, reinterpret_cast<void**>(cmd.Put()))));

    for (uint32_t m = 0u; m < kMips; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = src_tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = up_buf.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fps[m];
        cmd->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src_tex.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1u, &b);

    ID3D12DescriptorHeap* heaps[2] = {srv_heap.Get(), samp_heap.Get()};
    cmd->SetDescriptorHeaps(2u, heaps);
    cmd->SetComputeRootSignature(rs.Get());
    cmd->SetComputeRootDescriptorTable(0u, srv_heap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRootDescriptorTable(1u, samp_heap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRoot32BitConstant(2u, F32Bits(tc.shader_lod), 0u);
    cmd->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER ob{};
    ob.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ob.Transition.pResource = out_buf.Get();
    ob.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ob.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ob.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1u, &ob);
    cmd->CopyBufferRegion(rb_buf.Get(), 0ull, out_buf.Get(), 0ull, 16ull);
    OS_REQUIRE(SUCCEEDED(cmd->Close()));

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1u, lists);
    WaitForQueueIdle(dev, queue);

    D3D12_RANGE rr{0u, 16u};
    OS_REQUIRE(SUCCEEDED(rb_buf->Map(0u, &rr, &ptr)));
    const auto* f4 = static_cast<const float*>(ptr);
    std::array<uint8_t, 4> rgba{};
    for (int c = 0; c < 4; ++c) {
        rgba[c] = static_cast<uint8_t>(std::lround(std::clamp(f4[c], 0.0f, 1.0f) * 255.0f));
    }
    rb_buf->Unmap(0u, &rn);
    return rgba;
}

void TestSamplerAndViewRangeMipParity(
    openscarlett::VulkanBackend& vk_hw,
    openscarlett::VulkanBackend& vk_sw,
    ID3D12DeviceX* d3d_hw, ID3D12CommandQueueX* d3d_hw_q,
    ID3D12DeviceX* d3d_warp, ID3D12CommandQueueX* d3d_warp_q) {
    std::printf("[Test 6] Live GPU S# Sampler + T# View-Range Mip-Level Selection Parity (Vk + D3D12)...\n");

    const std::array<std::array<uint8_t, 4>, 4> mip_colors = {{
        {240u,  20u,  20u, 255u},  // Mip 0: Red
        { 20u, 230u,  30u, 255u},  // Mip 1: Green
        { 25u,  40u, 240u, 255u},  // Mip 2: Blue
        {240u, 220u,  20u, 255u},  // Mip 3: Yellow
    }};

    openscarlett::VulkanTextureDesc vk_tex{};
    vk_tex.width = 16u;
    vk_tex.height = 16u;
    vk_tex.depth = 1u;
    vk_tex.is_3d = false;
    vk_tex.format = openscarlett::VulkanImageFormat::Rgba8Unorm;
    for (uint32_t m = 0u; m < 4u; ++m) {
        const uint32_t mw = 16u >> m;
        const uint32_t mh = 16u >> m;
        openscarlett::VulkanTextureMipLevel lvl{};
        lvl.width = mw;
        lvl.height = mh;
        lvl.depth = 1u;
        lvl.data.resize(static_cast<size_t>(mw) * mh * 4u);
        for (size_t px = 0u; px < static_cast<size_t>(mw) * mh; ++px) {
            std::memcpy(lvl.data.data() + px * 4u, mip_colors[m].data(), 4u);
        }
        vk_tex.mips.push_back(std::move(lvl));
    }

    const auto vs_spv = BuildFullscreenPassVertexSpirv();
    const auto ps_spv = BuildSeparateSamplerMipFragmentSpirv();
    const float quad_verts[12] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         3.0f, -1.0f, 0.0f, 1.0f,
        -1.0f,  3.0f, 0.0f, 1.0f,
    };

    const std::array<MipSelectionCase, 5> cases = {{
        {"Base0_Lod0->Mip0(Red)",    0u, 4u, 0.0f, 4.0f, 0.0f, 0.0f, {240u,  20u,  20u, 255u}},
        {"Base1_Lod0->Mip1(Green)",  1u, 3u, 0.0f, 4.0f, 0.0f, 0.0f, { 20u, 230u,  30u, 255u}},
        {"Base1_MinLod1->Mip2(Blue)",1u, 3u, 1.0f, 1.0f, 0.0f, 0.0f, { 25u,  40u, 240u, 255u}},
        {"Base1_Lod2->Mip3(Yellow)", 1u, 3u, 0.0f, 2.0f, 0.0f, 2.0f, {240u, 220u,  20u, 255u}},
        {"Base0_Cnt2_Lod3->Mip1(Grn)",0u, 2u, 0.0f, 4.0f, 0.0f, 3.0f, { 20u, 230u,  30u, 255u}},
    }};

    auto check_rgba = [](const uint8_t actual[4], const uint8_t exp[4]) {
        for (int c = 0; c < 4; ++c) {
            OS_REQUIRE(std::abs(static_cast<int>(actual[c]) - static_cast<int>(exp[c])) <= 2);
        }
    };

    for (const auto& tc : cases) {
        openscarlett::VulkanGraphicsDrawDesc draw{};
        draw.vertex_spirv = vs_spv;
        draw.fragment_spirv = ps_spv;
        draw.width = 16u;
        draw.height = 16u;
        draw.vertex_bytes.resize(sizeof(quad_verts));
        std::memcpy(draw.vertex_bytes.data(), quad_verts, sizeof(quad_verts));
        draw.vertex_stride = 16u;
        draw.vertex_attributes = {{0u, 0u, openscarlett::VulkanVertexFormat::R32G32B32A32Sfloat, 0u}};
        draw.vertex_count = 3u;

        const float pc_lod[4] = {tc.shader_lod, 0.0f, 0.0f, 0.0f};
        draw.push_constants.resize(sizeof(pc_lod));
        std::memcpy(draw.push_constants.data(), pc_lod, sizeof(pc_lod));

        // Separate T# (SampledImage binding 0) and S# (Sampler binding 1)
        openscarlett::VulkanDescriptorBinding b_t{};
        b_t.binding = 0u;
        b_t.kind = openscarlett::VulkanDescriptorKind::SampledImage;
        b_t.texture = vk_tex;
        b_t.view_base_mip_level = tc.base_mip;
        b_t.view_mip_level_count = tc.mip_count;

        openscarlett::VulkanDescriptorBinding b_s{};
        b_s.binding = 1u;
        b_s.kind = openscarlett::VulkanDescriptorKind::Sampler;
        b_s.linear_filter = false;
        b_s.sampler_min_lod = tc.min_lod;
        b_s.sampler_max_lod = tc.max_lod;
        b_s.sampler_mip_lod_bias = tc.mip_lod_bias;

        draw.descriptors = {b_t, b_s};

        openscarlett::VulkanGraphicsDrawResult vk_hw_res{}, vk_sw_res{};
        OS_REQUIRE(vk_hw.ExecuteGraphicsDraw(draw, vk_hw_res).ok());
        OS_REQUIRE(vk_sw.ExecuteGraphicsDraw(draw, vk_sw_res).ok());

        const size_t center = (8u * 16u + 8u) * 4u;
        check_rgba(&vk_hw_res.framebuffer.pixels[center], tc.expected_rgba);
        check_rgba(&vk_sw_res.framebuffer.pixels[center], tc.expected_rgba);

        const auto d3d_warp_px = SampleD3D12MipSelectionCase(d3d_warp, d3d_warp_q, mip_colors, tc);
        check_rgba(d3d_warp_px.data(), tc.expected_rgba);

        if (d3d_hw && d3d_hw_q) {
            const auto d3d_hw_px = SampleD3D12MipSelectionCase(d3d_hw, d3d_hw_q, mip_colors, tc);
            check_rgba(d3d_hw_px.data(), tc.expected_rgba);
        }

        std::printf("  %-28s -> RGBA(%u,%u,%u,%u) [VkHW+VkSW+D3D12HW+WARP PARITY OK]\n",
                    tc.name,
                    vk_hw_res.framebuffer.pixels[center + 0],
                    vk_hw_res.framebuffer.pixels[center + 1],
                    vk_hw_res.framebuffer.pixels[center + 2],
                    vk_hw_res.framebuffer.pixels[center + 3]);
    }
}

// ---------------------------------------------------------------------------
// Test 7: xg_xs Console Pipe-Config Query + HTILE / DCC / CMASK Metadata Planes
// ---------------------------------------------------------------------------
void TestXgConsolePipeConfigAndMetadataPlanes() {
    std::printf("[Test 7] xg_xs Scarlett Pipe-Config Query + HTILE/DCC/CMASK Metadata Layouts...\n");
    OpenScarlettXgResetTelemetry();

    // 7a. Scarlett Anaconda (Series X) vs Lockhart (Series S) pipe config queries
    XG_SCARLETT_PIPE_CONFIG anaconda{}, lockhart{};
    OS_REQUIRE(XGQueryScarlettPipeConfig(XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &anaconda) == 0);
    OS_REQUIRE(XGQueryScarlettPipeConfig(XG_CONSOLE_TYPE_SCARLETT_LOCKHART, &lockhart) == 0);
    OS_REQUIRE(anaconda.NumPipes == 16u && anaconda.NumBanks == 16u &&
               anaconda.NumShaderEngines == 2u && anaconda.PipeInterleaveBytes == 256u &&
               anaconda.PipeBits == 4u);
    OS_REQUIRE(lockhart.NumPipes == 8u && lockhart.NumBanks == 8u &&
               lockhart.NumShaderEngines == 1u && lockhart.PipeInterleaveBytes == 256u &&
               lockhart.PipeBits == 3u);
    OS_REQUIRE(XGQueryScarlettPipeConfig(static_cast<XG_CONSOLE_TYPE>(99u), &anaconda) != 0);
    OS_REQUIRE(XGQueryScarlettPipeConfig(XG_CONSOLE_TYPE_SCARLETT_ANACONDA, nullptr) != 0);

    // 7b. Depth/Stencil 1920x1080 D32_FLOAT_S8X24_UINT -> HTILE enabled, DCC/CMASK disabled
    {
        XG_TEXTURE2D_DESC ds{};
        ds.Width = 1920u;
        ds.Height = 1080u;
        ds.MipLevels = 1u;
        ds.ArraySize = 1u;
        ds.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        ds.SampleCount = 1u;
        ds.BindFlags = XG_BIND_DEPTH_STENCIL;
        ds.TileMode = XG_TILE_MODE_2D_THIN_64KB_Z_X;

        XG_RESOURCE_METADATA_LAYOUT meta_x{}, meta_s{};
        OS_REQUIRE(XGComputeTexture2DMetadataLayout(&ds, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &meta_x) == 0);
        OS_REQUIRE(XGComputeTexture2DMetadataLayout(&ds, XG_CONSOLE_TYPE_SCARLETT_LOCKHART, &meta_s) == 0);
        OS_REQUIRE(meta_x.Htile.Enabled == 1u && meta_x.Htile.SizeBytes > 0ull);
        OS_REQUIRE(meta_x.Htile.OffsetBytes >= meta_x.BaseSurfaceSizeBytes);
        OS_REQUIRE((meta_x.Htile.OffsetBytes % meta_x.Htile.AlignmentBytes) == 0ull);
        OS_REQUIRE(meta_x.Dcc.Enabled == 0u && meta_x.Cmask.Enabled == 0u);
        OS_REQUIRE(meta_x.TotalSizeWithMetadataBytes >= meta_x.Htile.OffsetBytes + meta_x.Htile.SizeBytes);
        OS_REQUIRE(meta_x.Htile.MetaBlockWidth > meta_s.Htile.MetaBlockWidth);

        XG_METADATA_PLANE_INFO htile_direct{};
        OS_REQUIRE(XGComputeHtileLayout(&ds, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &htile_direct) == 0);
        OS_REQUIRE(htile_direct.OffsetBytes == meta_x.Htile.OffsetBytes &&
                   htile_direct.SizeBytes == meta_x.Htile.SizeBytes);
        std::printf("  Depth 1920x1080 D32S8 | Base=%llu B | HTILE off=%llu sz=%llu B | OK\n",
                    static_cast<unsigned long long>(meta_x.BaseSurfaceSizeBytes),
                    static_cast<unsigned long long>(meta_x.Htile.OffsetBytes),
                    static_cast<unsigned long long>(meta_x.Htile.SizeBytes));
    }

    // 7c. Color RenderTarget 1920x1080 RGBA8 4x MSAA -> DCC + CMASK enabled, HTILE disabled
    {
        XG_TEXTURE2D_DESC rt{};
        rt.Width = 1920u;
        rt.Height = 1080u;
        rt.MipLevels = 1u;
        rt.ArraySize = 1u;
        rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt.SampleCount = 4u;
        rt.BindFlags = XG_BIND_RENDER_TARGET | XG_BIND_SHADER_RESOURCE;
        rt.TileMode = XG_TILE_MODE_2D_THIN_64KB_R_X;

        XG_RESOURCE_METADATA_LAYOUT meta{};
        OS_REQUIRE(XGComputeTexture2DMetadataLayout(&rt, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &meta) == 0);
        OS_REQUIRE(meta.Htile.Enabled == 0u);
        OS_REQUIRE(meta.Dcc.Enabled == 1u && meta.Dcc.SizeBytes > 0ull);
        OS_REQUIRE(meta.Cmask.Enabled == 1u && meta.Cmask.SizeBytes > 0ull);
        OS_REQUIRE(meta.Dcc.OffsetBytes >= meta.BaseSurfaceSizeBytes);
        OS_REQUIRE(meta.Cmask.OffsetBytes >= meta.Dcc.OffsetBytes + meta.Dcc.SizeBytes);
        OS_REQUIRE(meta.TotalSizeWithMetadataBytes >= meta.Cmask.OffsetBytes + meta.Cmask.SizeBytes);

        XG_METADATA_PLANE_INFO cmask_direct{};
        OS_REQUIRE(XGComputeCmaskLayout(&rt, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &cmask_direct) == 0);
        OS_REQUIRE(cmask_direct.OffsetBytes == meta.Cmask.OffsetBytes &&
                   cmask_direct.SizeBytes == meta.Cmask.SizeBytes);

        // Verify XGComputeDccMetadataLayout computes real DCC when given buffer,
        // and preserves fail-closed E_NOTIMPL when probed with (nullptr, 0)!
        HMODULE xg_mod = GetModuleHandleA("xg_xs.dll");
        OS_REQUIRE(xg_mod != nullptr);
        using PfnXgDcc = int32_t(__stdcall*)(const XG_TEXTURE2D_DESC*, void*, uint64_t);
        auto fn_dcc = reinterpret_cast<PfnXgDcc>(GetProcAddress(xg_mod, "XGComputeDccMetadataLayout"));
        OS_REQUIRE(fn_dcc != nullptr);
        XG_METADATA_PLANE_INFO dcc_direct{};
        OS_REQUIRE(fn_dcc(&rt, &dcc_direct, sizeof(dcc_direct)) == 0);
        OS_REQUIRE(dcc_direct.Enabled == 1u && dcc_direct.OffsetBytes == meta.Dcc.OffsetBytes &&
                   dcc_direct.SizeBytes == meta.Dcc.SizeBytes);
        OS_REQUIRE(fn_dcc(&rt, nullptr, 0ull) == static_cast<int32_t>(E_NOTIMPL));

        std::printf("  ColorRT 1920x1080 4xMSAA | Base=%llu B | DCC off=%llu sz=%llu B | CMASK off=%llu sz=%llu B | OK\n",
                    static_cast<unsigned long long>(meta.BaseSurfaceSizeBytes),
                    static_cast<unsigned long long>(meta.Dcc.OffsetBytes),
                    static_cast<unsigned long long>(meta.Dcc.SizeBytes),
                    static_cast<unsigned long long>(meta.Cmask.OffsetBytes),
                    static_cast<unsigned long long>(meta.Cmask.SizeBytes));
    }

    // 7d. Linear and BC7 textures must have no HTILE/DCC/CMASK metadata planes
    {
        XG_TEXTURE2D_DESC lin{256u, 256u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, 1u, XG_BIND_RENDER_TARGET, XG_TILE_MODE_LINEAR};
        XG_RESOURCE_METADATA_LAYOUT m_lin{};
        OS_REQUIRE(XGComputeTexture2DMetadataLayout(&lin, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &m_lin) == 0);
        OS_REQUIRE(m_lin.Htile.Enabled == 0u && m_lin.Dcc.Enabled == 0u && m_lin.Cmask.Enabled == 0u);
        OS_REQUIRE(m_lin.TotalSizeWithMetadataBytes == m_lin.BaseSurfaceSizeBytes);

        XG_TEXTURE2D_DESC bc7{256u, 256u, 1u, 1u, DXGI_FORMAT_BC7_UNORM, 1u, XG_BIND_SHADER_RESOURCE, XG_TILE_MODE_2D_THIN_64KB_S};
        XG_RESOURCE_METADATA_LAYOUT m_bc7{};
        OS_REQUIRE(XGComputeTexture2DMetadataLayout(&bc7, XG_CONSOLE_TYPE_SCARLETT_ANACONDA, &m_bc7) == 0);
        OS_REQUIRE(m_bc7.Htile.Enabled == 0u && m_bc7.Dcc.Enabled == 0u && m_bc7.Cmask.Enabled == 0u);
        OS_REQUIRE(m_bc7.TotalSizeWithMetadataBytes == m_bc7.BaseSurfaceSizeBytes);
    }
}

void RunD3D12Suite(bool use_warp,
                   TestComPtr<ID3D12DeviceX>& out_dev,
                   TestComPtr<ID3D12CommandQueueX>& out_queue) {
    const char* label = use_warp ? "WARP" : "HW";
    OpenScarlettD3D12SetPreferWarp(use_warp ? 1 : 0);

    D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
    params.Version = 1u;
    OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(
        nullptr, &params, IID_ID3D12DeviceX, reinterpret_cast<void**>(out_dev.Put()))));
    OS_REQUIRE(out_dev.Get() != nullptr);

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    OS_REQUIRE(SUCCEEDED(out_dev->CreateCommandQueue(
        &qd, IID_ID3D12CommandQueueX, reinterpret_cast<void**>(out_queue.Put()))));
    OS_REQUIRE(out_queue.Get() != nullptr);

    TestR9G9B9E5UavFallback(out_dev.Get(), out_queue.Get(), label, use_warp);
    TestCommandQueueAndListXboxMethods(out_dev.Get(), out_queue.Get(), label);
    TestXMemPlacedGpuVirtualAddresses(out_dev.Get(), out_queue.Get(), label);
    TestMsvcX64VirtualSretAbi(out_dev.Get(), out_queue.Get(), label);
    OpenScarlettD3D12SetPreferWarp(0);
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    openscarlett::D3D12AdapterInfo hw_info{};
    const bool has_hw_d3d12 = HasHardwareD3D12Gpu(hw_info);
    if (has_hw_d3d12) {
        std::printf("Detected HW D3D12 GPU: %s (native R9G9B9E5 UAV=%s)\n",
                    hw_info.description.c_str(),
                    hw_info.supports_r9g9b9e5_uav ? "yes" : "no");
    } else {
        std::printf("No discrete HW D3D12 GPU detected; running WARP suite.\n");
    }

    TestComPtr<ID3D12DeviceX> hw_dev, warp_dev;
    TestComPtr<ID3D12CommandQueueX> hw_queue, warp_queue;
    if (has_hw_d3d12) {
        RunD3D12Suite(false, hw_dev, hw_queue);
    }
    RunD3D12Suite(true, warp_dev, warp_queue);

    openscarlett::VulkanBackend vk_hw, vk_sw;
    OS_REQUIRE(vk_hw.Initialize(true).ok());
    OS_REQUIRE(vk_sw.InitializeSoftwareFallback().ok());
    std::printf("Vulkan ICD: %s (sparseBinding=%d, sparse2D=%d)\n",
                vk_hw.GetDeviceInfo().device_name.c_str(),
                vk_hw.GetDeviceInfo().supports_sparse_binding ? 1 : 0,
                vk_hw.GetDeviceInfo().supports_sparse_residency_image_2d ? 1 : 0);

    TestVulkanSparseReservedAndD3D12TilingParity(
        hw_dev ? hw_dev.Get() : warp_dev.Get(), vk_hw, vk_sw);

    TestSamplerAndViewRangeMipParity(
        vk_hw, vk_sw,
        hw_dev.Get(), hw_queue.Get(),
        warp_dev.Get(), warp_queue.Get());

    TestXgConsolePipeConfigAndMetadataPlanes();

    vk_sw.Shutdown();
    vk_hw.Shutdown();
    std::printf("\n[ALL PASS] test_gpu_d3d12x_vk_parity completed successfully!\n");
    return 0;
#else
    std::printf("test_gpu_d3d12x_vk_parity skipped on non-Windows host.\n");
    return 0;
#endif
}
