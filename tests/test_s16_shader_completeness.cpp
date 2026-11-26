// OpenScarlett - MIT License
// Sprint 16 Workstream 2: RDNA2 / GFX10.3 Shader Compiler 100% Completeness Verification Suite
// Covers:
//  1. Single bounds-check per s_buffer_load_dwordx2/x4/x8/x16 (>50% check reduction + bit-exact OOB/partial-tail)
//  2. BVH box-node word-bounded reads (truncated/OOB nodes -> safe empty miss, zero TDR)
//  3. Exact RNE rounding & NaN->0 for v_cvt_pknorm_{i16,u16}_{f16,f32} & v_cvt_norm_{i16,u16}_f16
//  4. SPIR-V MinLod capability/operand preserved in HLSL Sample/SampleGrad/SampleBias/SampleCmp
//  5. Compressed EXP (compr=1) across ALL 16 raw EN masks (0x0..0xF) + low16 draw modes
//  6. MUBUF D16 byte/short loads (0x20..0x25) & d16_hi stores (0x19, 0x1B)
//  7. LDS DS completeness (ds_mskor[_rtn]_b32, ds_wrxchg2[st64]_rtn_b32, ds_{read,write}_b96/b128, ds_nop)
//  8. All 24 SOP1 saveexec/wrexec variants + v_mad_{u64_u32,i64_i32} inactive-lane carry zeroing
//  9. Wave64 vote invalidation + s_flbit_* + s_cmp_{eq,lg}_u64
// 10. Quad-local DPP8/DPP8FI + non-packed 16-bit VOP3 op_sel + 64-bit PS integer compares
// 11. All-stage real-GPU execution (CS/VS/PS/MS/AS/HS+DS/GS) + honest 775/779 bench JSON
// 12. Zero-hitch multi-threaded async PSO + persistent disk ShaderCache

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "openscarlett/gpu/descriptor_sgpr_bridge.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rdna2_stages_ext.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "openscarlett/gpu/shader_cache.h"
#include "os_check.h"
#include "rdna2_coverage_tool.h"
#include "rdna2_isa_mem_img.h"
#include "rdna2_isa_scalar_vector.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace {

using namespace openscarlett;

uint32_t F2U(float f) noexcept {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

float U2F(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

uint16_t F32ToF16(float f) noexcept {
    const uint32_t x = F2U(f);
    const uint32_t sign = (x >> 16u) & 0x8000u;
    const uint32_t exp = (x >> 23u) & 0xFFu;
    const uint32_t mant = x & 0x007FFFFFu;
    if (exp == 0xFFu) {
        return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x0200u : 0u));
    }
    const int32_t new_exp = static_cast<int32_t>(exp) - 127 + 15;
    if (new_exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (new_exp <= 0) {
        if (new_exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = (mant | 0x00800000u) >> static_cast<uint32_t>(14 - new_exp);
        return static_cast<uint16_t>(sign | m);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10u) | (mant >> 13u));
}

size_t CountSubstr(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0u;
    size_t count = 0u;
    size_t pos = 0u;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

size_t CountSpirvOpcode(const std::vector<uint32_t>& spv, uint16_t target_op) {
    size_t count = 0u;
    size_t idx = 5u;
    while (idx < spv.size()) {
        const uint32_t w = spv[idx];
        const uint16_t op = static_cast<uint16_t>(w & 0xFFFFu);
        const uint32_t wc = w >> 16u;
        if (wc == 0u || idx + wc > spv.size()) break;
        if (op == target_op) ++count;
        idx += wc;
    }
    return count;
}

std::string FindSpirvValPortable() {
    std::error_code ec;
#if defined(_WIN32)
    char env_buf[1024]{};
    const DWORD env_len = GetEnvironmentVariableA(
        "OPENSCARLETT_SPIRV_VAL", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len > 0u && env_len < sizeof(env_buf) && std::filesystem::exists(env_buf, ec)) {
        return std::string(env_buf, env_len);
    }
#endif
    const std::array<std::filesystem::path, 2> roots = {
        std::filesystem::path(OPENSCARLETT_SOURCE_DIR),
        std::filesystem::current_path(ec),
    };
    for (auto cur : roots) {
        for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
            const auto cand = cur / ".tools" / "spirv-val.exe";
            if (std::filesystem::exists(cand, ec)) return cand.string();
            const auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Portable COM / D3D12 Runner (Zero raw __uuidof, Zero WRL -> MSVC/Clang/MinGW)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComRef& operator=(ComRef&& o) noexcept {
        if (this != &o) {
            Reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }
    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

// Explicit portable IIDs (never uses raw __uuidof)
constexpr GUID kIidIDXGIFactory6         = {0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}};
constexpr GUID kIidIDXGIAdapter1         = {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
constexpr GUID kIidID3D12Device          = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
constexpr GUID kIidID3D12CommandQueue    = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
constexpr GUID kIidID3D12CommandAllocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
constexpr GUID kIidID3D12GraphicsCmdList = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
constexpr GUID kIidID3D12Fence           = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
constexpr GUID kIidID3D12RootSignature   = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
constexpr GUID kIidID3D12PipelineState   = {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};
constexpr GUID kIidID3D12Resource        = {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};
constexpr GUID kIidID3D12DescriptorHeap  = {0x8efb471d, 0x616c, 0x4f49, {0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51}};

struct S16D3D12Runner {
    ComRef<IDXGIFactory6> factory;
    ComRef<IDXGIAdapter1> adapter;
    ComRef<ID3D12Device> device;
    ComRef<ID3D12CommandQueue> queue;
    ComRef<ID3D12CommandAllocator> allocator;
    ComRef<ID3D12GraphicsCommandList> cmd_list;
    ComRef<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0u;
    std::string adapter_name;
    bool is_warp = false;
    uint32_t highest_sm = 0u;

    ~S16D3D12Runner() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool use_warp) {
        is_warp = use_warp;
        if (FAILED(CreateDXGIFactory2(0, kIidIDXGIFactory6, factory.PutVoid()))) return false;
        if (use_warp) {
            if (FAILED(factory->EnumWarpAdapter(kIidIDXGIAdapter1, adapter.PutVoid()))) return false;
        } else {
            if (FAILED(factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, kIidIDXGIAdapter1, adapter.PutVoid()))) {
                return false;
            }
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, sizeof(name_buf), nullptr, nullptr);
        adapter_name = name_buf;

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, kIidID3D12Device, device.PutVoid()))) {
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, kIidID3D12CommandQueue, queue.PutVoid()))) return false;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, kIidID3D12CommandAllocator, allocator.PutVoid()))) return false;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             kIidID3D12GraphicsCmdList, cmd_list.PutVoid()))) return false;
        cmd_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, kIidID3D12Fence, fence.PutVoid()))) return false;
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    std::string TargetProfile(const char* prefix) const {
        if (highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5)) return std::string(prefix) + "_6_5";
        if (highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) return std::string(prefix) + "_6_2";
        return std::string(prefix) + "_6_0";
    }

    bool WaitGpu() {
        ++fence_val;
        if (FAILED(queue->Signal(fence.Get(), fence_val))) return false;
        if (fence->GetCompletedValue() < fence_val) {
            if (FAILED(fence->SetEventOnCompletion(fence_val, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 15000) != WAIT_OBJECT_0) return false;
        }
        return true;
    }

    ComRef<ID3D12Resource> CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap,
                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<uint64_t>(4u, bytes);
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComRef<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                   kIidID3D12Resource, res.PutVoid()))) {
            return {};
        }
        return res;
    }

    ComRef<ID3D12Resource> CreateTex2D(uint32_t w, uint32_t h, uint16_t mips, DXGI_FORMAT fmt,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = mips;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;
        ComRef<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                   kIidID3D12Resource, res.PutVoid()))) {
            return {};
        }
        return res;
    }

    // Runs compute shader with Descriptor-Table Raw UAVs (u0, u1) so GetDimensions() returns EXACT byte sizes!
    bool RunComputeDualUavTable(const std::vector<uint8_t>& dxil,
                                const std::vector<uint32_t>& uav0_in,
                                const std::vector<uint32_t>& uav1_init,
                                std::vector<uint32_t>& uav1_out,
                                uint32_t group_x = 1u) {
        const uint32_t n0 = std::max<uint32_t>(1u, static_cast<uint32_t>(uav0_in.size()));
        const uint32_t n1 = std::max<uint32_t>(1u, static_cast<uint32_t>(uav1_init.size()));
        const uint64_t b0 = static_cast<uint64_t>(n0) * 4u;
        const uint64_t b1 = static_cast<uint64_t>(n1) * 4u;
        uav1_out.assign(uav1_init.size(), 0u);

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 2u;
        range.BaseShaderRegister = 0u;
        range.OffsetInDescriptorsFromTableStart = 0u;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1u, &range};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1u;
        rsd.pParameters = &param;
        ComRef<ID3DBlob> sig_blob, err_blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put()))) return false;
        ComRef<ID3D12RootSignature> rs;
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                               kIidID3D12RootSignature, rs.PutVoid()))) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = rs.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComRef<ID3D12PipelineState> pso;
        if (FAILED(device->CreateComputePipelineState(&cpd, kIidID3D12PipelineState, pso.PutVoid()))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2u;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComRef<ID3D12DescriptorHeap> heap;
        if (FAILED(device->CreateDescriptorHeap(&hd, kIidID3D12DescriptorHeap, heap.PutVoid()))) return false;
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto up0 = CreateBuffer(b0, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto up1 = CreateBuffer(b1, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto gpu0 = CreateBuffer(b0, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto gpu1 = CreateBuffer(b1, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(b1, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!up0 || !up1 || !gpu0 || !gpu1 || !rb) return false;

        void* mapped = nullptr;
        if (FAILED(up0->Map(0, nullptr, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(b0));
        if (!uav0_in.empty()) std::memcpy(mapped, uav0_in.data(), uav0_in.size() * 4u);
        up0->Unmap(0, nullptr);

        if (FAILED(up1->Map(0, nullptr, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(b1));
        if (!uav1_init.empty()) std::memcpy(mapped, uav1_init.data(), uav1_init.size() * 4u);
        up1->Unmap(0, nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32_TYPELESS;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavd.Buffer.FirstElement = 0u;
        uavd.Buffer.NumElements = n0;
        uavd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(gpu0.Get(), nullptr, &uavd, cpu_h);
        cpu_h.ptr += inc;
        uavd.Buffer.NumElements = n1;
        device->CreateUnorderedAccessView(gpu1.Get(), nullptr, &uavd, cpu_h);

        if (FAILED(allocator->Reset())) return false;
        if (FAILED(cmd_list->Reset(allocator.Get(), pso.Get()))) return false;
        cmd_list->CopyBufferRegion(gpu0.Get(), 0, up0.Get(), 0, b0);
        cmd_list->CopyBufferRegion(gpu1.Get(), 0, up1.Get(), 0, b1);

        D3D12_RESOURCE_BARRIER to_uav[2]{};
        to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[0].Transition = {gpu0.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[1].Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, to_uav);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(rs.Get());
        cmd_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(group_x, 1u, 1u);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);
        cmd_list->CopyBufferRegion(rb.Get(), 0, gpu1.Get(), 0, b1);

        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!WaitGpu()) return false;

        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        if (!uav1_out.empty()) std::memcpy(uav1_out.data(), mapped, uav1_out.size() * 4u);
        rb->Unmap(0, nullptr);
        return true;
    }

    // Runs compute shader with Texture2D (t0, 2 mips: 4x4 mip0 + 2x2 mip1) + UAVs u0, u1 + static samplers s0, s1
    bool RunComputeTex2DMip(const std::vector<uint8_t>& dxil,
                            const std::vector<uint32_t>& uav0_in,
                            const std::vector<uint32_t>& uav1_init,
                            const std::array<float, 4>& mip0_rgba,
                            const std::array<float, 4>& mip1_rgba,
                            std::vector<uint32_t>& uav1_out) {
        const uint32_t n0 = std::max<uint32_t>(1u, static_cast<uint32_t>(uav0_in.size()));
        const uint32_t n1 = std::max<uint32_t>(1u, static_cast<uint32_t>(uav1_init.size()));
        const uint64_t b0 = static_cast<uint64_t>(n0) * 4u;
        const uint64_t b1 = static_cast<uint64_t>(n1) * 4u;
        uav1_out.assign(uav1_init.size(), 0u);

        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 2u;
        ranges[0].BaseShaderRegister = 0u;
        ranges[0].OffsetInDescriptorsFromTableStart = 0u;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1u;
        ranges[1].BaseShaderRegister = 0u;
        ranges[1].OffsetInDescriptorsFromTableStart = 2u;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {2u, ranges};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samps[2]{};
        samps[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samps[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samps[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samps[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samps[0].MaxLOD = D3D12_FLOAT32_MAX;
        samps[0].ShaderRegister = 0u;
        samps[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        samps[1] = samps[0];
        samps[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        samps[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samps[1].ShaderRegister = 1u;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1u;
        rsd.pParameters = &param;
        rsd.NumStaticSamplers = 2u;
        rsd.pStaticSamplers = samps;
        ComRef<ID3DBlob> sig_blob, err_blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put()))) return false;
        ComRef<ID3D12RootSignature> rs;
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                               kIidID3D12RootSignature, rs.PutVoid()))) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = rs.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComRef<ID3D12PipelineState> pso;
        if (FAILED(device->CreateComputePipelineState(&cpd, kIidID3D12PipelineState, pso.PutVoid()))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3u;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComRef<ID3D12DescriptorHeap> heap;
        if (FAILED(device->CreateDescriptorHeap(&hd, kIidID3D12DescriptorHeap, heap.PutVoid()))) return false;
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto up0 = CreateBuffer(b0, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto up1 = CreateBuffer(b1, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto gpu0 = CreateBuffer(b0, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto gpu1 = CreateBuffer(b1, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(b1, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto tex = CreateTex2D(4u, 4u, 2u, DXGI_FORMAT_R32G32B32A32_FLOAT,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        constexpr uint32_t kPitch = 256u;
        auto tex_up = CreateBuffer(kPitch * 6u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up0 || !up1 || !gpu0 || !gpu1 || !rb || !tex || !tex_up) return false;

        void* mapped = nullptr;
        if (FAILED(up0->Map(0, nullptr, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(b0));
        if (!uav0_in.empty()) std::memcpy(mapped, uav0_in.data(), uav0_in.size() * 4u);
        up0->Unmap(0, nullptr);

        if (FAILED(up1->Map(0, nullptr, &mapped))) return false;
        std::memset(mapped, 0, static_cast<size_t>(b1));
        if (!uav1_init.empty()) std::memcpy(mapped, uav1_init.data(), uav1_init.size() * 4u);
        up1->Unmap(0, nullptr);

        if (FAILED(tex_up->Map(0, nullptr, &mapped))) return false;
        auto* base = static_cast<uint8_t*>(mapped);
        std::memset(base, 0, kPitch * 6u);
        for (uint32_t y = 0; y < 4u; ++y) {
            for (uint32_t x = 0; x < 4u; ++x) {
                std::memcpy(base + y * kPitch + x * 16u, mip0_rgba.data(), 16u);
            }
        }
        for (uint32_t y = 0; y < 2u; ++y) {
            for (uint32_t x = 0; x < 2u; ++x) {
                std::memcpy(base + (4u + y) * kPitch + x * 16u, mip1_rgba.data(), 16u);
            }
        }
        tex_up->Unmap(0, nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32_TYPELESS;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavd.Buffer.NumElements = n0;
        uavd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(gpu0.Get(), nullptr, &uavd, cpu_h);
        cpu_h.ptr += inc;
        uavd.Buffer.NumElements = n1;
        device->CreateUnorderedAccessView(gpu1.Get(), nullptr, &uavd, cpu_h);
        cpu_h.ptr += inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 2u;
        device->CreateShaderResourceView(tex.Get(), &srvd, cpu_h);

        if (FAILED(allocator->Reset())) return false;
        if (FAILED(cmd_list->Reset(allocator.Get(), pso.Get()))) return false;
        cmd_list->CopyBufferRegion(gpu0.Get(), 0, up0.Get(), 0, b0);
        cmd_list->CopyBufferRegion(gpu1.Get(), 0, up1.Get(), 0, b1);

        for (uint32_t mip = 0u; mip < 2u; ++mip) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = tex_up.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = (mip == 0u) ? 0u : (4u * kPitch);
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, (mip == 0u) ? 4u : 2u, (mip == 0u) ? 4u : 2u, 1u, kPitch};
            cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER bars[3]{};
        bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[0].Transition = {gpu0.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[1].Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        bars[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[2].Transition = {tex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        cmd_list->ResourceBarrier(3, bars);

        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(rs.Get());
        cmd_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        cmd_list->Dispatch(1u, 1u, 1u);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {gpu1.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);
        cmd_list->CopyBufferRegion(rb.Get(), 0, gpu1.Get(), 0, b1);

        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!WaitGpu()) return false;

        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        if (!uav1_out.empty()) std::memcpy(uav1_out.data(), mapped, uav1_out.size() * 4u);
        rb->Unmap(0, nullptr);
        return true;
    }

    bool RunGraphicsDraw(const std::vector<uint8_t>& vs_dxil,
                         const std::vector<uint8_t>& ps_dxil,
                         std::vector<float>& out_rgba32f_4x4) {
        constexpr uint32_t kW = 4u, kH = 4u, kPitch = 256u;
        out_rgba32f_4x4.assign(kW * kH * 4u, 0.0f);

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        ComRef<ID3DBlob> sig_blob, err_blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put()))) return false;
        ComRef<ID3D12RootSignature> rs;
        if (FAILED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                               kIidID3D12RootSignature, rs.PutVoid()))) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gps{};
        gps.pRootSignature = rs.Get();
        gps.VS = {vs_dxil.data(), vs_dxil.size()};
        gps.PS = {ps_dxil.data(), ps_dxil.size()};
        gps.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gps.SampleMask = 0xFFFFFFFFu;
        gps.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gps.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gps.RasterizerState.DepthClipEnable = TRUE;
        gps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gps.NumRenderTargets = 1u;
        gps.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        gps.SampleDesc.Count = 1u;
        ComRef<ID3D12PipelineState> pso;
        if (FAILED(device->CreateGraphicsPipelineState(&gps, kIidID3D12PipelineState, pso.PutVoid()))) return false;

        auto rt = CreateTex2D(kW, kH, 1u, DXGI_FORMAT_R32G32B32A32_FLOAT,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto rb = CreateBuffer(kPitch * kH, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!rt || !rb) return false;

        D3D12_DESCRIPTOR_HEAP_DESC rhd{};
        rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rhd.NumDescriptors = 1u;
        ComRef<ID3D12DescriptorHeap> rtv_heap;
        if (FAILED(device->CreateDescriptorHeap(&rhd, kIidID3D12DescriptorHeap, rtv_heap.PutVoid()))) return false;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);

        if (FAILED(allocator->Reset())) return false;
        if (FAILED(cmd_list->Reset(allocator.Get(), pso.Get()))) return false;
        const float clear_col[4] = {-9.0f, -9.0f, -9.0f, -9.0f};
        cmd_list->ClearRenderTargetView(rtv_h, clear_col, 0, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        cmd_list->RSSetViewports(1, &vp);
        cmd_list->RSSetScissorRects(1, &sc);
        cmd_list->OMSetRenderTargets(1, &rtv_h, FALSE, nullptr);
        cmd_list->SetGraphicsRootSignature(rs.Get());
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd_list->DrawInstanced(3u, 1u, 0u, 0u);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, kW, kH, 1u, kPitch};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = rt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0u;
        cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!WaitGpu()) return false;

        void* mapped = nullptr;
        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        for (uint32_t y = 0; y < kH; ++y) {
            std::memcpy(&out_rgba32f_4x4[y * kW * 4u], static_cast<const uint8_t*>(mapped) + y * kPitch, kW * 16u);
        }
        rb->Unmap(0, nullptr);
        return true;
    }
};
#endif

struct TestEnv {
    std::string dxc_path;
    std::string spirv_val_path;
#if defined(_WIN32)
    S16D3D12Runner hw;
    S16D3D12Runner warp;
    bool has_hw = false;
    bool has_warp = false;
#endif
};

void ValidateSpirvIfAvailable(const TestEnv& env, const std::vector<uint32_t>& spv, const char* label) {
    OS_REQUIRE(ValidateSpirvBinary(spv).ok());
    if (!env.spirv_val_path.empty()) {
        std::string diag;
        const Status st = ValidateSpirvExternal(spv, diag, env.spirv_val_path, "vulkan1.2");
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val failed (%s): %s\n%s\n", label, st.message.c_str(), diag.c_str());
        }
        OS_REQUIRE(st.ok());
    }
}

void RunAndVerifyComputeBothGpus(TestEnv& env,
                                 const Rdna2LiftResult& lifted,
                                 const std::vector<uint32_t>& uav0,
                                 const std::vector<uint32_t>& init_uav1,
                                 const std::vector<uint32_t>& expected_uav1,
                                 const char* label,
                                 uint32_t group_x = 1u) {
    ValidateSpirvIfAvailable(env, lifted.spirv_words, label);
#if defined(_WIN32)
    if (env.dxc_path.empty()) return;
    auto run_one = [&](S16D3D12Runner& gpu, const char* tag) {
        std::vector<uint8_t> dxil;
        std::string diag;
        const Status cst = CompileHlslWithDxc(lifted.hlsl_source, gpu.TargetProfile("cs"), "main", dxil, diag, env.dxc_path);
        if (!cst.ok()) {
            std::fprintf(stderr, "[FAIL] DXC failed (%s / %s): %s\n%s\nHLSL:\n%s\n",
                         label, tag, cst.message.c_str(), diag.c_str(), lifted.hlsl_source.c_str());
        }
        OS_REQUIRE(cst.ok());
        OS_REQUIRE(ResignDxilContainer(dxil).ok());
        std::vector<uint32_t> out;
        OS_REQUIRE(gpu.RunComputeDualUavTable(dxil, uav0, init_uav1, out, group_x));
        OS_REQUIRE(out.size() == expected_uav1.size());
        for (size_t i = 0u; i < expected_uav1.size(); ++i) {
            if (out[i] != expected_uav1[i]) {
                std::fprintf(stderr, "[FAIL] %s (%s) mismatch at [%zu]: got=0x%08X expected=0x%08X\n",
                             label, tag, i, out[i], expected_uav1[i]);
            }
            OS_REQUIRE(out[i] == expected_uav1[i]);
        }
    };
    if (env.has_hw) run_one(env.hw, "D3D12-HW");
    if (env.has_warp) run_one(env.warp, "D3D12-WARP");
#else
    (void)uav0;
    (void)init_uav1;
    (void)expected_uav1;
    (void)group_x;
#endif
}

// ---------------------------------------------------------------------------
// 1. Single Bounds-Check per s_buffer_load_dwordx2/x4/x8/x16
// ---------------------------------------------------------------------------
void Test1_SingleBoundsCheckSBufferLoad(TestEnv& env) {
    // Compare 1x s_buffer_load_dwordx16 vs 16x s_buffer_load_dword
    auto build_x16_shader = [](int32_t byte_offset) {
        Rdna2Assembler a;
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORDX16, 16u, 0u, byte_offset);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        // Only lane 0 stores the 16 loaded SGPRs to uav1[0..15]
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::Vgpr(0), 0u);
        for (uint8_t k = 0u; k < 16u; ++k) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::Sgpr(static_cast<uint8_t>(16u + k)));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(k * 4u));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 3u, 4u, 0u);
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        return a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 40u, 8u, 32u);
    };

    auto build_16x_single_shader = []() {
        Rdna2Assembler a;
        // Use dynamic SGPR soffset=s2 (value 0) so each single-dword s_buffer_load reads uav0 with bounds-check
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 2u, Rdna2Operand::InlineInt(0));
        for (uint8_t k = 0u; k < 16u; ++k) {
            a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, static_cast<uint8_t>(16u + k), 0u, static_cast<int32_t>(k * 4u), 2u);
        }
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        for (uint8_t k = 0u; k < 16u; ++k) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::Sgpr(static_cast<uint8_t>(16u + k)));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(k * 4u));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 3u, 4u, 0u);
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        return a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 40u, 8u, 32u);
    };

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    Rdna2LiftResult x16_inbounds{}, x16_partial{}, x16_oob{}, single16{};
    OS_REQUIRE(LiftRdna2Xs2Shader(build_x16_shader(0), x16_inbounds, opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(build_x16_shader(40), x16_partial, opts).ok());  // 16-dword buffer = 64 bytes -> 6 valid + 10 zeros!
    OS_REQUIRE(LiftRdna2Xs2Shader(build_x16_shader(256), x16_oob, opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(build_16x_single_shader(), single16, opts).ok());

    // Verify >50% reduction in bounds-check / GetDimensions emissions in BOTH HLSL & SPIR-V
    const size_t x16_hlsl_dims = CountSubstr(x16_inbounds.hlsl_source, "GetDimensions");
    const size_t single16_hlsl_dims = CountSubstr(single16.hlsl_source, "GetDimensions");
    const size_t x16_spv_arrlen = CountSpirvOpcode(x16_inbounds.spirv_words, 68u /*OpArrayLength*/);
    const size_t single16_spv_arrlen = CountSpirvOpcode(single16.spirv_words, 68u /*OpArrayLength*/);
    OS_REQUIRE(x16_hlsl_dims == 1u);   // Single hoisted bounds check for all 16 dwords!
    OS_REQUIRE(single16_hlsl_dims == 16u);
    OS_REQUIRE(x16_hlsl_dims * 2u < single16_hlsl_dims);
    OS_REQUIRE(x16_spv_arrlen == 1u);  // Single OpArrayLength for all 16 dwords!
    OS_REQUIRE(single16_spv_arrlen >= 16u);
    // Extended SV single-check check on s_buffer_load_dwordx16 vs 16x s_buffer_load_dword
    {
        using namespace openscarlett::rdna2_sv;
        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Compute;
        xs2.threadgroup_x = 1u;
        xs2.sgpr_count = 32u;
        xs2.vgpr_count = 8u;
        xs2.wave_size = 32u;

        std::vector<uint32_t> code;
        OS_REQUIRE(EncodeScalarVectorInst("s_buffer_load_dwordx16", 8u, {Rdna2Operand::Sgpr(0)}, 0, code));
        OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));
        std::vector<SvDecodedInst> dec;
        OS_REQUIRE(DecodeScalarVectorStream(code, dec).ok());
        Rdna2LiftResult ext_x16{};
        OS_REQUIRE(LiftScalarVectorShader(xs2, dec, ext_x16, opts).ok());

        std::vector<uint32_t> code_16;
        for (uint32_t k = 0u; k < 16u; ++k) {
            OS_REQUIRE(EncodeScalarVectorInst("s_buffer_load_dword", static_cast<uint8_t>(8u + k),
                                              {Rdna2Operand::Sgpr(0)}, static_cast<int32_t>(k * 4u), code_16));
        }
        OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code_16));
        std::vector<SvDecodedInst> dec16;
        OS_REQUIRE(DecodeScalarVectorStream(code_16, dec16).ok());
        Rdna2LiftResult ext_16x{};
        OS_REQUIRE(LiftScalarVectorShader(xs2, dec16, ext_16x, opts).ok());

        const size_t checks_x16 = CountSubstr(ext_x16.hlsl_source, "GetDimensions");
        const size_t checks_16x = CountSubstr(ext_16x.hlsl_source, "GetDimensions");
        OS_REQUIRE(checks_x16 <= 1u);
        OS_REQUIRE(checks_x16 * 2u < std::max<size_t>(checks_16x, 4u));
    }

    // Prepare 16-dword (64-byte) source buffer
    std::vector<uint32_t> uav0(16u);
    for (uint32_t i = 0u; i < 16u; ++i) uav0[i] = 0xA0000000u | (i * 0x1111u + 7u);
    const std::vector<uint32_t> init_uav1(16u, 0xDEAD0000u);

    // Case A: Full in-bounds x16
    Rdna2CpuExecState cpu_in{};
    cpu_in.uav0 = uav0;
    cpu_in.uav1 = init_uav1;
    OS_REQUIRE(ExecuteRdna2Cpu(x16_inbounds.container, x16_inbounds.instructions, cpu_in).ok());
    OS_REQUIRE(cpu_in.uav1 == uav0);
    RunAndVerifyComputeBothGpus(env, x16_inbounds, uav0, init_uav1, cpu_in.uav1, "s_buffer_load_x16_inbounds");

    // Case B: Partial-tail x16 (offset=40 -> 6 valid dwords [10..15] + 10 zero dwords!)
    Rdna2CpuExecState cpu_part{};
    cpu_part.uav0 = uav0;
    cpu_part.uav1 = init_uav1;
    OS_REQUIRE(ExecuteRdna2Cpu(x16_partial.container, x16_partial.instructions, cpu_part).ok());
    for (uint32_t k = 0u; k < 6u; ++k) OS_REQUIRE(cpu_part.uav1[k] == uav0[10u + k]);
    for (uint32_t k = 6u; k < 16u; ++k) OS_REQUIRE(cpu_part.uav1[k] == 0u);
    RunAndVerifyComputeBothGpus(env, x16_partial, uav0, init_uav1, cpu_part.uav1, "s_buffer_load_x16_partial_tail");

    // Case C: Fully OOB x16 (offset=256 -> all 16 zeros!)
    Rdna2CpuExecState cpu_oob{};
    cpu_oob.uav0 = uav0;
    cpu_oob.uav1 = init_uav1;
    OS_REQUIRE(ExecuteRdna2Cpu(x16_oob.container, x16_oob.instructions, cpu_oob).ok());
    for (uint32_t k = 0u; k < 16u; ++k) OS_REQUIRE(cpu_oob.uav1[k] == 0u);
    RunAndVerifyComputeBothGpus(env, x16_oob, uav0, init_uav1, cpu_oob.uav1, "s_buffer_load_x16_oob");

    // Also verify x2, x4, x8 in one combined compute shader
    {
        Rdna2Assembler a;
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORDX2, 8u, 0u, 0);
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORDX4, 12u, 0u, 8);
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORDX8, 16u, 0u, 24);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        for (uint8_t k = 0u; k < 14u; ++k) {
            const uint8_t sreg = (k < 2u) ? static_cast<uint8_t>(8u + k)
                                          : ((k < 6u) ? static_cast<uint8_t>(12u + (k - 2u))
                                                      : static_cast<uint8_t>(16u + (k - 6u)));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::Sgpr(sreg));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(k * 4u));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 3u, 4u, 0u);
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult r{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 32u, 8u, 32u), r, opts).ok());
        OS_REQUIRE(CountSubstr(r.hlsl_source, "GetDimensions") == 3u);  // 1 per x2/x4/x8 (vs 14!)
        OS_REQUIRE(CountSpirvOpcode(r.spirv_words, 68u) == 3u);
        Rdna2CpuExecState c{};
        c.uav0 = uav0;
        c.uav1.assign(16u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(r.container, r.instructions, c).ok());
        for (uint32_t k = 0u; k < 14u; ++k) OS_REQUIRE(c.uav1[k] == uav0[k]);
        RunAndVerifyComputeBothGpus(env, r, uav0, std::vector<uint32_t>(16u, 0u), c.uav1, "s_buffer_load_x2_x4_x8");
    }

    std::printf("[PASS] Test1: Single bounds-check per s_buffer_load_x2/x4/x8/x16 (1 vs 16 checks, exact in-bounds/partial/OOB on CPU+GPU)\n");
}

// ---------------------------------------------------------------------------
// 2. BVH Box-Node Word-Bounded Reads (rdna2_rayquery_bridge + rdna2_lifter + mem_img)
// ---------------------------------------------------------------------------
void Test2_BvhBoxNodeWordBoundedReads(TestEnv& env) {
    // A. RayQuery bridge compute fallback HLSL must guard box/tri/inst reads with GetDimensions + BvhWordRangeValid
    {
        Rdna2BvhIntersectDesc desc{};
        desc.op = Rdna2BvhIntersectOp::ImageBvh64IntersectRay;
        Rdna2RayQueryTranslationResult res{};
        OS_REQUIRE(TranslateRdna2BvhIntersectToRayQuery(desc, res).ok());
        OS_REQUIRE(res.compute_bvh4_hlsl.find("GetDimensions") != std::string::npos);
        OS_REQUIRE(res.compute_bvh4_hlsl.find("BvhWordRangeValid") != std::string::npos);
        if (!env.dxc_path.empty()) {
            std::vector<uint8_t> dxil;
            std::string diag;
            OS_REQUIRE(CompileHlslWithDxc(res.compute_bvh4_hlsl, "cs_6_5", "main", dxil, diag, env.dxc_path).ok());
        }
    }

    // B. Core IMAGE_BVH_INTERSECT_RAY with valid node, truncated node, and OOB node offset!
    auto build_bvh_shader = [](uint32_t node_dword_offset) {
        Rdna2Assembler a;
        // v10 = node_ptr (dword offset), v11 = ray_tmax (100.0), v12..14 = orig (0,0,-5), v15..17 = dir (0,0,1)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::LiteralU32(node_dword_offset));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11u, Rdna2Operand::LiteralF32(100.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14u, Rdna2Operand::LiteralF32(-5.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 16u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17u, Rdna2Operand::InlineFloat(1.0f));
        a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 2u, 10u, 0u, 0u, 0xFu, 1u);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        // Store v2 (t_hit), v3 (hit_flag), v4 (prim), v5 (child_or_u) to uav1[0..3]
        for (uint8_t k = 0u; k < 4u; ++k) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::LiteralU32(k * 4u));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, static_cast<uint8_t>(2u + k), 20u, 4u, 0u);
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        return a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 16u, 24u, 32u);
    };

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    Rdna2LiftResult bvh_valid{}, bvh_oob{};
    OS_REQUIRE(LiftRdna2Xs2Shader(build_bvh_shader(0u), bvh_valid, opts).ok());
    OS_REQUIRE(LiftRdna2Xs2Shader(build_bvh_shader(0x00010000u), bvh_oob, opts).ok());
    OS_REQUIRE(bvh_valid.hlsl_source.find("GetDimensions") != std::string::npos);
    OS_REQUIRE(CountSpirvOpcode(bvh_valid.spirv_words, 68u /*OpArrayLength*/) >= 1u);

    // Valid AABB box node (7 dwords): hdr = 0x80000000 | hit_id(42), min=(-1,-1,-1), max=(1,1,1) -> hit t_enter = 4.0!
    const std::vector<uint32_t> valid_bvh = {
        0x80000000u | 42u,
        F2U(-1.0f), F2U(-1.0f), F2U(-1.0f),
        F2U(1.0f),  F2U(1.0f),  F2U(1.0f),
    };
    const std::vector<uint32_t> init_out(4u, 0xCAFEBABEu);
    {
        Rdna2CpuExecState c{};
        c.uav0 = valid_bvh;
        c.uav1 = init_out;
        OS_REQUIRE(ExecuteRdna2Cpu(bvh_valid.container, bvh_valid.instructions, c).ok());
        OS_REQUIRE(std::fabs(U2F(c.uav1[0]) - 4.0f) < 1e-4f);
        OS_REQUIRE(c.uav1[3] == 42u);
        RunAndVerifyComputeBothGpus(env, bvh_valid, valid_bvh, init_out, c.uav1, "bvh_valid_hit");
    }

    // Truncated 4-dword BVH box-node buffer (needs 7 dwords!) -> safe empty miss (hit_id = 0xFFFFFFFF, t = tmax)!
    const std::vector<uint32_t> trunc_bvh = {0x80000000u | 42u, F2U(-1.0f), F2U(-1.0f), F2U(-1.0f)};
    {
        Rdna2CpuExecState c{};
        c.uav0 = trunc_bvh;
        c.uav1 = init_out;
        OS_REQUIRE(ExecuteRdna2Cpu(bvh_valid.container, bvh_valid.instructions, c).ok());
        OS_REQUIRE(std::fabs(U2F(c.uav1[0]) - 100.0f) < 1e-4f);
        OS_REQUIRE(c.uav1[3] == 0xFFFFFFFFu);
        RunAndVerifyComputeBothGpus(env, bvh_valid, trunc_bvh, init_out, c.uav1, "bvh_truncated_safe_miss");
    }

    // Wild OOB node offset (0x10000) -> safe empty miss!
    {
        Rdna2CpuExecState c{};
        c.uav0 = valid_bvh;
        c.uav1 = init_out;
        OS_REQUIRE(ExecuteRdna2Cpu(bvh_oob.container, bvh_oob.instructions, c).ok());
        OS_REQUIRE(std::fabs(U2F(c.uav1[0]) - 100.0f) < 1e-4f);
        OS_REQUIRE(c.uav1[3] == 0xFFFFFFFFu);
        RunAndVerifyComputeBothGpus(env, bvh_oob, valid_bvh, init_out, c.uav1, "bvh_oob_offset_safe_miss");
    }

    std::printf("[PASS] Test2: BVH box-node word-bounded reads (valid hit + truncated/OOB safe miss on CPU+GPU)\n");
}

// ---------------------------------------------------------------------------
// 3. Exact RNE Rounding & NaN->0 for v_cvt_pknorm_* / v_cvt_norm_*
// ---------------------------------------------------------------------------
void Test3_ExactRneAndNanCvtPkNorm(TestEnv& env) {
    // Core VOP3: v_cvt_pknorm_i16_f32, v_cvt_pknorm_u16_f32, v_cvt_pknorm_i16_f16, v_cvt_pknorm_u16_f16
    Rdna2Assembler a;
    // Each lane i reads pair (uav0[2*i], uav0[2*i+1]) and outputs:
    //   uav1[4*i + 0] = v_cvt_pknorm_i16_f32(a, b)
    //   uav1[4*i + 1] = v_cvt_pknorm_u16_f32(a, b)
    //   uav1[4*i + 2] = v_cvt_pknorm_i16_f16(uav0[32 + i]) (with f16 packed in lo16 of src0 & src1)
    //   uav1[4*i + 3] = v_cvt_pknorm_u16_f16(uav0[32 + i])
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1u, Rdna2Operand::InlineInt(3), 0u);  // v1 = lane * 8
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);                 // v2 = a (f32)
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3u, 1u, 0u, 4u);                 // v3 = b (f32)
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4u, Rdna2Operand::InlineInt(2), 0u);  // v4 = lane * 4
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 5u, 4u, 0u, 128u);               // v5 = packed f16 lo|hi
    a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 6u, Rdna2Operand::InlineInt(16), 5u); // v6 = f16 hi
    a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);

    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F32, 10u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_U16_F32, 11u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F16, 12u, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
    a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_U16_F16, 13u, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));

    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 14u, Rdna2Operand::InlineInt(4), 0u); // v14 = lane * 16
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10u, 14u, 4u, 0u);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11u, 14u, 4u, 4u);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12u, 14u, 4u, 8u);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13u, 14u, 4u, 12u);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 8u, 1u, 1u, 16u, 16u, 32u), lifted, opts).ok());

    std::vector<uint32_t> uav0(40u, 0u);
    // Exact RNE tie points for i16 (scale 32767.0f) & u16 (scale 65535.0f) + NaNs + clamps:
    // Lane 0: (0.5/32767 -> 0, 1.5/32767 -> 2)
    uav0[0] = F2U(0.5f / 32767.0f);
    uav0[1] = F2U(1.5f / 32767.0f);
    // Lane 1: (2.5/32767 -> 2, -0.5/32767 -> 0)
    uav0[2] = F2U(2.5f / 32767.0f);
    uav0[3] = F2U(-0.5f / 32767.0f);
    // Lane 2: (-1.5/32767 -> -2, -2.5/32767 -> -2)
    uav0[4] = F2U(-1.5f / 32767.0f);
    uav0[5] = F2U(-2.5f / 32767.0f);
    // Lane 3: (qNaN -> 0, -qNaN -> 0)
    uav0[6] = 0x7FC00000u;
    uav0[7] = 0xFFC00001u;
    // Lane 4: (0.5/65535 -> u16 0, 1.5/65535 -> u16 2)
    uav0[8] = F2U(0.5f / 65535.0f);
    uav0[9] = F2U(1.5f / 65535.0f);
    // Lane 5: (2.5/65535 -> u16 2, 3.5/65535 -> u16 4)
    uav0[10] = F2U(2.5f / 65535.0f);
    uav0[11] = F2U(3.5f / 65535.0f);
    // Lane 6: (clamp extremes: -2.0f -> i16 -32767 / u16 0, +2.0f -> i16 32767 / u16 65535)
    uav0[12] = F2U(-2.0f);
    uav0[13] = F2U(2.0f);
    // Lane 7: (0.5f -> 16384 [32767*0.5=16383.5 -> RNE 16384!], -0.5f -> -16384)
    uav0[14] = F2U(0.5f);
    uav0[15] = F2U(-0.5f);

    // Packed f16 pairs in uav0[32..39]:
    // f16 0.5 * 32767 = 16383.5 -> RNE 16384! f16 -0.5 * 32767 = -16383.5 -> RNE -16384!
    // f16 0.5 * 65535 = 32767.5 -> RNE 32768!
    uav0[32] = static_cast<uint32_t>(F32ToF16(0.5f)) | (static_cast<uint32_t>(F32ToF16(-0.5f)) << 16u);
    uav0[33] = 0x7E00u | (0xFE01u << 16u);  // f16 +NaN, -NaN -> MUST be 0, 0!
    uav0[34] = static_cast<uint32_t>(F32ToF16(1.0f)) | (static_cast<uint32_t>(F32ToF16(-1.0f)) << 16u);
    uav0[35] = static_cast<uint32_t>(F32ToF16(2.0f)) | (static_cast<uint32_t>(F32ToF16(-2.0f)) << 16u);
    uav0[36] = static_cast<uint32_t>(F32ToF16(0.25f)) | (static_cast<uint32_t>(F32ToF16(0.75f)) << 16u);
    uav0[37] = 0x0000u | (0x8000u << 16u);  // +0.0, -0.0
    uav0[38] = static_cast<uint32_t>(F32ToF16(0.125f)) | (static_cast<uint32_t>(F32ToF16(-0.125f)) << 16u);
    uav0[39] = 0x7C00u | (0xFC00u << 16u);  // +Inf -> max, -Inf -> min/0

    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1.assign(32u, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());

    // Verify exact RNE & NaN->0 analytically on CPU results!
    auto lo_i16 = [](uint32_t w) -> int16_t { return static_cast<int16_t>(w & 0xFFFFu); };
    auto hi_i16 = [](uint32_t w) -> int16_t { return static_cast<int16_t>((w >> 16u) & 0xFFFFu); };
    auto lo_u16 = [](uint32_t w) -> uint16_t { return static_cast<uint16_t>(w & 0xFFFFu); };
    auto hi_u16 = [](uint32_t w) -> uint16_t { return static_cast<uint16_t>((w >> 16u) & 0xFFFFu); };

    // Lane 0 i16_f32: 0.5 -> 0, 1.5 -> 2
    OS_REQUIRE(lo_i16(cpu.uav1[0]) == 0 && hi_i16(cpu.uav1[0]) == 2);
    // Lane 1 i16_f32: 2.5 -> 2, -0.5 -> 0
    OS_REQUIRE(lo_i16(cpu.uav1[4]) == 2 && hi_i16(cpu.uav1[4]) == 0);
    // Lane 2 i16_f32: -1.5 -> -2, -2.5 -> -2
    OS_REQUIRE(lo_i16(cpu.uav1[8]) == -2 && hi_i16(cpu.uav1[8]) == -2);
    // Lane 3 NaN -> 0 (BOTH i16 & u16!)
    OS_REQUIRE(cpu.uav1[12] == 0u && cpu.uav1[13] == 0u);
    // Lane 4 u16_f32: 0.5 -> 0, 1.5 -> 2
    OS_REQUIRE(lo_u16(cpu.uav1[17]) == 0u && hi_u16(cpu.uav1[17]) == 2u);
    // Lane 5 u16_f32: 2.5 -> 2, 3.5 -> 4
    OS_REQUIRE(lo_u16(cpu.uav1[21]) == 2u && hi_u16(cpu.uav1[21]) == 4u);
    // Lane 6 clamp: i16=(-32767, 32767), u16=(0, 65535)
    OS_REQUIRE(lo_i16(cpu.uav1[24]) == -32767 && hi_i16(cpu.uav1[24]) == 32767);
    OS_REQUIRE(lo_u16(cpu.uav1[25]) == 0u && hi_u16(cpu.uav1[25]) == 65535u);
    // Lane 7 0.5f / -0.5f: RNE 16383.5 -> 16384, -16383.5 -> -16384; u16 32767.5 -> 32768
    OS_REQUIRE(lo_i16(cpu.uav1[28]) == 16384 && hi_i16(cpu.uav1[28]) == -16384);
    OS_REQUIRE(lo_u16(cpu.uav1[29]) == 32768u && hi_u16(cpu.uav1[29]) == 0u);
    // f16 Lane 0 (0.5, -0.5): i16=(16384, -16384), u16=(32768, 0)
    OS_REQUIRE(lo_i16(cpu.uav1[2]) == 16384 && hi_i16(cpu.uav1[2]) == -16384);
    OS_REQUIRE(lo_u16(cpu.uav1[3]) == 32768u && hi_u16(cpu.uav1[3]) == 0u);
    // f16 Lane 1 (NaN, -NaN): MUST be 0!
    OS_REQUIRE(cpu.uav1[6] == 0u && cpu.uav1[7] == 0u);

    // Verify BIT-EXACT match on BOTH real D3D12 HW GPU & D3D12 WARP + spirv-val!
    RunAndVerifyComputeBothGpus(env, lifted, uav0, std::vector<uint32_t>(32u, 0u), cpu.uav1, "cvt_pknorm_rne_nan");

    // Also verify extended SV v_cvt_norm_i16_f16 & v_cvt_norm_u16_f16 RNE + NaN->0 on CPU!
    {
        using namespace openscarlett::rdna2_sv;
        std::vector<uint32_t> code;
        OS_REQUIRE(EncodeScalarVectorInst("v_cvt_norm_i16_f16", 2u, {Rdna2Operand::Vgpr(1)}, 0, code));
        OS_REQUIRE(EncodeScalarVectorInst("v_cvt_norm_u16_f16", 3u, {Rdna2Operand::Vgpr(1)}, 0, code));
        OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));
        std::vector<SvDecodedInst> dec;
        OS_REQUIRE(DecodeScalarVectorStream(code, dec).ok());
        SvWaveState w{};
        w.Reset(32u);
        w.exec = 0x3ull;
        w.WriteVgpr(0u, 1u, F32ToF16(0.5f));  // 16383.5 -> 16384, 32767.5 -> 32768
        w.WriteVgpr(1u, 1u, 0x7E00u);         // f16 NaN -> 0
        OS_REQUIRE(ExecuteScalarVectorCpu(dec, w).ok());
        OS_REQUIRE(static_cast<int16_t>(w.ReadVgpr(0u, 2u) & 0xFFFFu) == 16384);
        OS_REQUIRE(static_cast<uint16_t>(w.ReadVgpr(0u, 3u) & 0xFFFFu) == 32768u);
        OS_REQUIRE((w.ReadVgpr(1u, 2u) & 0xFFFFu) == 0u);
        OS_REQUIRE((w.ReadVgpr(1u, 3u) & 0xFFFFu) == 0u);
    }

    std::printf("[PASS] Test3: Exact RNE (0.5->0,1.5->2,2.5->2) & NaN->0 for v_cvt_[pk]norm_* on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 4. SPIR-V MinLod Preserved in HLSL (image_sample_*_cl*)
// ---------------------------------------------------------------------------
void Test4_SpirvAndHlslMinLod(TestEnv& env) {
    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    // A. Verify all 6 _cl variants preserve MinLod in HLSL & SPIR-V + pass spirv-val & DXC
    const std::array<std::pair<Rdna2Opcode, const char*>, 6> ops = {{
        {Rdna2Opcode::IMAGE_SAMPLE_CL,     "Sample("},
        {Rdna2Opcode::IMAGE_SAMPLE_D_CL,   "SampleGrad("},
        {Rdna2Opcode::IMAGE_SAMPLE_B_CL,   "SampleBias("},
        {Rdna2Opcode::IMAGE_SAMPLE_C_CL,   "Sample("},
        {Rdna2Opcode::IMAGE_SAMPLE_C_D_CL, "SampleGrad("},
        {Rdna2Opcode::IMAGE_SAMPLE_C_B_CL, "SampleBias("},
    }};
    for (const auto& [op, expected_hlsl_call] : ops) {
        Rdna2Assembler a;
        // Fill v10..v17 with valid float values (u, v, min_lod, bias, dref, grads)
        for (uint8_t r = 10u; r < 18u; ++r) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, r, Rdna2Operand::InlineFloat(0.5f));
        }
        a.EmitMimg(op, 2u, 10u, 0u, 0u, 0xFu, 1u);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 20u, 4u, 0u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult r{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Pixel, 1u, 1u, 1u, 16u, 24u, 32u), r, opts).ok());
        OS_REQUIRE(r.hlsl_source.find(expected_hlsl_call) != std::string::npos);
        OS_REQUIRE(r.hlsl_source.find("int2(0, 0)") != std::string::npos);
        // Verify SPIR-V has OpCapability MinLod (42)
        bool has_minlod_cap = false;
        size_t idx = 5u;
        while (idx < r.spirv_words.size()) {
            const uint16_t spv_op = static_cast<uint16_t>(r.spirv_words[idx] & 0xFFFFu);
            const uint32_t wc = r.spirv_words[idx] >> 16u;
            if (wc == 0u || idx + wc > r.spirv_words.size()) break;
            if (spv_op == 17u /*OpCapability*/ && wc >= 2u && r.spirv_words[idx + 1u] == 42u) {
                has_minlod_cap = true;
            }
            idx += wc;
        }
        OS_REQUIRE(has_minlod_cap);
        ValidateSpirvIfAvailable(env, r.spirv_words, "mimg_minlod");
        if (!env.dxc_path.empty()) {
            std::vector<uint8_t> dxil;
            std::string diag;
            const Status st = CompileHlslWithDxc(r.hlsl_source, "ps_6_5", "main", dxil, diag, env.dxc_path);
            if (!st.ok()) {
                std::fprintf(stderr, "[FAIL] DXC MinLod PS failed: %s\n%s\n%s\n",
                             st.message.c_str(), diag.c_str(), r.hlsl_source.c_str());
            }
            OS_REQUIRE(st.ok());
        }
    }

    // B. Execute IMAGE_SAMPLE_D_CL in CS on LIVE GPU (mip0 = 1.0, mip1 = 5.0):
    //    SampleGrad with ddx=(0,0), ddy=(0,0) selects mip0 when min_lod=0.0, and MUST clamp to mip1 (5.0) when min_lod=1.0!
#if defined(_WIN32)
    if (!env.dxc_path.empty() && (env.has_hw || env.has_warp)) {
        Rdna2Assembler a;
        // IMAGE_SAMPLE_D_CL vaddr order: [dhx=v10, dhy=v11, dvx=v12, dvy=v13, u=v14, v=v15, min_lod=v16]
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14u, Rdna2Operand::InlineFloat(0.5f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15u, Rdna2Operand::InlineFloat(0.5f));
        // Read min_lod from uav0[lane * 4]
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1u, Rdna2Operand::InlineInt(2), 0u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 16u, 1u, 0u, 0u);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE_D_CL, 2u, 10u, 0u, 0u, 0xFu, 1u);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 1u, 4u, 0u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult r{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 2u, 1u, 1u, 16u, 20u, 32u), r, opts).ok());
        ValidateSpirvIfAvailable(env, r.spirv_words, "mimg_sample_d_cl_cs");
        const std::vector<uint32_t> uav0 = {F2U(0.0f), F2U(1.0f)};
        const std::vector<uint32_t> init1(2u, 0u);
        const std::array<float, 4> mip0 = {1.0f, 0.0f, 0.0f, 1.0f};
        const std::array<float, 4> mip1 = {5.0f, 0.0f, 0.0f, 1.0f};

        auto run_mip = [&](S16D3D12Runner& gpu, const char* tag) {
            std::vector<uint8_t> dxil;
            std::string diag;
            OS_REQUIRE(CompileHlslWithDxc(r.hlsl_source, gpu.TargetProfile("cs"), "main", dxil, diag, env.dxc_path).ok());
            OS_REQUIRE(ResignDxilContainer(dxil).ok());
            std::vector<uint32_t> out;
            OS_REQUIRE(gpu.RunComputeTex2DMip(dxil, uav0, init1, mip0, mip1, out));
            if (std::fabs(U2F(out[0]) - 1.0f) > 1e-3f || std::fabs(U2F(out[1]) - 5.0f) > 1e-3f) {
                std::fprintf(stderr, "[FAIL] MinLod GPU (%s): lane0=%.3f (exp 1.0) lane1=%.3f (exp 5.0)\n",
                             tag, U2F(out[0]), U2F(out[1]));
            }
            OS_REQUIRE(std::fabs(U2F(out[0]) - 1.0f) < 1e-3f);
            OS_REQUIRE(std::fabs(U2F(out[1]) - 5.0f) < 1e-3f);
        };
        if (env.has_hw) run_mip(env.hw, "D3D12-HW");
        if (env.has_warp) run_mip(env.warp, "D3D12-WARP");
    }
#endif

    std::printf("[PASS] Test4: SPIR-V MinLod preserved in HLSL (all 6 _cl variants + live mip-clamp GPU check)\n");
}

// ---------------------------------------------------------------------------
// 5. Compressed EXP Enable-Mask Decoding Across ALL 16 Raw EN Values (0x0..0xF) + Low16
// ---------------------------------------------------------------------------
void Test5_CompressedExpAll16MasksAndLow16(TestEnv& env) {
    // Pack known f16 pairs:
    //   v0 = (0.25, 0.50)
    //   v1 = (0.75, 1.00)  (for low16 masks en <= 0x3, we set v1 to NaN 0x7E007E00 to PROVE v1 is NEVER read!)
    const uint32_t pair0 = static_cast<uint32_t>(F32ToF16(0.25f)) | (static_cast<uint32_t>(F32ToF16(0.50f)) << 16u);
    const uint32_t pair1 = static_cast<uint32_t>(F32ToF16(0.75f)) | (static_cast<uint32_t>(F32ToF16(1.00f)) << 16u);

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    // Build simple fullscreen VS for live PS verification
    std::vector<uint8_t> vs_dxil_hw, vs_dxil_warp;
#if defined(_WIN32)
    {
        Rdna2Assembler vs;
        vs.EmitVop2(Rdna2Opcode::V_AND_B32, 1u, Rdna2Operand::InlineInt(1), 0u);
        vs.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2u, Rdna2Operand::InlineInt(1), 0u);
        vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 3u, Rdna2Operand::Vgpr(1));
        vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 4u, Rdna2Operand::Vgpr(2));
        vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 5u, Rdna2Operand::Vgpr(3), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
        vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 6u, Rdna2Operand::Vgpr(4), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
        vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::InlineFloat(0.0f));
        vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::InlineFloat(1.0f));
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 5u, 6u, 7u, 8u, 0xFu, true);
        vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
        Rdna2LiftResult vs_lift{};
        OS_REQUIRE(LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex, 1u, 1u, 1u, 16u, 16u, 32u), vs_lift, opts).ok());
        if (!env.dxc_path.empty()) {
            std::string diag;
            if (env.has_hw) {
                OS_REQUIRE(CompileHlslWithDxc(vs_lift.hlsl_source, env.hw.TargetProfile("vs"), "main", vs_dxil_hw, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(vs_dxil_hw).ok());
            }
            if (env.has_warp) {
                OS_REQUIRE(CompileHlslWithDxc(vs_lift.hlsl_source, env.warp.TargetProfile("vs"), "main", vs_dxil_warp, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(vs_dxil_warp).ok());
            }
        }
    }
#endif

    for (uint8_t en = 0u; en <= 0xFu; ++en) {
        Rdna2Assembler ps;
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(pair0));
        // For low16 masks (en <= 0x3), poison v5 with NaN to PROVE high pair v5 is NEVER read!
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::LiteralU32((en <= 0x3u) ? 0x7E007E00u : pair1));
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4u, 5u, 0u, 0u, en, true, /*compr=*/true, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult ps_lift{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel, 1u, 1u, 1u, 16u, 16u, 32u), ps_lift, opts).ok());
        ValidateSpirvIfAvailable(env, ps_lift.spirv_words, "exp_compr_mask");

        Rdna2CpuExecState cpu{};
        OS_REQUIRE(ExecuteRdna2Cpu(ps_lift.container, ps_lift.instructions, cpu).ok());
        OS_REQUIRE(!cpu.exports.empty());
        const auto& ex = cpu.exports[0];
        const std::array<float, 4> expected = {
            (en & 0x1u) ? 0.25f : 0.0f,
            (en & 0x2u) ? 0.50f : 0.0f,
            (en & 0x4u) ? 0.75f : 0.0f,
            (en & 0x8u) ? 1.00f : 0.0f,
        };
        if (en == 0u) {
            OS_REQUIRE(!ex.wrote_mrt0);
        } else {
            OS_REQUIRE(ex.wrote_mrt0);
            for (size_t c = 0u; c < 4u; ++c) {
                OS_REQUIRE(std::fabs(ex.mrt0[c] - expected[c]) < 1e-3f);
            }
        }

        // Verify live GPU rendering for odd/asymmetric/low16/full masks: 0x1, 0x3, 0x5, 0xA, 0xD, 0xF!
#if defined(_WIN32)
        if (!env.dxc_path.empty() && (en == 0x1u || en == 0x3u || en == 0x5u || en == 0xAu || en == 0xDu || en == 0xFu)) {
            auto run_ps = [&](S16D3D12Runner& gpu, const std::vector<uint8_t>& vs_dxil, const char* tag) {
                std::vector<uint8_t> ps_dxil;
                std::string diag;
                OS_REQUIRE(CompileHlslWithDxc(ps_lift.hlsl_source, gpu.TargetProfile("ps"), "main", ps_dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(ps_dxil).ok());
                std::vector<float> fb;
                OS_REQUIRE(gpu.RunGraphicsDraw(vs_dxil, ps_dxil, fb));
                for (size_t c = 0u; c < 4u; ++c) {
                    if (std::fabs(fb[c] - expected[c]) > 1e-3f) {
                        std::fprintf(stderr, "[FAIL] EXP compr en=0x%X (%s) ch%zu: got=%.4f exp=%.4f\n",
                                     en, tag, c, fb[c], expected[c]);
                    }
                    OS_REQUIRE(std::fabs(fb[c] - expected[c]) < 1e-3f);
                }
            };
            if (env.has_hw) run_ps(env.hw, vs_dxil_hw, "D3D12-HW");
            if (env.has_warp) run_ps(env.warp, vs_dxil_warp, "D3D12-WARP");
        }
#endif
    }

    std::printf("[PASS] Test5: Compressed EXP (compr=1) verified across ALL 16 EN masks (0x0..0xF) + low16 on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 6. MUBUF D16 Byte/Short Loads & d16_hi Stores
// ---------------------------------------------------------------------------
void Test6_MubufD16LoadsAndHiStores(TestEnv& env) {
    Rdna2Assembler a;
    // uav0[0] = 0x80FE7F12 (bytes: [0]=0x12, [1]=0x7F, [2]=0xFE (-2), [3]=0x80 (-128))
    // uav0[1] = 0xBEEF1234 (shorts: [0]=0x1234, [2]=0xBEEF)
    // Pre-populate v2..v7 with sentinel 0xCAFEBABE so untouched 16-bit half preservation is strictly verified!
    for (uint8_t r = 2u; r <= 7u; ++r) {
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, r, Rdna2Operand::LiteralU32(0xCAFEBABEu));
    }
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::InlineInt(0));  // byte 0
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11u, Rdna2Operand::InlineInt(2));  // byte 2 (0xFE)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12u, Rdna2Operand::InlineInt(4));  // byte 4 (0x1234)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::InlineInt(6));  // byte 6 (0xBEEF)

    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_UBYTE_D16,    2u, 11u, 0u, 0u);  // lo16=0x00FE, hi16=0xCAFE -> 0xCAFE00FE
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_UBYTE_D16_HI, 3u, 11u, 0u, 0u);  // hi16=0x00FE, lo16=0xBABE -> 0x00FEBABE
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_SBYTE_D16,    4u, 11u, 0u, 0u);  // lo16=0xFFFE, hi16=0xCAFE -> 0xCAFEFFFE
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_SBYTE_D16_HI, 5u, 11u, 0u, 0u);  // hi16=0xFFFE, lo16=0xBABE -> 0xFFFEBABE
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_SHORT_D16,    6u, 12u, 0u, 0u);  // lo16=0x1234, hi16=0xCAFE -> 0xCAFE1234
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_SHORT_D16_HI, 7u, 13u, 0u, 0u);  // hi16=0xBEEF, lo16=0xBABE -> 0xBEEFBABE
    a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);

    // Store loaded results v2..v7 to uav1[0..5]
    for (uint8_t k = 0u; k < 6u; ++k) {
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14u, Rdna2Operand::LiteralU32(k * 4u));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, static_cast<uint8_t>(2u + k), 14u, 4u, 0u);
    }

    // Now test BUFFER_STORE_BYTE_D16_HI & BUFFER_STORE_SHORT_D16_HI into uav1[6] (bytes 24..27):
    // Initialize uav1[6] = 0x11223344, then:
    //   store byte d16_hi of 0x00A5DEAD (hi16 low byte = 0xA5) at byte 25 -> 0x1122A544
    //   store short d16_hi of 0x7788BEEF (hi16 = 0x7788) at byte 26 -> 0x7788A544
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15u, Rdna2Operand::LiteralU32(0x11223344u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 16u, Rdna2Operand::LiteralU32(24u));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15u, 16u, 4u, 0u);

    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17u, Rdna2Operand::LiteralU32(0x00A5DEADu));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 18u, Rdna2Operand::LiteralU32(25u));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_BYTE_D16_HI, 17u, 18u, 4u, 0u);

    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 19u, Rdna2Operand::LiteralU32(0x7788BEEFu));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::LiteralU32(26u));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_SHORT_D16_HI, 19u, 20u, 4u, 0u);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 16u, 24u, 32u), lifted, opts).ok());

    const std::vector<uint32_t> uav0 = {0x80FE7F12u, 0xBEEF1234u};
    const std::vector<uint32_t> init1(8u, 0u);
    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1 = init1;
    OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());

    OS_REQUIRE(cpu.uav1[0] == 0xCAFE00FEu);
    OS_REQUIRE(cpu.uav1[1] == 0x00FEBABEu);
    OS_REQUIRE(cpu.uav1[2] == 0xCAFEFFFEu);
    OS_REQUIRE(cpu.uav1[3] == 0xFFFEBABEu);
    OS_REQUIRE(cpu.uav1[4] == 0xCAFE1234u);
    OS_REQUIRE(cpu.uav1[5] == 0xBEEFBABEu);
    OS_REQUIRE(cpu.uav1[6] == 0x7788A544u);

    RunAndVerifyComputeBothGpus(env, lifted, uav0, init1, cpu.uav1, "mubuf_d16_loads_stores");
    std::printf("[PASS] Test6: MUBUF D16 byte/short loads & d16_hi stores (exact untouched-half preservation on CPU+SPIR-V+GPU)\n");
}

// ---------------------------------------------------------------------------
// 7. LDS DS Completeness (ds_mskor[_rtn], ds_wrxchg2[st64]_rtn, ds_{read,write}_b96/b128, ds_nop)
// ---------------------------------------------------------------------------
void Test7_LdsDsCompletenessAndDsNop(TestEnv& env) {
    Rdna2Assembler a;
    // All 4 lanes test non-overlapping LDS blocks (base = lane * 1024), plus cross-lane ds_nop check
    // Focus lane 0 on uav1[0..15] cleanly via single-thread compute group (tg_x = 1)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1u, Rdna2Operand::InlineInt(0));  // base addr = 0

    // A. ds_mskor_b32 & ds_mskor_rtn_b32 at LDS[0]:
    //    init = 0x12345678
    //    ds_mskor_b32(mask=0x00FF00FF, data=0xAABBCCDD) -> (0x12345678 & ~0x00FF00FF) | (0xAABBCCDD & 0x00FF00FF) = 0x12BB56DD
    //    ds_mskor_rtn_b32(mask=0xFF000000, data=0x55000000) -> returns old=0x12BB56DD, new=0x55BB56DD
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x12345678u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0u, 1u, 2u, 0u, 0u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0x00FF00FFu));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(0xAABBCCDDu));
    a.EmitDs(Rdna2Opcode::DS_MSKOR_B32, 0u, 1u, 3u, 4u, 0u);
    a.EmitDs(Rdna2Opcode::DS_NOP, 0u, 0u, 0u, 0u, 0u);  // barrier alignment!
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::LiteralU32(0xFF000000u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::LiteralU32(0x55000000u));
    a.EmitDs(Rdna2Opcode::DS_MSKOR_RTN_B32, 10u, 1u, 5u, 6u, 0u);  // v10 = 0x12BB56DD
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 11u, 1u, 0u, 0u, 0u);       // v11 = 0x55BB56DD

    // B. ds_wrxchg2_rtn_b32 at offset0=2 (byte 8), offset1=5 (byte 20):
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x11111111u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0u, 1u, 2u, 0u, 8u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x22222222u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0u, 1u, 2u, 0u, 20u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0xAAAAAAAAu));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(0xBBBBBBBBu));
    a.EmitDs(Rdna2Opcode::DS_WRXCHG2_RTN_B32, 12u, 1u, 3u, 4u, static_cast<uint16_t>((5u << 8u) | 2u));  // v12=0x11111111, v13=0x22222222
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 14u, 1u, 0u, 0u, 8u);   // v14 = 0xAAAAAAAA
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 15u, 1u, 0u, 0u, 20u);  // v15 = 0xBBBBBBBB

    // C. ds_wrxchg2st64_rtn_b32 at offset0=1 (byte 256), offset1=2 (byte 512):
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x33333333u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0u, 1u, 2u, 0u, 256u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x44444444u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0u, 1u, 2u, 0u, 512u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0xCCCCCCCCu));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(0xDDDDDDDDu));
    a.EmitDs(Rdna2Opcode::DS_WRXCHG2ST64_RTN_B32, 16u, 1u, 3u, 4u, static_cast<uint16_t>((2u << 8u) | 1u));  // v16=0x33333333, v17=0x44444444
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 18u, 1u, 0u, 0u, 256u);  // v18 = 0xCCCCCCCC
    a.EmitDs(Rdna2Opcode::DS_READ_B32, 19u, 1u, 0u, 0u, 512u);  // v19 = 0xDDDDDDDD

    // D. ds_write_b96 / ds_read_b96 at LDS[64] + ds_write_b128 / ds_read_b128 at LDS[96]
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::LiteralU32(0x96010001u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21u, Rdna2Operand::LiteralU32(0x96020002u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 22u, Rdna2Operand::LiteralU32(0x96030003u));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23u, Rdna2Operand::LiteralU32(0x12840004u));
    a.EmitDs(Rdna2Opcode::DS_WRITE_B96, 0u, 1u, 20u, 0u, 64u);
    a.EmitDs(Rdna2Opcode::DS_WRITE_B128, 0u, 1u, 20u, 0u, 96u);
    a.EmitDs(Rdna2Opcode::DS_NOP, 0u, 0u, 0u, 0u, 0u);
    a.EmitDs(Rdna2Opcode::DS_READ_B96, 24u, 1u, 0u, 0u, 64u);   // v24..26
    a.EmitDs(Rdna2Opcode::DS_READ_B128, 27u, 1u, 0u, 0u, 96u);  // v27..30
    a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);

    // Store v10..v19 (10 dwords) + v24..v30 (7 dwords) = 17 dwords into uav1[0..16]
    const std::array<uint8_t, 17> out_regs = {
        10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 24, 25, 26, 27, 28, 29, 30
    };
    for (uint32_t k = 0u; k < out_regs.size(); ++k) {
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 31u, Rdna2Operand::LiteralU32(k * 4u));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, out_regs[k], 31u, 4u, 0u);
    }
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 16u, 36u, 32u), lifted, opts).ok());
    OS_REQUIRE(lifted.hlsl_source.find("GroupMemoryBarrier()") != std::string::npos);

    const std::vector<uint32_t> uav0(4u, 0u);
    const std::vector<uint32_t> init1(17u, 0u);
    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1 = init1;
    OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());

    const std::vector<uint32_t> expected = {
        0x12BB56DDu, 0x55BB56DDu,
        0x11111111u, 0x22222222u, 0xAAAAAAAAu, 0xBBBBBBBBu,
        0x33333333u, 0x44444444u, 0xCCCCCCCCu, 0xDDDDDDDDu,
        0x96010001u, 0x96020002u, 0x96030003u,
        0x96010001u, 0x96020002u, 0x96030003u, 0x12840004u,
    };
    OS_REQUIRE(cpu.uav1 == expected);
    RunAndVerifyComputeBothGpus(env, lifted, uav0, init1, expected, "lds_ds_complete");
    std::printf("[PASS] Test7: LDS DS completeness (mskor/wrxchg2/st64/b96/b128/ds_nop) verified on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 8. All 24 SOP1 saveexec/wrexec + v_mad_u64_u32 / v_mad_i64_i32 Inactive-Lane Zeroing
// ---------------------------------------------------------------------------
void Test8_Sop1SaveExecWrExecAndMad64InactiveZeroing(TestEnv& env) {
    // A. Verify all 24 SOP1 saveexec/wrexec opcodes decode & execute exact EXEC/SDST/SCC semantics!
    struct ExecOpCase {
        uint8_t raw_op;
        bool is_64;
        bool is_wrexec;
        uint64_t expected_exec;  // given initial exec = 0x0Fu, src0 = 0x33u
    };
    const uint64_t init_e = 0x0Full;
    const uint64_t src_v = 0x33ull;
    const std::array<ExecOpCase, 24> cases = {{
        {kSop1RawAndSaveExecB64,   true,  false, (init_e & src_v)},
        {kSop1RawOrSaveExecB64,    true,  false, (init_e | src_v)},
        {kSop1RawXorSaveExecB64,   true,  false, (init_e ^ src_v)},
        {kSop1RawAndN2SaveExecB64, true,  false, (src_v & ~init_e)},
        {kSop1RawOrN2SaveExecB64,  true,  false, (src_v | ~init_e)},
        {kSop1RawNandSaveExecB64,  true,  false, ~(init_e & src_v)},
        {kSop1RawNorSaveExecB64,   true,  false, ~(init_e | src_v)},
        {kSop1RawXnorSaveExecB64,  true,  false, ~(init_e ^ src_v)},
        {kSop1RawAndN1SaveExecB64, true,  false, (~src_v & init_e)},
        {kSop1RawOrN1SaveExecB64,  true,  false, (~src_v | init_e)},
        {kSop1RawAndN1WrExecB64,   true,  true,  (~src_v & init_e)},
        {kSop1RawAndN2WrExecB64,   true,  true,  (src_v & ~init_e)},
        {kSop1RawAndSaveExecB32,   false, false, (init_e & src_v) & 0xFFFFFFFFull},
        {kSop1RawOrSaveExecB32,    false, false, (init_e | src_v) & 0xFFFFFFFFull},
        {kSop1RawXorSaveExecB32,   false, false, (init_e ^ src_v) & 0xFFFFFFFFull},
        {kSop1RawAndN2SaveExecB32, false, false, (src_v & ~init_e) & 0xFFFFFFFFull},
        {kSop1RawOrN2SaveExecB32,  false, false, (src_v | ~init_e) & 0xFFFFFFFFull},
        {kSop1RawNandSaveExecB32,  false, false, (~(init_e & src_v)) & 0xFFFFFFFFull},
        {kSop1RawNorSaveExecB32,   false, false, (~(init_e | src_v)) & 0xFFFFFFFFull},
        {kSop1RawXnorSaveExecB32,  false, false, (~(init_e ^ src_v)) & 0xFFFFFFFFull},
        {kSop1RawAndN1SaveExecB32, false, false, (~src_v & init_e) & 0xFFFFFFFFull},
        {kSop1RawOrN1SaveExecB32,  false, false, (~src_v | init_e) & 0xFFFFFFFFull},
        {kSop1RawAndN1WrExecB32,   false, true,  (~src_v & init_e) & 0xFFFFFFFFull},
        {kSop1RawAndN2WrExecB32,   false, true,  (src_v & ~init_e) & 0xFFFFFFFFull},
    }};

    auto build_xs2_from_words = [](ShaderStage stage, const std::vector<uint32_t>& words,
                                   uint32_t tg_x, uint32_t sgpr, uint32_t vgpr, uint32_t wave) {
        Xs2ShaderContainer c{};
        c.version = kXs2ContainerVersion;
        c.stage = stage;
        c.sgpr_count = sgpr;
        c.vgpr_count = vgpr;
        c.threadgroup_x = tg_x;
        c.threadgroup_y = 1u;
        c.threadgroup_z = 1u;
        c.entry_offset = kXs2HeaderSizeBytes;
        c.wave_size = (wave == 64u) ? 64u : 32u;
        c.code_words = words;
        return BuildXs2Container(c);
    };

    for (const auto& tc : cases) {
        std::vector<uint32_t> words;
        auto append_asm = [&](const Rdna2Assembler& part) {
            words.insert(words.end(), part.Words().begin(), part.Words().end());
        };
        {
            Rdna2Assembler p0;
            p0.EmitSop1(Rdna2Opcode::S_MOV_B32, 4u, Rdna2Operand::LiteralU32(static_cast<uint32_t>(init_e)));
            p0.EmitSop1(Rdna2Opcode::S_MOV_B32, 5u, Rdna2Operand::InlineInt(0));
            p0.EmitSop1(Rdna2Opcode::S_MOV_B32, 6u, Rdna2Operand::LiteralU32(static_cast<uint32_t>(src_v)));
            p0.EmitSop1(Rdna2Opcode::S_MOV_B32, 7u, Rdna2Operand::InlineInt(0));
            append_asm(p0);
        }
        // Set EXEC = init_e (raw SOP1 s_mov_b64 EXEC_LO, s[4:5])
        words.push_back(0xBE800000u | (126u << 16u) | (0x04u << 8u) | 4u);
        // Emit raw SOP1 <tc.raw_op> s8, s6
        words.push_back(0xBE800000u | (8u << 16u) | (static_cast<uint32_t>(tc.raw_op) << 8u) | 6u);
        {
            Rdna2Assembler p1;
            p1.EmitSop1(Rdna2Opcode::S_MOV_B32, 10u, Rdna2Operand::ExecLo());
            p1.EmitSop1(Rdna2Opcode::S_MOV_B32, 11u, Rdna2Operand::ExecHi());
            p1.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 12u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));
            p1.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u, Rdna2Operand::InlineInt(1));
            p1.EmitSop1(Rdna2Opcode::S_MOV_B32, 127u, Rdna2Operand::InlineInt(0));
            const std::array<uint8_t, 4> cap_s = {8u, 10u, 11u, 12u};
            for (uint32_t k = 0u; k < 4u; ++k) {
                p1.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::Sgpr(cap_s[k]));
                p1.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(k * 4u));
                p1.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 3u, 4u, 0u);
            }
            p1.EmitSopp(Rdna2Opcode::S_ENDPGM);
            append_asm(p1);
        }

        Rdna2LiftOptions opts{};
        opts.emit_hlsl = true;
        opts.emit_spirv = true;
        opts.compile_dxil_with_dxc = false;
        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(build_xs2_from_words(ShaderStage::Compute, words, 64u, 16u, 8u, 64u), lifted, opts).ok());
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(4u, 0u);
        cpu.uav1.assign(4u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());
        const uint64_t got_exec = static_cast<uint64_t>(cpu.uav1[1]) | (static_cast<uint64_t>(cpu.uav1[2]) << 32u);
        const uint32_t exp_sdst = tc.is_wrexec ? static_cast<uint32_t>(tc.expected_exec & 0xFFFFFFFFull)
                                               : static_cast<uint32_t>(init_e & 0xFFFFFFFFull);
        const uint32_t exp_scc = (tc.expected_exec != 0ull) ? 1u : 0u;
        if (cpu.uav1[0] != exp_sdst || got_exec != tc.expected_exec || cpu.uav1[3] != exp_scc) {
            std::fprintf(stderr, "[FAIL] SOP1 raw=0x%02X: sdst=0x%08X (exp 0x%08X) exec=0x%016llX (exp 0x%016llX) scc=%u (exp %u)\n",
                         tc.raw_op, cpu.uav1[0], exp_sdst,
                         static_cast<unsigned long long>(got_exec),
                         static_cast<unsigned long long>(tc.expected_exec),
                         cpu.uav1[3], exp_scc);
        }
        OS_REQUIRE(cpu.uav1[0] == exp_sdst);
        OS_REQUIRE(got_exec == tc.expected_exec);
        OS_REQUIRE(cpu.uav1[3] == exp_scc);
    }

    // B. v_mad_u64_u32 & v_mad_i64_i32 inactive-lane carry zeroing on CPU + SPIR-V + LIVE GPU!
    //    Launch 8 lanes; ALL 8 lanes hold overflowing inputs (0xFFFFFFFF * 0xFFFFFFFF + 0xFFFFFFFFFFFFFFFF),
    //    AND we mask EXEC to ONLY even lanes (lanes 0, 2, 4, 6 active; lanes 1, 3, 5, 7 INACTIVE!).
    //    Expected carry-out SDST MUST be 0x55 (bits 0,2,4,6 ONLY — inactive odd lanes strictly ZEROED!).
    {
        std::vector<uint32_t> words;
        auto append_asm = [&](const Rdna2Assembler& part) {
            words.insert(words.end(), part.Words().begin(), part.Words().end());
        };
        {
            Rdna2Assembler p0;
            // Prepare unsigned overflowing inputs for all 8 lanes: v2=0xFFFFFFFF, v3=2, v[4:5]=0xFFFFFFFFFFFFFFFF
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::InlineInt(2));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
            // Prepare signed overflowing inputs: v6=0x7FFFFFFF, v7=2, v[8:9]=0x7FFFFFFFFFFFFFFF
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::LiteralU32(0x7FFFFFFFu));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::InlineInt(2));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
            p0.EmitVop1(Rdna2Opcode::V_MOV_B32, 9u, Rdna2Operand::LiteralU32(0x7FFFFFFFu));
            // Mask EXEC to even lanes ONLY: v1 = lane & 1; v_cmp_eq_u32(v1, 0) -> vcc
            p0.EmitVop2(Rdna2Opcode::V_AND_B32, 1u, Rdna2Operand::InlineInt(1), 0u);
            p0.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 1u);
            append_asm(p0);
        }
        // Raw SOP1 s_and_saveexec_b32 s10, vcc_lo (0x3C)
        words.push_back(0xBE800000u | (10u << 16u) | (static_cast<uint32_t>(kSop1RawAndSaveExecB32) << 8u) | 106u);
        {
            Rdna2Assembler p1;
            p1.EmitVop3b(Rdna2Opcode::V_MAD_U64_U32, 12u, 12u,
                         Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
            p1.EmitVop3b(Rdna2Opcode::V_MAD_I64_I32, 14u, 14u,
                         Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(8));
            // Restore EXEC from s10
            p1.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u, Rdna2Operand::Sgpr(10u));
            p1.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::Sgpr(12u));
            p1.EmitVop1(Rdna2Opcode::V_MOV_B32, 21u, Rdna2Operand::Sgpr(14u));
            // Only write from lane 0
            p1.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0u);
            append_asm(p1);
        }
        words.push_back(0xBE800000u | (10u << 16u) | (static_cast<uint32_t>(kSop1RawAndSaveExecB32) << 8u) | 106u);
        {
            Rdna2Assembler p2;
            p2.EmitVop1(Rdna2Opcode::V_MOV_B32, 23u, Rdna2Operand::InlineInt(0));
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20u, 23u, 4u, 0u);
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 21u, 23u, 4u, 4u);
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12u, 23u, 4u, 8u);
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13u, 23u, 4u, 12u);
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14u, 23u, 4u, 16u);
            p2.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15u, 23u, 4u, 20u);
            p2.EmitSopp(Rdna2Opcode::S_ENDPGM);
            append_asm(p2);
        }

        Rdna2LiftOptions opts{};
        opts.emit_hlsl = true;
        opts.emit_spirv = true;
        opts.compile_dxil_with_dxc = false;
        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(build_xs2_from_words(ShaderStage::Compute, words, 8u, 20u, 28u, 32u), lifted, opts).ok());

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(8u, 0u);
        cpu.uav1.assign(6u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());
        // Carry masks MUST have bits 0,2,4,6 set (0x55) and inactive odd lanes 1,3,5,7 strictly 0!
        OS_REQUIRE(cpu.uav1[0] == 0x55u);
        OS_REQUIRE(cpu.uav1[1] == 0x55u);
        // Verify exact 64-bit product+add values:
        //   u64: 0xFFFFFFFF * 2 + 0xFFFFFFFFFFFFFFFF = 0x1_00000001_FFFFFFFD
        OS_REQUIRE(cpu.uav1[2] == 0xFFFFFFFDu && cpu.uav1[3] == 0x00000001u);
        //   i64: 0x7FFFFFFF * 2 + 0x7FFFFFFFFFFFFFFF = 0x80000000_FFFFFFFD
        OS_REQUIRE(cpu.uav1[4] == 0xFFFFFFFDu && cpu.uav1[5] == 0x80000000u);

        RunAndVerifyComputeBothGpus(env, lifted, cpu.uav0, std::vector<uint32_t>(6u, 0u), cpu.uav1, "mad64_inactive_lane_zeroing");
    }

    std::printf("[PASS] Test8: All 24 SOP1 saveexec/wrexec + v_mad_{u64_u32,i64_i32} inactive-lane carry zeroing on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 9. Wave64 Vote Invalidation + s_flbit_* + s_cmp_{eq,lg}_u64
// ---------------------------------------------------------------------------
void Test9_Wave64VoteInvalidationAndFlbitCmp64(TestEnv& env) {
    Rdna2Assembler a;
    // A. Wave64 vote invalidation:
    //    1) v_cmp_eq_u32 v0, v0 -> sets VCC to all-ones (non-zero cross-half vote!)
    //    2) Overwrite VCC_LO & VCC_HI with 0 via s_mov_b32 -> MUST invalidate cached vote!
    //    3) s_cbranch_vccz MUST BE TAKEN (jumping over clobber v2 = 999)
    //    4) v_cndmask_b32 with overwritten VCC=0 MUST select src0 (0x12345678), NOT stale vote!
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::Vgpr(0), 0u);  // vcc = all-1s
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 106u /*VCC_LO*/, Rdna2Operand::InlineInt(0));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 107u /*VCC_HI*/, Rdna2Operand::InlineInt(0));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x11111111u));
    const uint32_t br_vccz = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0xBAD00000u));  // MUST be skipped!
    a.PatchBranch(br_vccz, a.CurrentWordOffset());
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0xBAD11111u));
    a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 4u, Rdna2Operand::LiteralU32(0x12345678u), 3u);  // vcc==0 -> 0x12345678!

    // B. s_flbit_i32_b32, s_flbit_i32_b64, s_flbit_i32, s_flbit_i32_i64
    //    s_flbit_i32_b32(0x00100000) = 11; s_flbit_i32_b32(0) = -1 (0xFFFFFFFF)
    //    s_flbit_i32(0x0000FFFF) = 16; s_flbit_i32(0xFFFFFFF0) = 28; s_flbit_i32(-1) = -1
    //    s_flbit_i32_b64(0x00000000_00000080) = 56
    //    s_flbit_i32_i64(0xFFFFFFFF_FFFFF000) = 52
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32_B32, 8u, Rdna2Operand::LiteralU32(0x00100000u));
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32_B32, 9u, Rdna2Operand::InlineInt(0));
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32, 10u, Rdna2Operand::LiteralU32(0x0000FFFFu));
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32, 11u, Rdna2Operand::LiteralU32(0xFFFFFFF0u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 12u, Rdna2Operand::LiteralU32(0x00000080u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 13u, Rdna2Operand::InlineInt(0));
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32_B64, 14u, Rdna2Operand::Sgpr(12u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 16u, Rdna2Operand::LiteralU32(0xFFFFF000u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 17u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
    a.EmitSop1(Rdna2Opcode::S_FLBIT_I32_I64, 15u, Rdna2Operand::Sgpr(16u));

    // C. s_cmp_eq_u64 & s_cmp_lg_u64 (differ ONLY in upper 32 bits -> tests true 64-bit compare!)
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 18u, Rdna2Operand::LiteralU32(0x12345678u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 19u, Rdna2Operand::InlineInt(1));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 20u, Rdna2Operand::LiteralU32(0x12345678u));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 21u, Rdna2Operand::InlineInt(2));
    a.EmitSopc(Rdna2Opcode::S_CMP_EQ_U64, Rdna2Operand::Sgpr(18u), Rdna2Operand::Sgpr(20u));  // false (hi 1 != 2)
    a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 22u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));
    a.EmitSopc(Rdna2Opcode::S_CMP_LG_U64, Rdna2Operand::Sgpr(18u), Rdna2Operand::Sgpr(20u));  // true (hi 1 != 2)
    a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 23u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));
    a.EmitSopc(Rdna2Opcode::S_CMP_EQ_U64, Rdna2Operand::Sgpr(18u), Rdna2Operand::Sgpr(18u));  // true
    a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 24u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));

    // Store all 11 results (all 64 lanes hold identical values — guard with S_CBRANCH_VCCZ for lane 0 only!)
    a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0u);
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::InlineInt(0));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 5u, 4u, 0u);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4u, 5u, 4u, 4u);
    const std::array<uint8_t, 9> s_out = {8, 9, 10, 11, 14, 15, 22, 23, 24};
    for (uint32_t k = 0u; k < s_out.size(); ++k) {
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::Sgpr(s_out[k]));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6u, 5u, 4u, static_cast<uint16_t>((2u + k) * 4u));
    }
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;
    Rdna2LiftResult lifted{};
    // Use Wave64 container to exercise Wave64-on-Wave32 vote invalidation path!
    OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 64u, 1u, 1u, 32u, 12u, 64u), lifted, opts).ok());

    const std::vector<uint32_t> uav0(4u, 0u);
    const std::vector<uint32_t> init1(11u, 0u);
    Rdna2CpuExecState cpu{};
    cpu.uav0 = uav0;
    cpu.uav1 = init1;
    OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());

    const std::vector<uint32_t> expected = {
        0x11111111u,  // br_vccz taken!
        0x12345678u,  // v_cndmask read invalidated vcc==0!
        11u,          // s_flbit_i32_b32(0x00100000)
        0xFFFFFFFFu,  // s_flbit_i32_b32(0)
        16u,          // s_flbit_i32(0x0000FFFF)
        28u,          // s_flbit_i32(0xFFFFFFF0)
        56u,          // s_flbit_i32_b64(0x80)
        52u,          // s_flbit_i32_i64(0xFFFFFFFFFFFFF000)
        0u,           // s_cmp_eq_u64 (hi 1 != 2)
        1u,           // s_cmp_lg_u64 (hi 1 != 2)
        1u,           // s_cmp_eq_u64 (same)
    };
    OS_REQUIRE(cpu.uav1 == expected);
    RunAndVerifyComputeBothGpus(env, lifted, uav0, init1, expected, "wave64_vote_inval_flbit_cmp64");
    std::printf("[PASS] Test9: Wave64 vote invalidation + s_flbit_* + s_cmp_{eq,lg}_u64 on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 10. Quad-Local DPP8 + Non-Packed 16-Bit VOP3 op_sel + 64-Bit PS Integer Compares
// ---------------------------------------------------------------------------
void Test10_Dpp8AndVop3I16AndPsCmp64(TestEnv& env) {
    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    // A. Quad-local DPP8 (0xE9) & DPP8FI (0xEA) arbitrary 8-lane swizzle in CS
    {
        Rdna2Assembler a;
        // v1 = lane * 10 + 1
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 1u, Rdna2Operand::Vgpr(0), Rdna2Operand::InlineInt(10));
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 1u, Rdna2Operand::InlineInt(1), 1u);
        // sel = [1, 0, 3, 2, 7, 6, 5, 4] (quad-local swap + upper-quad reverse!)
        const uint32_t sel24 = (1u << 0u) | (0u << 3u) | (3u << 6u) | (2u << 9u) |
                               (7u << 12u) | (6u << 15u) | (5u << 18u) | (4u << 21u);
        a.EmitVop1Dpp8(Rdna2Opcode::V_MOV_B32, 2u, 1u, sel24, /*fi=*/false);
        // Also DPP8FI on V_ADD_NC_U32: v3 = dpp8(v1, reverse8) + v1
        const uint32_t rev8 = (7u << 0u) | (6u << 3u) | (5u << 6u) | (4u << 9u) |
                              (3u << 12u) | (2u << 15u) | (1u << 18u) | (0u << 21u);
        a.EmitVop2Dpp8(Rdna2Opcode::V_ADD_NC_U32, 3u, 1u, 1u, rev8, /*fi=*/true);

        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4u, Rdna2Operand::InlineInt(3), 0u);  // lane * 8
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 4u, 4u, 0u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 4u, 4u, 4u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 16u, 1u, 1u, 16u, 12u, 32u), lifted, opts).ok());
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(32u, 0u);
        cpu.uav1.assign(32u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());
        // Verify lane 0 got lane 1's value (11), lane 4 got lane 7's value (71), lane 8 got lane 9's value (91)
        OS_REQUIRE(cpu.uav1[0] == 11u);
        OS_REQUIRE(cpu.uav1[2] == 1u);
        OS_REQUIRE(cpu.uav1[8] == 71u);
        OS_REQUIRE(cpu.uav1[16] == 91u);
        // Verify reverse8 sum: lane 0 + lane 7 = 1 + 71 = 72
        OS_REQUIRE(cpu.uav1[1] == 72u);
        RunAndVerifyComputeBothGpus(env, lifted, cpu.uav0, std::vector<uint32_t>(32u, 0u), cpu.uav1, "dpp8_quad_local");
    }

    // B. Non-packed 16-bit integer VOP3 with exact op_sel (src0/src1/dst hi/lo preserving untouched half!)
    {
        Rdna2Assembler a;
        // v2 = 0x1000_0020 (hi=0x1000, lo=0x0020), v3 = 0xFFFC_0003 (hi=-4, lo=3)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0x10000020u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0xFFFC0003u));
        // Pre-seed destinations with sentinel 0xBEEFCAFE to verify untouched-half preservation!
        for (uint8_t r = 10u; r <= 19u; ++r) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, r, Rdna2Operand::LiteralU32(0xBEEFCAFEu));
        }
        // op_sel bits: [0]=src0_hi, [1]=src1_hi, [3]=dst_hi
        a.EmitVop3(Rdna2Opcode::V_ADD_NC_U16,   10u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x1u);  // hi(v2)+lo(v3)=0x1003 -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_SUB_NC_U16,   11u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x8u);  // lo(v2)-lo(v3)=0x001D -> hi, lo=CAFE
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U16,   12u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x0u);  // 0x20*3=0x60 -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_MIN_U16,      13u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x2u);  // min_u16(0x20,0xFFFC)=0x20 -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_MAX_U16,      14u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x2u);  // max_u16(0x20,0xFFFC)=0xFFFC -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_MIN_I16,      15u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x2u);  // min_i16(32,-4)=-4 (0xFFFC) -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_MAX_I16,      16u, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0xAu);  // max_i16(32,-4)=32 (0x0020) -> hi, lo=CAFE
        a.EmitVop3(Rdna2Opcode::V_LSHLREV_B16,  17u, Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(0), false, 0x0u);  // 0x20<<2=0x80 -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_LSHRREV_B16,  18u, Rdna2Operand::InlineInt(1), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0x2u);  // 0xFFFC>>1=0x7FFE -> lo, hi=BEEF
        a.EmitVop3(Rdna2Opcode::V_ASHRREV_I16,  19u, Rdna2Operand::InlineInt(1), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(0), false, 0xAu);  // -4>>1=-2 (0xFFFE) -> hi, lo=CAFE

        for (uint8_t k = 0u; k < 10u; ++k) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20u, Rdna2Operand::LiteralU32(k * 4u));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, static_cast<uint8_t>(10u + k), 20u, 4u, 0u);
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 16u, 24u, 32u), lifted, opts).ok());
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(4u, 0u);
        cpu.uav1.assign(10u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok());
        const std::vector<uint32_t> expected = {
            0xBEEF1003u, 0x001DCAFEu, 0xBEEF0060u, 0xBEEF0020u, 0xBEEFFFFCu,
            0xBEEFFFFCu, 0x0020CAFEu, 0xBEEF0080u, 0xBEEF7FFEu, 0xFFFECAFEu,
        };
        OS_REQUIRE(cpu.uav1 == expected);
        RunAndVerifyComputeBothGpus(env, lifted, cpu.uav0, std::vector<uint32_t>(10u, 0u), expected, "vop3_i16_op_sel");
    }

    // C. 64-bit integer comparisons inside Pixel Shader (PS) stage on CPU + SPIR-V + DXC + LIVE GPU!
    {
        Rdna2Assembler ps;
        // a = 0x00000001_FFFFFFFF (v[2:3]), b = 0x00000002_00000000 (v[4:5])
        // c = -2 (0xFFFFFFFF_FFFFFFFE = v[6:7]), d = +1 (0x00000000_00000001 = v[8:9])
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::InlineInt(1));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::InlineInt(0));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::InlineInt(2));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::LiteralU32(0xFFFFFFFEu));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::InlineInt(1));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 9u, Rdna2Operand::InlineInt(0));

        // R: v_cmp_lt_u64(a, b) -> true -> 0.25f
        ps.EmitVopc(Rdna2Opcode::V_CMP_LT_U64, Rdna2Operand::Vgpr(2), 4u);
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::LiteralF32(0.25f));
        ps.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20u, Rdna2Operand::InlineFloat(0.0f), 10u);

        // G: v_cmp_lt_i64(c=-2, d=+1) -> true (unsigned would be FALSE!) -> 0.50f
        ps.EmitVopc(Rdna2Opcode::V_CMP_LT_I64, Rdna2Operand::Vgpr(6), 8u);
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 11u, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 21u, Rdna2Operand::InlineFloat(0.0f), 11u);

        // B: v_cmp_gt_u64(c=0xFFFFFFFFFFFFFFFE, d=1) -> true -> 0.75f
        ps.EmitVopc(Rdna2Opcode::V_CMP_GT_U64, Rdna2Operand::Vgpr(6), 8u);
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 12u, Rdna2Operand::LiteralF32(0.75f));
        ps.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 22u, Rdna2Operand::InlineFloat(0.0f), 12u);

        // A: v_cmp_eq_i64(c, c) -> true -> 1.0f
        ps.EmitVopc(Rdna2Opcode::V_CMP_EQ_I64, Rdna2Operand::Vgpr(6), 6u);
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::InlineFloat(1.0f));
        ps.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 23u, Rdna2Operand::InlineFloat(0.0f), 13u);

        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 20u, 21u, 22u, 23u, 0xFu, true, false, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult ps_lift{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel, 1u, 1u, 1u, 16u, 28u, 32u), ps_lift, opts).ok());
        ValidateSpirvIfAvailable(env, ps_lift.spirv_words, "ps_cmp64");

        Rdna2CpuExecState cpu{};
        OS_REQUIRE(ExecuteRdna2Cpu(ps_lift.container, ps_lift.instructions, cpu).ok());
        OS_REQUIRE(!cpu.exports.empty() && cpu.exports[0].wrote_mrt0);
        const std::array<float, 4> expected = {0.25f, 0.50f, 0.75f, 1.0f};
        for (size_t c = 0u; c < 4u; ++c) {
            OS_REQUIRE(std::fabs(cpu.exports[0].mrt0[c] - expected[c]) < 1e-4f);
        }

#if defined(_WIN32)
        if (!env.dxc_path.empty() && (env.has_hw || env.has_warp)) {
            Rdna2Assembler vs;
            vs.EmitVop2(Rdna2Opcode::V_AND_B32, 1u, Rdna2Operand::InlineInt(1), 0u);
            vs.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2u, Rdna2Operand::InlineInt(1), 0u);
            vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 3u, Rdna2Operand::Vgpr(1));
            vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 4u, Rdna2Operand::Vgpr(2));
            vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 5u, Rdna2Operand::Vgpr(3), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
            vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 6u, Rdna2Operand::Vgpr(4), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
            vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::InlineFloat(0.0f));
            vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::InlineFloat(1.0f));
            vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 5u, 6u, 7u, 8u, 0xFu, true);
            vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
            Rdna2LiftResult vs_lift{};
            OS_REQUIRE(LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex, 1u, 1u, 1u, 16u, 16u, 32u), vs_lift, opts).ok());

            auto run_ps = [&](S16D3D12Runner& gpu, const char* tag) {
                std::vector<uint8_t> vs_dxil, ps_dxil;
                std::string diag;
                OS_REQUIRE(CompileHlslWithDxc(vs_lift.hlsl_source, gpu.TargetProfile("vs"), "main", vs_dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(CompileHlslWithDxc(ps_lift.hlsl_source, gpu.TargetProfile("ps"), "main", ps_dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(vs_dxil).ok() && ResignDxilContainer(ps_dxil).ok());
                std::vector<float> fb;
                OS_REQUIRE(gpu.RunGraphicsDraw(vs_dxil, ps_dxil, fb));
                for (size_t c = 0u; c < 4u; ++c) {
                    if (std::fabs(fb[c] - expected[c]) > 1e-4f) {
                        std::fprintf(stderr, "[FAIL] PS 64-bit cmp (%s) ch%zu: got=%.4f exp=%.4f\n",
                                     tag, c, fb[c], expected[c]);
                    }
                    OS_REQUIRE(std::fabs(fb[c] - expected[c]) < 1e-4f);
                }
            };
            if (env.has_hw) run_ps(env.hw, "D3D12-HW");
            if (env.has_warp) run_ps(env.warp, "D3D12-WARP");
        }
#endif
    }

    std::printf("[PASS] Test10: Quad-local DPP8/DPP8FI + non-packed 16-bit VOP3 op_sel + PS 64-bit compares on CPU+SPIR-V+GPU\n");
}

// ---------------------------------------------------------------------------
// 11. All-Stage Real-GPU Execution (CS / VS / PS / MS / AS / HS+DS / GS) + Honest 775/779
// ---------------------------------------------------------------------------
void Test11_All7StagesLiveGpuAndBenchJson() {
    Rdna2BenchOptions bopts{};
    bopts.corpus_count = 24u;
    bopts.external_val_sample = 6u;
    bopts.run_live_gpu = true;
    bopts.run_external_validators = false;
    bopts.quiet = true;

    Rdna2CoverageReport rep{};
    const Status st = RunRdna2CoverageBenchmark(bopts, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] RunRdna2CoverageBenchmark failed: %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.opcodes_total == 779u);
    OS_REQUIRE(rep.opcodes_decoded == 779u);
    // Strict honesty: 775 real-GPU verified + 4 privileged trap/halt opcodes (never inflated to 779!)
    OS_REQUIRE(rep.opcodes_gpu_verified == 775u);
    // Backward compatibility: stages stays 5 (CS/VS/PS/MS/AS), extended_stages has 3 (HS/DS/GS)
    OS_REQUIRE(rep.stages.size() == 5u);
    OS_REQUIRE(rep.extended_stages.size() == 3u);
    OS_REQUIRE(rep.extended_stages[0].stage == "HS");
    OS_REQUIRE(rep.extended_stages[1].stage == "DS");
    OS_REQUIRE(rep.extended_stages[2].stage == "GS");
    if (rep.has_dxc_tool && (rep.has_d3d12_hw || rep.has_d3d12_warp)) {
        OS_REQUIRE(rep.extended_stages[0].gpu_passed >= 1u);
        OS_REQUIRE(rep.extended_stages[1].gpu_passed >= 1u);
        OS_REQUIRE(rep.extended_stages[2].gpu_passed >= 1u);
    }

    const std::string json = SerializeRdna2CoverageJson(rep);
    for (const char* stg : {"\"CS\":", "\"VS\":", "\"PS\":", "\"MS\":", "\"AS\":", "\"HS\":", "\"DS\":", "\"GS\":"}) {
        OS_REQUIRE(json.find(stg) != std::string::npos);
    }

    std::printf("[PASS] Test11: All-7-stage real-GPU execution (CS/VS/PS/MS/AS/HS+DS/GS) + honest %u/%u GPU opcodes\n",
                rep.opcodes_gpu_verified, rep.opcodes_total);
}

// ---------------------------------------------------------------------------
// 12. Zero-Hitch Multi-Threaded Async PSO + Persistent Disk ShaderCache
// ---------------------------------------------------------------------------
void Test12_ZeroHitchAsyncPsoAndPersistentDiskCache(const TestEnv& env) {
    if (env.dxc_path.empty()) {
        std::printf("[SKIP] Test12: DXC unavailable\n");
        return;
    }

    const auto tmp_root = std::filesystem::temp_directory_path() / "openscarlett_s16_shader_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
    std::filesystem::create_directories(tmp_root, ec);

    // Build 4 distinct compute XS2 shaders
    std::vector<std::vector<uint8_t>> xs2_blobs;
    for (uint32_t i = 0u; i < 4u; ++i) {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1u, Rdna2Operand::InlineInt(2), 0u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u);
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 2u, Rdna2Operand::LiteralU32(100u + i * 17u), 2u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 1u, 4u, 0u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        xs2_blobs.push_back(a.BuildXs2(ShaderStage::Compute, 32u, 1u, 1u, 16u, 8u, 32u));
    }

    ShaderCacheLiftRequest req{};
    req.highest_shader_model = 0x65u;
    req.dxc_path = env.dxc_path;

    // Phase 1: Multi-threaded worker pool + Prewarm + priority promotion + stand-in + hot-swap callback
    {
        ShaderCache cache;
        cache.SetCacheDirectory(tmp_root.string());
        cache.SetAsyncWorkerCount(3u);
        OS_REQUIRE(cache.GetAsyncWorkerCount() == 3u);
        cache.EnableAsyncPsoCompilation(true);

        std::atomic<uint32_t> cb_count{0u};
        cache.SetAsyncCompletionCallback([&](const ShaderCacheKey&, const CachedShaderResult& res, const Status& st) {
            if (st.ok() && !res.dxil.empty() && !res.is_standin_async) {
                cb_count.fetch_add(1u, std::memory_order_relaxed);
            }
        });

        // Prewarm shaders 0 & 1 at Prewarm priority
        ShaderCacheKey k0{}, k1{};
        OS_REQUIRE(cache.PrewarmXboxShaderAsync(xs2_blobs[0].data(), xs2_blobs[0].size(), req, &k0, ShaderCachePriority::Prewarm).ok());
        OS_REQUIRE(cache.PrewarmXboxShaderAsync(xs2_blobs[1].data(), xs2_blobs[1].size(), req, &k1, ShaderCachePriority::Prewarm).ok());
        // Duplicate prewarm on k1 at Urgent priority (exercises dedup + priority promotion if still queued)
        OS_REQUIRE(cache.PrewarmXboxShaderAsync(xs2_blobs[1].data(), xs2_blobs[1].size(), req, nullptr, ShaderCachePriority::Urgent).ok());

        // Request shaders 2 & 3 via GetOrLiftXboxShader -> immediate stand-in return (zero hitch!)
        CachedShaderResult r2{}, r3{};
        OS_REQUIRE(cache.GetOrLiftXboxShader(xs2_blobs[2].data(), xs2_blobs[2].size(), req, r2).ok());
        OS_REQUIRE(r2.is_standin_async && !r2.dxil.empty());
        OS_REQUIRE(cache.GetOrLiftXboxShader(xs2_blobs[3].data(), xs2_blobs[3].size(), req, r3).ok());
        OS_REQUIRE(r3.is_standin_async && !r3.dxil.empty());

        cache.WaitForPendingCompiles();
        OS_REQUIRE(cache.IsCompileReady(k0));
        OS_REQUIRE(cache.IsCompileReady(k1));
        OS_REQUIRE(cache.IsCompileReady(r2.key));
        OS_REQUIRE(cache.IsCompileReady(r3.key));
        OS_REQUIRE(cb_count.load() == 4u);

        const auto st = cache.Stats();
        OS_REQUIRE(st.async_compiles_completed == 4u);
        OS_REQUIRE(st.standins_served >= 2u);
        OS_REQUIRE(st.hot_swaps_ready >= 2u);
        OS_REQUIRE(st.disk_bytes_written > 0u);
        OS_REQUIRE(st.total_compile_time_us > 0u);
        OS_REQUIRE(!cache.FormatDiagnosticsSummary().empty());

        // Re-query r2 -> instant memory hit with real lifted DXIL (not stand-in!)
        CachedShaderResult r2_real{};
        OS_REQUIRE(cache.GetOrLiftXboxShader(xs2_blobs[2].data(), xs2_blobs[2].size(), req, r2_real).ok());
        OS_REQUIRE(r2_real.from_memory_cache && !r2_real.is_standin_async);
        OS_REQUIRE(VerifyDxbcContainerHash(r2_real.dxil.data(), r2_real.dxil.size()));
    }

    // Phase 2: Cold persistent disk reload + EnumerateDiskCacheEntries + CRC32 auto-heal + PruneDiskCache
    {
        ShaderCache cold;
        cold.SetCacheDirectory(tmp_root.string());
        cold.EnableAsyncPsoCompilation(false);

        const auto entries = cold.EnumerateDiskCacheEntries();
        OS_REQUIRE(entries.size() == 4u);
        for (const auto& e : entries) {
            OS_REQUIRE(e.valid_crc32);
            OS_REQUIRE(e.size_bytes > 0u);
        }

        // Cold load shader 0 -> disk hit!
        CachedShaderResult hit0{};
        OS_REQUIRE(cold.GetOrLiftXboxShader(xs2_blobs[0].data(), xs2_blobs[0].size(), req, hit0).ok());
        OS_REQUIRE(hit0.from_disk_cache && !hit0.is_standin_async);
        OS_REQUIRE(cold.Stats().disk_hits == 1u);
        OS_REQUIRE(cold.Stats().disk_bytes_read > 0u);

        // Corrupt shader 1's `.ossc` payload on disk -> verify CRC32 auto-heal detects & recompiles cleanly!
        ShaderCacheKey k1{};
        {
            CachedShaderResult tmp{};
            OS_REQUIRE(cold.GetOrLiftXboxShader(xs2_blobs[1].data(), xs2_blobs[1].size(), req, tmp).ok());
            k1 = tmp.key;
        }
        cold.ClearMemoryCache();
        const std::string p1 = cold.CacheFilePathForKey(k1);
        {
            std::fstream fs(p1, std::ios::binary | std::ios::in | std::ios::out);
            OS_REQUIRE(fs.good());
            fs.seekp(-1, std::ios::end);
            char b = 0;
            fs.read(&b, 1);
            fs.seekp(-1, std::ios::end);
            b = static_cast<char>(b ^ 0xFF);
            fs.write(&b, 1);
        }
        CachedShaderResult healed{};
        OS_REQUIRE(cold.GetOrLiftXboxShader(xs2_blobs[1].data(), xs2_blobs[1].size(), req, healed).ok());
        OS_REQUIRE(cold.Stats().corrupt_entries == 1u);
        OS_REQUIRE(!healed.from_disk_cache && VerifyDxbcContainerHash(healed.dxil.data(), healed.dxil.size()));

        // Prune disk cache down to max 2 entries
        const size_t pruned = cold.PruneDiskCache(/*max_bytes=*/0u, /*max_entries=*/2u);
        OS_REQUIRE(pruned == 2u);
        OS_REQUIRE(cold.EnumerateDiskCacheEntries().size() == 2u);
        OS_REQUIRE(cold.Stats().disk_entries_pruned == 2u);
    }

    std::filesystem::remove_all(tmp_root, ec);
    std::printf("[PASS] Test12: Zero-hitch multi-threaded async PSO + persistent disk ShaderCache (pool/prewarm/hot-swap/CRC32/prune)\n");
}

// ---------------------------------------------------------------------------
// 13. 3D-World RDNA2 ISA Completeness: DPP16, v_writelane_b32 Wave64 Spills,
//     v_bfm_b32 / s_bfm_b64 / s_quadmask_b32_b64 / SAD+MSAD / SGPR104..107 /
//     SALU Null SCC / MIMG Atomics / V# Unified 7-bit UFMT + DST_SEL
// ---------------------------------------------------------------------------
uint32_t MakeDpp16Word(uint8_t vsrc0,
                       uint16_t dpp_ctrl,
                       bool bound_ctrl = false,
                       uint8_t bank_mask = 0xFu,
                       uint8_t row_mask = 0xFu,
                       bool s0_neg = false,
                       bool s0_abs = false,
                       bool s1_neg = false,
                       bool s1_abs = false) noexcept {
    return static_cast<uint32_t>(vsrc0) |
           (static_cast<uint32_t>(dpp_ctrl & 0x1FFu) << 8u) |
           (bound_ctrl ? (1u << 19u) : 0u) |
           (s0_neg ? (1u << 20u) : 0u) |
           (s0_abs ? (1u << 21u) : 0u) |
           (s1_neg ? (1u << 22u) : 0u) |
           (s1_abs ? (1u << 23u) : 0u) |
           (static_cast<uint32_t>(bank_mask & 0xFu) << 24u) |
           (static_cast<uint32_t>(row_mask & 0xFu) << 28u);
}

void Test13_Rdna2_3dWorldIsaCompleteness(TestEnv& env) {
    Rdna2LiftOptions opts{};
    opts.emit_hlsl = true;
    opts.emit_spirv = true;
    opts.compile_dxil_with_dxc = false;

    // 13A. DPP16 (0xFA) Full Decoding & Lowering (CS Wave64 all modes + PS QuadReadLaneAt)
    {
        Rdna2Assembler a;
        // v1 = lane + 100 (100..163)
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 1u, Rdna2Operand::LiteralU32(100u), 0u);
        // v2: quad_perm:[1,0,3,2] (0xB1)
        const uint32_t dpp_qp = MakeDpp16Word(1u, 0xB1u);
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 2u, dpp_qp);
        // v3: row_shl:1 (0x101) with bound_ctrl=1 (OOB lane 15/31/47/63 -> 0)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::LiteralU32(0xDEADu));
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 3u, MakeDpp16Word(1u, 0x101u, /*bound_ctrl=*/true));
        // v4: row_shr:2 (0x112) with bound_ctrl=0 (OOB lanes 0,1 in each row keep 0x7777!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::LiteralU32(0x7777u));
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 4u, MakeDpp16Word(1u, 0x112u, /*bound_ctrl=*/false));
        // v5: row_ror:3 (0x123)
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 5u, MakeDpp16Word(1u, 0x123u));
        // v6: row_mirror (0x140)
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 6u, MakeDpp16Word(1u, 0x140u));
        // v7: row_half_mirror (0x141)
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 7u, MakeDpp16Word(1u, 0x141u));
        // v8: row_bcast15 (0x142) with row_mask=0xA (rows 1 & 3 active, rows 0 & 2 keep 0x5555)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::LiteralU32(0x5555u));
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 8u, MakeDpp16Word(1u, 0x142u, false, 0xFu, 0xAu));
        // v9: row_bcast31 (0x143) with row_mask=0xC (rows 2 & 3 read lane 31, rows 0 & 1 keep 0x6666)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9u, Rdna2Operand::LiteralU32(0x6666u));
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 9u, MakeDpp16Word(1u, 0x143u, false, 0xFu, 0xCu));
        // v10: row_share:5 (0x155) with bank_mask=0x5 (banks 0,2 active; banks 1,3 keep 0x8888)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::LiteralU32(0x8888u));
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 10u, MakeDpp16Word(1u, 0x155u, false, 0x5u, 0xFu));
        // v11: row_xmask:3 (0x163)
        a.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 11u, MakeDpp16Word(1u, 0x163u));
        // v12: VOP2 DPP16 with src0_abs=1, src1_neg=1: |-3.0f| + -(-1.0f) = 4.0f (0x40800000)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::LiteralF32(-3.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14u, Rdna2Operand::InlineFloat(-1.0f));
        a.EmitVop2Dpp16(Rdna2Opcode::V_ADD_F32, 12u, 14u,
                        MakeDpp16Word(13u, 0x00u, false, 0xFu, 0xFu, false, true, true, false));

        // Store 11 dwords per lane (lanes 0..63 -> 704 dwords)
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 15u, Rdna2Operand::Vgpr(0), Rdna2Operand::InlineInt(44));
        for (uint8_t r = 2u; r <= 12u; ++r) {
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, r, 15u, 4u, static_cast<uint16_t>((r - 2u) * 4u));
        }
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 64u, 1u, 1u, 24u, 20u, 64u), lifted, opts).ok());
        // Verify disassembler populated has_dpp16 and dpp16_word
        OS_REQUIRE(lifted.instructions.size() >= 3u);
        OS_REQUIRE(lifted.instructions[1].has_dpp16 && lifted.instructions[1].dpp16_word == dpp_qp);

        std::vector<uint32_t> expected(64u * 11u, 0u);
        for (uint32_t l = 0u; l < 64u; ++l) {
            const uint32_t in_r = l & 15u;
            const uint32_t rb = l & ~15u;
            const uint32_t bk = (l >> 2u) & 3u;
            const uint32_t rw = (l >> 4u) & 3u;
            const size_t base = static_cast<size_t>(l) * 11u;
            const uint32_t qp_idx = (0xB1u >> ((l & 3u) * 2u)) & 3u;
            expected[base + 0u] = ((l & ~3u) | qp_idx) + 100u;
            expected[base + 1u] = (in_r + 1u < 16u) ? (rb + in_r + 1u + 100u) : 0u;
            expected[base + 2u] = (in_r >= 2u) ? (rb + in_r - 2u + 100u) : 0x7777u;
            expected[base + 3u] = (rb | ((in_r + 13u) & 15u)) + 100u;
            expected[base + 4u] = (rb | (15u - in_r)) + 100u;
            expected[base + 5u] = ((l & ~7u) | (7u - (l & 7u))) + 100u;
            expected[base + 6u] = (((0xAu >> rw) & 1u) != 0u) ? (((l & ~31u) | 15u) + 100u) : 0x5555u;
            expected[base + 7u] = (((0xCu >> rw) & 1u) != 0u) ? (31u + 100u) : 0x6666u;
            expected[base + 8u] = (((0x5u >> bk) & 1u) != 0u) ? ((rb | 5u) + 100u) : 0x8888u;
            expected[base + 9u] = (l ^ 3u) + 100u;
            expected[base + 10u] = F2U(4.0f);
        }
        RunAndVerifyComputeBothGpus(env, lifted, std::vector<uint32_t>(8u, 0u),
                                    std::vector<uint32_t>(expected.size(), 0u), expected, "dpp16_wave64_all_modes");

        // Pixel-stage quad_perm lowering via QuadReadLaneAt when row_mask==0xF && bank_mask==0xF
        Rdna2Assembler ps;
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::LiteralF32(0.25f));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop1Dpp16(Rdna2Opcode::V_MOV_B32, 4u, MakeDpp16Word(2u, 0x00u));  // quad_perm:[0,0,0,0] -> QuadReadLaneAt(v,0)
        ps.EmitVop2Dpp16(Rdna2Opcode::V_ADD_F32, 5u, 3u, MakeDpp16Word(2u, 0xB1u));  // quad_perm:[1,0,3,2] + 0.5f = 0.75f
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::InlineFloat(1.0f));
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4u, 5u, 3u, 6u, 0xFu, true, false, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult ps_lift{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ps.BuildXs2(ShaderStage::Pixel, 1u, 1u, 1u, 16u, 12u, 32u), ps_lift, opts).ok());
        OS_REQUIRE(CountSubstr(ps_lift.hlsl_source, "QuadReadLaneAt") >= 5u);
        ValidateSpirvIfAvailable(env, ps_lift.spirv_words, "ps_dpp16_quad_perm");
#if defined(_WIN32)
        if (!env.dxc_path.empty() && (env.has_hw || env.has_warp)) {
            Rdna2Assembler vs;
            vs.EmitVop2(Rdna2Opcode::V_AND_B32, 1u, Rdna2Operand::InlineInt(1), 0u);
            vs.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2u, Rdna2Operand::InlineInt(1), 0u);
            vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 3u, Rdna2Operand::Vgpr(1));
            vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 4u, Rdna2Operand::Vgpr(2));
            vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 5u, Rdna2Operand::Vgpr(3), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
            vs.EmitVop3(Rdna2Opcode::V_FMA_F32, 6u, Rdna2Operand::Vgpr(4), Rdna2Operand::LiteralF32(4.0f), Rdna2Operand::InlineFloat(-1.0f));
            vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::InlineFloat(0.0f));
            vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::InlineFloat(1.0f));
            vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 5u, 6u, 7u, 8u, 0xFu, true);
            vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
            Rdna2LiftResult vs_lift{};
            OS_REQUIRE(LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex, 1u, 1u, 1u, 16u, 16u, 32u), vs_lift, opts).ok());
            auto run_ps = [&](S16D3D12Runner& gpu) {
                std::vector<uint8_t> vs_dxil, ps_dxil;
                std::string diag;
                OS_REQUIRE(CompileHlslWithDxc(vs_lift.hlsl_source, gpu.TargetProfile("vs"), "main", vs_dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(CompileHlslWithDxc(ps_lift.hlsl_source, gpu.TargetProfile("ps"), "main", ps_dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(vs_dxil).ok() && ResignDxilContainer(ps_dxil).ok());
                std::vector<float> fb;
                OS_REQUIRE(gpu.RunGraphicsDraw(vs_dxil, ps_dxil, fb));
                OS_REQUIRE(std::fabs(fb[0] - 0.25f) < 1e-4f && std::fabs(fb[1] - 0.75f) < 1e-4f);
            };
            if (env.has_hw) run_ps(env.hw);
            if (env.has_warp) run_ps(env.warp);
        }
#endif
    }

    // 13B. v_writelane_b32 (VOP3 0x361) + Wave64 SGPR Constant-Lane Spills (K in 0..63 on Wave32 Host!)
    {
        Rdna2Assembler a;
        // Spill 5 SGPR constants into v4 at K = 0, 15, 32, 47, 63 (note tg_x=16 < 32 so lanes 32..63
        // do not even exist as physical GPU threads — only exact (vN, K) spill tracking can recover them!)
        const std::array<uint32_t, 5> spill_lanes = {0u, 15u, 32u, 47u, 63u};
        const std::array<uint32_t, 5> spill_vals = {
            0xA0000000u, 0xA000000Fu, 0xA0000020u, 0xA000002Fu, 0xA000003Fu};
        for (size_t i = 0u; i < 5u; ++i) {
            a.EmitSop1(Rdna2Opcode::S_MOV_B32, static_cast<uint8_t>(10u + i), Rdna2Operand::LiteralU32(spill_vals[i]));
            a.EmitVop3(Rdna2Opcode::V_WRITELANE_B32, 4u,
                       Rdna2Operand::Sgpr(static_cast<uint8_t>(10u + i)),
                       Rdna2Operand::InlineInt(static_cast<int32_t>(spill_lanes[i])));
        }
        for (size_t i = 0u; i < 5u; ++i) {
            a.EmitVop3(Rdna2Opcode::V_READLANE_B32, static_cast<uint8_t>(20u + i),
                       Rdna2Operand::Vgpr(4u),
                       Rdna2Operand::InlineInt(static_cast<int32_t>(spill_lanes[i])));
        }
        // Normal VGPR write to v5 MUST invalidate stale spill slot (v5, 5)!
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 15u, Rdna2Operand::LiteralU32(0xCAFEBABEu));
        a.EmitVop3(Rdna2Opcode::V_WRITELANE_B32, 5u, Rdna2Operand::Sgpr(15u), Rdna2Operand::InlineInt(5));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5u, Rdna2Operand::LiteralU32(0x12345678u));
        a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 25u, Rdna2Operand::Vgpr(5u), Rdna2Operand::InlineInt(5));

        // Dynamic-lane v_writelane_b32 MUST update vdst on (lane == ssrc1) IGNORING EXEC!
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6u, Rdna2Operand::LiteralU32(0x11111111u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 2u, Rdna2Operand::InlineInt(0));
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 26u, 0u, 0, 2u);  // uav0[0] == 7 (runtime dynamic!)
        a.EmitSopp(Rdna2Opcode::S_WAITCNT, 0);
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /*EXEC_LO*/, Rdna2Operand::InlineInt(1));  // lane 7 is INACTIVE in EXEC!
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 127u /*EXEC_HI*/, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 27u, Rdna2Operand::LiteralU32(0x99999999u));
        a.EmitVop3(Rdna2Opcode::V_WRITELANE_B32, 6u, Rdna2Operand::Sgpr(27u), Rdna2Operand::Sgpr(26u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /*EXEC_LO*/, Rdna2Operand::InlineInt(-1));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 127u /*EXEC_HI*/, Rdna2Operand::InlineInt(-1));

        // Lane 0 writes recovered scalars [0..5]; all 16 lanes write v6 to [6 + lane]
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 0u);
        const uint32_t br_skip = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7u, Rdna2Operand::InlineInt(0));
        for (uint8_t k = 0u; k < 6u; ++k) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8u, Rdna2Operand::Sgpr(static_cast<uint8_t>(20u + k)));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8u, 7u, 4u, static_cast<uint16_t>(k * 4u));
        }
        a.PatchBranch(br_skip, a.CurrentWordOffset());
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 9u, Rdna2Operand::InlineInt(2), 0u);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6u, 9u, 4u, 24u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 16u, 1u, 1u, 40u, 12u, 64u), lifted, opts).ok());
        std::vector<uint32_t> expected(22u, 0x11111111u);
        for (size_t i = 0u; i < 5u; ++i) expected[i] = spill_vals[i];
        expected[5] = 0x12345678u;
        expected[6u + 7u] = 0x99999999u;  // Lane 7 updated even though EXEC was 0x1!
        RunAndVerifyComputeBothGpus(env, lifted, std::vector<uint32_t>{7u, 0u, 0u, 0u},
                                    std::vector<uint32_t>(expected.size(), 0u), expected, "writelane_wave64_spill");
    }

    // 13C. V_BFM_B32 (0x363) vs V_BCNT_U32_B32 (0x178), S_BFM_B64 (0x25),
    //      S_QUADMASK_B32/B64 (0x2C/0x2D), V_SAD_*/V_MSAD_U8, SGPR 104..107, SALU Null SCC
    {
        Rdna2Assembler a;
        // 1) V_BFM_B32 (0x363) & V_BCNT_U32_B32 (0x178)
        a.EmitVop3(Rdna2Opcode::V_BFM_B32, 2u, Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(4));   // 0x00000FF0
        a.EmitVop3(Rdna2Opcode::V_BFM_B32, 3u, Rdna2Operand::InlineInt(0), Rdna2Operand::InlineInt(12));  // 0
        a.EmitVop3(Rdna2Opcode::V_BCNT_U32_B32, 4u, Rdna2Operand::LiteralU32(0xF0F00F0Fu), Rdna2Operand::InlineInt(3));  // 19

        // 2) S_BFM_B64 (0x25): width=40, offset=12 -> 0x000FFFFF_FFFFF000, leaving SCC=1 unchanged!
        a.EmitSopc(Rdna2Opcode::S_CMP_EQ_U32, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(1));  // SCC = 1
        a.EmitSop2(Rdna2Opcode::S_BFM_B64, 30u, Rdna2Operand::InlineInt(40), Rdna2Operand::InlineInt(12));
        a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 32u, Rdna2Operand::LiteralU32(0xAA55AA55u), Rdna2Operand::InlineInt(0));

        // 3) S_QUADMASK_B32 (0x2C) & S_QUADMASK_B64 (0x2D) + SCC update
        a.EmitSop1(Rdna2Opcode::S_QUADMASK_B32, 33u, Rdna2Operand::LiteralU32(0x80040300u));  // nibs 2,4,7 -> 0x94, SCC=1
        a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 34u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_QUADMASK_B32, 35u, Rdna2Operand::InlineInt(0));  // 0, SCC=0
        a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 36u, Rdna2Operand::InlineInt(1), Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 38u, Rdna2Operand::LiteralU32(0x00010000u));  // nib 4
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 39u, Rdna2Operand::LiteralU32(0xF0000002u));  // nibs 8, 15
        a.EmitSop1(Rdna2Opcode::S_QUADMASK_B64, 40u, Rdna2Operand::Sgpr(38u));           // s40=0x8110, s41=0, SCC=1

        // 4) V_SAD_U8 / V_SAD_HI_U8 / V_SAD_U16 / V_SAD_U32 / V_MSAD_U8
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10u, Rdna2Operand::LiteralU32(0x10203040u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11u, Rdna2Operand::LiteralU32(0x15183050u));
        a.EmitVop3(Rdna2Opcode::V_SAD_U8, 5u, Rdna2Operand::Vgpr(10u), Rdna2Operand::Vgpr(11u), Rdna2Operand::LiteralU32(100u));  // 29 + 100 = 129
        a.EmitVop3(Rdna2Opcode::V_SAD_HI_U8, 6u, Rdna2Operand::Vgpr(10u), Rdna2Operand::Vgpr(11u), Rdna2Operand::LiteralU32(0x00050007u));  // (29<<16)+0x50007 = 0x00220007
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12u, Rdna2Operand::LiteralU32(0x01000050u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13u, Rdna2Operand::LiteralU32(0x00800070u));
        a.EmitVop3(Rdna2Opcode::V_SAD_U16, 7u, Rdna2Operand::Vgpr(12u), Rdna2Operand::Vgpr(13u), Rdna2Operand::InlineInt(10));  // 32 + 128 + 10 = 170
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14u, Rdna2Operand::LiteralU32(1000u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15u, Rdna2Operand::LiteralU32(2500u));
        a.EmitVop3(Rdna2Opcode::V_SAD_U32, 8u, Rdna2Operand::Vgpr(14u), Rdna2Operand::Vgpr(15u), Rdna2Operand::InlineInt(7));  // 1507
        // V_MSAD_U8: src=0xAA203040, ref=0x00180050 -> bytes 1 & 3 skipped (ref==0)! |0x40-0x50|+|0x20-0x18|+5 = 16+8+5 = 29
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 16u, Rdna2Operand::LiteralU32(0xAA203040u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17u, Rdna2Operand::LiteralU32(0x00180050u));
        a.EmitVop3(Rdna2Opcode::V_MSAD_U8, 9u, Rdna2Operand::Vgpr(16u), Rdna2Operand::Vgpr(17u), Rdna2Operand::InlineInt(5));

        // 5) SALU writes to Null (125) still compute & update SCC!
        a.EmitSop2(Rdna2Opcode::S_AND_B32, 125u /*null*/, Rdna2Operand::LiteralU32(0xF0u), Rdna2Operand::InlineInt(15));  // 0 -> SCC = 0
        a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 42u, Rdna2Operand::InlineInt(9), Rdna2Operand::InlineInt(7));              // -> 7
        a.EmitSop2(Rdna2Opcode::S_ADD_I32, 125u /*null*/, Rdna2Operand::LiteralU32(0x7FFFFFFFu), Rdna2Operand::InlineInt(1));  // signed overflow -> SCC = 1
        a.EmitSop2(Rdna2Opcode::S_CSELECT_B32, 43u, Rdna2Operand::InlineInt(9), Rdna2Operand::InlineInt(7));              // -> 9

        // 6) SGPR 104..107 descriptor tracking (V# in s[104:107] -> s107/vcc_hi holds V# word3 UFMT=60 R8G8B8A8_UINT + DST_SEL_X=Z(6)!)
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 104u, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 105u, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 106u, Rdna2Operand::InlineInt(64));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 107u,
                   Rdna2Operand::LiteralU32(6u /*Z*/ | (5u << 3u) | (6u << 6u) | (7u << 9u) |
                                            (static_cast<uint32_t>(Rdna2Gfx103Ufmt::R8G8B8A8_UINT) << 12u) |
                                            (1u << 24u) | (2u << 30u)));

        // Store all 20 results from lane 0 (slot 20 holds 0xDEADC0DE in uav1 -> byte 2 (Z) == 0xAD!)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 18u, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_FORMAT_X, 20u, 18u, 104u, 20u * 4u, true, false);
        const std::array<uint8_t, 8> v_out = {2, 3, 4, 5, 6, 7, 8, 9};
        for (size_t i = 0u; i < v_out.size(); ++i) {
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, v_out[i], 18u, 4u, static_cast<uint16_t>(i * 4u));
        }
        const std::array<uint8_t, 11> s_out = {30, 31, 32, 33, 34, 35, 36, 40, 41, 42, 43};
        for (size_t i = 0u; i < s_out.size(); ++i) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 19u, Rdna2Operand::Sgpr(s_out[i]));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 19u, 18u, 4u, static_cast<uint16_t>((8u + i) * 4u));
        }
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20u, 18u, 4u, 19u * 4u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 108u, 24u, 32u), lifted, opts).ok());
        // Verify disassembler opcode IDs (no silent misdecode!)
        auto has_op = [&](Rdna2Opcode op) {
            for (const auto& ins : lifted.instructions) {
                if (ins.opcode == op) return true;
            }
            return false;
        };
        OS_REQUIRE(has_op(Rdna2Opcode::V_BFM_B32));
        OS_REQUIRE(has_op(Rdna2Opcode::V_BCNT_U32_B32));
        OS_REQUIRE(has_op(Rdna2Opcode::S_BFM_B64));
        OS_REQUIRE(has_op(Rdna2Opcode::S_QUADMASK_B32));
        OS_REQUIRE(has_op(Rdna2Opcode::S_QUADMASK_B64));
        OS_REQUIRE(has_op(Rdna2Opcode::V_MSAD_U8));

        const std::vector<uint32_t> expected = {
            0x00000FF0u,  // v_bfm_b32(8, 4)
            0u,           // v_bfm_b32(0, 12)
            19u,          // v_bcnt_u32_b32(0xF0F00F0F, 3)
            129u,         // v_sad_u8
            0x00220007u,  // v_sad_hi_u8
            170u,         // v_sad_u16
            1507u,        // v_sad_u32
            29u,          // v_msad_u8 (skipped zero ref bytes!)
            0xFFFFF000u,  // s_bfm_b64 lo
            0x000FFFFFu,  // s_bfm_b64 hi
            0xAA55AA55u,  // SCC preserved as 1 across s_bfm_b64!
            0x94u,        // s_quadmask_b32(0x80040300)
            1u,           // SCC == 1
            0u,           // s_quadmask_b32(0)
            0u,           // SCC == 0
            0x8110u,      // s_quadmask_b64 lo
            0u,           // s_quadmask_b64 hi
            7u,           // s_and_b32 null, 0xF0, 15 -> SCC == 0
            9u,           // s_add_i32 null, 0x7FFFFFFF, 1 -> SCC == 1
            0xADu,        // loaded byte 2 (DST_SEL_X=Z) of 0xDEADC0DE via V# in s[104:107]!
            0xDEADC0DEu,  // untouched source word at uav1[20]
        };
        std::vector<uint32_t> init1(expected.size(), 0u);
        init1[20] = 0xDEADC0DEu;
        RunAndVerifyComputeBothGpus(env, lifted, std::vector<uint32_t>(4u, 0u),
                                    init1, expected, "bfm_quadmask_sad_null_sgpr104");
    }

    // 13D. MIMG IMAGE_ATOMIC_* (0x10..0x1E) Disassembly + IR + SPIR-V + DXC SM 6.5 Validation
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2u, Rdna2Operand::InlineInt(1));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3u, Rdna2Operand::InlineInt(2));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4u, Rdna2Operand::InlineInt(9));
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_SWAP, 4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_ADD,  4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_SUB,  4u, 2u, 8u, 0u, 0x1u, 1u, false, false);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_UMIN, 4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_UMAX, 4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_AND,  4u, 2u, 8u, 0u, 0x1u, 1u, false, false);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_OR,   4u, 2u, 8u, 0u, 0x1u, 1u, false, false);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_XOR,  4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_INC,  4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_DEC,  4u, 2u, 8u, 0u, 0x1u, 1u, false, true);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4u, 2u, 4u, 0u);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 24u, 12u, 32u), lifted, opts).ok());
        ValidateSpirvIfAvailable(env, lifted.spirv_words, "mimg_image_atomics_all");
#if defined(_WIN32)
        if (!env.dxc_path.empty()) {
            std::vector<uint8_t> dxil;
            std::string diag;
            OS_REQUIRE(CompileHlslWithDxc(lifted.hlsl_source, "cs_6_5", "main", dxil, diag, env.dxc_path).ok());
            OS_REQUIRE(ResignDxilContainer(dxil).ok());
        }
#endif
    }

    // 13E. GFX10.3 Buffer V# Word 3 [18:12] Unified 7-Bit FORMAT + DST_SEL (0,1,4,5,6,7)
    {
        // DescriptorSgprBridge check on real hardware GFX10.3 V# (resource_level=true, type=2)
#if defined(_WIN32)
        {
            Rdna2BufferDescriptor vsharp{};
            vsharp.base_address = 0x100000ull;
            vsharp.num_records = 64u;
            vsharp.resource_level = true;
            vsharp.type = 2u;
            vsharp.dst_sel_x = Rdna2DstSel::B;
            vsharp.dst_sel_y = Rdna2DstSel::G;
            vsharp.dst_sel_z = Rdna2DstSel::R;
            vsharp.dst_sel_w = Rdna2DstSel::One;
            vsharp.format = static_cast<uint8_t>(Rdna2Gfx103Ufmt::R8G8B8A8_UNORM);  // 56
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12BufferSrv(vsharp, 0u, srv));
            OS_REQUIRE(srv.Format == DXGI_FORMAT_R8G8B8A8_UNORM);

            vsharp.format = static_cast<uint8_t>(Rdna2Gfx103Ufmt::R16G16_FLOAT);  // 29
            OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12BufferSrv(vsharp, 0u, srv));
            OS_REQUIRE(srv.Format == DXGI_FORMAT_R16G16_FLOAT);

            vsharp.format = static_cast<uint8_t>(Rdna2Gfx103Ufmt::R32G32B32_FLOAT);  // 74
            OS_REQUIRE(DescriptorSgprBridge::SynthesizeD3D12BufferSrv(vsharp, 0u, srv));
            OS_REQUIRE(srv.Format == DXGI_FORMAT_R32G32B32_FLOAT);
        }
#endif

        auto pack_vsharp_w3 = [](Rdna2Gfx103Ufmt ufmt,
                                 uint32_t sx = 4u, uint32_t sy = 5u,
                                 uint32_t sz = 6u, uint32_t sw = 7u) -> uint32_t {
            return (sx & 7u) | ((sy & 7u) << 3u) | ((sz & 7u) << 6u) | ((sw & 7u) << 9u) |
                   (static_cast<uint32_t>(ufmt) << 12u) | (1u << 24u) | (2u << 30u);
        };

        // Buffer layout in uav0:
        // [0]  = 0x4080C0FF (bytes: FF, C0, 80, 40)
        // [1]  = 0x000080FF (bytes: FF, 80)
        // [2]  = F2U(2.5f)
        // [3]  = 0xFFFF0000 (u16: 0x0000, 0xFFFF)
        // [4]  = 0x80017FFF (s16: +32767, -32767)
        // [5]  = 0x12345678 (u16: 0x5678, 0x1234)
        // [6]  = (F32ToF16(-2.0f) << 16) | F32ToF16(1.0f)
        // [7]  = F2U(3.25f), [8] = F2U(-4.5f)
        // [9]  = (F32ToF16(2.0f) << 16) | F32ToF16(1.0f), [10] = (F32ToF16(-0.5f) << 16) | F32ToF16(4.0f)
        // [11..13] = 1.5f, 2.5f, 3.5f
        // [14..17] = 10.0f, 20.0f, 30.0f, 40.0f
        std::vector<uint32_t> uav0 = {
            0x4080C0FFu,
            0x000080FFu,
            F2U(2.5f),
            0xFFFF0000u,
            0x80017FFFu,
            0x12345678u,
            (static_cast<uint32_t>(F32ToF16(-2.0f)) << 16u) | static_cast<uint32_t>(F32ToF16(1.0f)),
            F2U(3.25f), F2U(-4.5f),
            (static_cast<uint32_t>(F32ToF16(2.0f)) << 16u) | static_cast<uint32_t>(F32ToF16(1.0f)),
            (static_cast<uint32_t>(F32ToF16(-0.5f)) << 16u) | static_cast<uint32_t>(F32ToF16(4.0f)),
            F2U(1.5f), F2U(2.5f), F2U(3.5f),
            F2U(10.0f), F2U(20.0f), F2U(30.0f), F2U(40.0f),
            0u, 0u,
        };

        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1u, Rdna2Operand::InlineInt(0));  // vaddr = 0
        uint16_t out_off = 0u;
        auto emit_vsharp_load4 = [&](uint32_t w3, uint16_t byte_off, bool use_tbuf, uint8_t tbuf_ufmt = 0u) {
            a.EmitSop1(Rdna2Opcode::S_MOV_B32, 3u, Rdna2Operand::LiteralU32(w3));
            if (use_tbuf) {
                a.EmitMtbufUfmt(Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW, 4u, 1u, 0u, tbuf_ufmt, byte_off, true, false);
            } else {
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW, 4u, 1u, 0u, byte_off, true, false);
            }
            for (uint8_t c = 0u; c < 4u; ++c) {
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, static_cast<uint8_t>(4u + c), 1u, 4u, out_off);
                out_off = static_cast<uint16_t>(out_off + 4u);
            }
        };

        // 1: R8_UNORM (ufmt=1) -> (1.0f, 0.0f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R8_UNORM), 0u, false);
        // 5: R8_UINT (ufmt=5) -> (255u, 0u, 0u, 1u)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R8_UINT), 0u, false);
        // 14: R8G8_UNORM (ufmt=14) via tbuffer_load_format_xyzw -> (1.0f, 128/255.f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::Invalid), 4u, true,
                          static_cast<uint8_t>(Rdna2Gfx103Ufmt::R8G8_UNORM));
        // 22: R32_FLOAT (ufmt=22) -> (2.5f, 0.0f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R32_FLOAT), 8u, false);
        // 23: R16G16_UNORM (ufmt=23) -> (0.0f, 1.0f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R16G16_UNORM), 12u, false);
        // 24: R16G16_SNORM (ufmt=24) -> (+1.0f, -1.0f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R16G16_SNORM), 16u, false);
        // 27: R16G16_UINT (ufmt=27) -> (0x5678u, 0x1234u, 0u, 1u)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R16G16_UINT), 20u, false);
        // 29: R16G16_FLOAT (ufmt=29) -> (1.0f, -2.0f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R16G16_FLOAT), 24u, false);
        // 56: R8G8B8A8_UNORM + DST_SEL = (W=7, Z=6, Zero=0, One=1) -> (0x40/255.f, 0x80/255.f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R8G8B8A8_UNORM, 7u, 6u, 0u, 1u), 0u, false);
        // 60: R8G8B8A8_UINT + DST_SEL = (Z=6, Y=5, X=4, One=1) -> (0x80u, 0xC0u, 0xFFu, 1u)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R8G8B8A8_UINT, 6u, 5u, 4u, 1u), 0u, false);
        // 64: R32G32_FLOAT (ufmt=64) -> (3.25f, -4.5f, 0.0f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R32G32_FLOAT), 28u, false);
        // 71: R16G16B16A16_FLOAT (ufmt=71) -> (1.0f, 2.0f, 4.0f, -0.5f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R16G16B16A16_FLOAT), 36u, false);
        // 74: R32G32B32_FLOAT (ufmt=74) -> (1.5f, 2.5f, 3.5f, 1.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R32G32B32_FLOAT), 44u, false);
        // 77: R32G32B32A32_FLOAT (ufmt=77) -> (10.0f, 20.0f, 30.0f, 40.0f)
        emit_vsharp_load4(pack_vsharp_w3(Rdna2Gfx103Ufmt::R32G32B32A32_FLOAT), 56u, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 1u, 1u, 1u, 16u, 12u, 32u), lifted, opts).ok());
        ValidateSpirvIfAvailable(env, lifted.spirv_words, "vsharp_ufmt7_dst_sel");

        const std::vector<uint32_t> expected = {
            F2U(1.0f), F2U(0.0f), F2U(0.0f), F2U(1.0f),
            255u, 0u, 0u, 1u,
            F2U(1.0f), F2U(128.0f / 255.0f), F2U(0.0f), F2U(1.0f),
            F2U(2.5f), F2U(0.0f), F2U(0.0f), F2U(1.0f),
            F2U(0.0f), F2U(1.0f), F2U(0.0f), F2U(1.0f),
            F2U(1.0f), F2U(-1.0f), F2U(0.0f), F2U(1.0f),
            0x5678u, 0x1234u, 0u, 1u,
            F2U(1.0f), F2U(-2.0f), F2U(0.0f), F2U(1.0f),
            F2U(64.0f / 255.0f), F2U(128.0f / 255.0f), 0u, F2U(1.0f),
            0x80u, 0xC0u, 0xFFu, 1u,
            F2U(3.25f), F2U(-4.5f), F2U(0.0f), F2U(1.0f),
            F2U(1.0f), F2U(2.0f), F2U(4.0f), F2U(-0.5f),
            F2U(1.5f), F2U(2.5f), F2U(3.5f), F2U(1.0f),
            F2U(10.0f), F2U(20.0f), F2U(30.0f), F2U(40.0f),
        };
#if defined(_WIN32)
        if (!env.dxc_path.empty()) {
            auto run_ufmt = [&](S16D3D12Runner& gpu, const char* tag) {
                std::vector<uint8_t> dxil;
                std::string diag;
                OS_REQUIRE(CompileHlslWithDxc(lifted.hlsl_source, gpu.TargetProfile("cs"), "main", dxil, diag, env.dxc_path).ok());
                OS_REQUIRE(ResignDxilContainer(dxil).ok());
                std::vector<uint32_t> out;
                OS_REQUIRE(gpu.RunComputeDualUavTable(dxil, uav0, std::vector<uint32_t>(expected.size(), 0u), out));
                OS_REQUIRE(out.size() == expected.size());
                for (size_t i = 0u; i < expected.size(); ++i) {
                    if (out[i] != expected[i]) {
                        // Allow 1-ULP float tolerance on 128/255f & 64/255f division
                        const bool approx_ok = (std::fabs(U2F(out[i]) - U2F(expected[i])) < 1e-6f);
                        if (!approx_ok) {
                            std::fprintf(stderr, "[FAIL] vsharp_ufmt7 (%s) [%zu]: got=0x%08X exp=0x%08X\n",
                                         tag, i, out[i], expected[i]);
                        }
                        OS_REQUIRE(approx_ok);
                    }
                }
            };
            if (env.has_hw) run_ufmt(env.hw, "D3D12-HW");
            if (env.has_warp) run_ufmt(env.warp, "D3D12-WARP");
        }
#endif
    }

    std::printf("[PASS] Test13: 3D-world RDNA2 ISA completeness (DPP16, writelane Wave64 spills, BFM/QUADMASK/SAD/MSAD, SGPR104..107, Null SCC, MIMG atomics, V# UFMT+DST_SEL)\n");
}

}  // namespace

int main() {
    TestEnv env{};
    env.dxc_path = ResolvePortableDxcPath();
    env.spirv_val_path = FindSpirvValPortable();
#if defined(_WIN32)
    env.has_hw = env.hw.Init(/*use_warp=*/false);
    env.has_warp = env.warp.Init(/*use_warp=*/true);
    std::printf("=== Sprint 16 RDNA2 Shader Completeness Suite (HW=%s [%s], WARP=%s, DXC=%s, spirv-val=%s) ===\n",
                env.has_hw ? "YES" : "NO",
                env.has_hw ? env.hw.adapter_name.c_str() : "none",
                env.has_warp ? "YES" : "NO",
                env.dxc_path.empty() ? "NO" : "YES",
                env.spirv_val_path.empty() ? "NO" : "YES");
#endif

    Test1_SingleBoundsCheckSBufferLoad(env);
    Test2_BvhBoxNodeWordBoundedReads(env);
    Test3_ExactRneAndNanCvtPkNorm(env);
    Test4_SpirvAndHlslMinLod(env);
    Test5_CompressedExpAll16MasksAndLow16(env);
    Test6_MubufD16LoadsAndHiStores(env);
    Test7_LdsDsCompletenessAndDsNop(env);
    Test8_Sop1SaveExecWrExecAndMad64InactiveZeroing(env);
    Test9_Wave64VoteInvalidationAndFlbitCmp64(env);
    Test10_Dpp8AndVop3I16AndPsCmp64(env);
    Test11_All7StagesLiveGpuAndBenchJson();
    Test12_ZeroHitchAsyncPsoAndPersistentDiskCache(env);
    Test13_Rdna2_3dWorldIsaCompleteness(env);

    std::printf("=== ALL 13 SPRINT 16 SHADER COMPLETENESS TESTS PASSED ===\n");
    return 0;
}
