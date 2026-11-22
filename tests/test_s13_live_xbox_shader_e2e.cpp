#include <windows.h>

#include <initguid.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/d3d12_xs_abi.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "os_check.h"

namespace {

using namespace openscarlett;

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

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
    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

constexpr uint64_t AlignUp64(uint64_t v, uint64_t a) noexcept {
    return (v + a - 1ull) & ~(a - 1ull);
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE TypeVal, typename InnerType>
struct alignas(void*) StreamSubobj {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = TypeVal;
    InnerType inner{};
};

struct MeshStreamWithRootSig {
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_sig;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
};

struct MeshStreamNoRootSig {
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rt_formats;
    StreamSubobj<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool HasHardwareGpu(D3D12AdapterInfo& out_info) {
    D3D12AdapterSelectionOptions opts{};
    opts.prefer_warp = false;
    opts.allow_warp_fallback = false;
    ComPtr<ID3D12Device> dev;
    ComPtr<IDXGIAdapter1> ad;
    const Status st = SelectD3D12AdapterAndCreateDevice(opts, dev.Put(), ad.Put(), &out_info);
    return st.ok() && dev.Get() != nullptr && !out_info.is_warp;
}

struct LiveDeviceFixture {
    ComPtr<ID3D12DeviceX> device;
    ComPtr<IOpenScarlettUnifiedMemory> unified;
    ComPtr<ID3D12CommandQueueX> queue;
    ComPtr<IOpenScarlettPresentCapture> present_capture;
    ComPtr<IOpenScarlettPresentWindow> present_window;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandListX> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0ull;
    D3D12XBOX_GPU_HARDWARE_INFO gpu_info{};

    ~LiveDeviceFixture() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    void Init(bool prefer_warp) {
        OpenScarlettD3D12SetBackendMode(D3D12XBOX_BACKEND_D3D12);
        OpenScarlettD3D12SetPreferWarp(prefer_warp ? 1 : 0);

        D3D12XBOX_CREATE_DEVICE_PARAMETERS params{};
        params.Version = 1u;
        OS_REQUIRE(SUCCEEDED(D3D12XboxCreateDevice(
            nullptr, &params, IID_ID3D12DeviceX, device.PutVoid())));
        OS_REQUIRE(device.Get() != nullptr);
        OS_REQUIRE(SUCCEEDED(device->GetGpuHardwareInfoX(&gpu_info)));
        OS_REQUIRE((gpu_info.IsWarp != 0u) == prefer_warp);

        OS_REQUIRE(SUCCEEDED(device->QueryInterface(
            IID_IOpenScarlettUnifiedMemory, unified.PutVoid())));

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        OS_REQUIRE(SUCCEEDED(device->CreateCommandQueue(
            &qd, IID_ID3D12CommandQueueX, queue.PutVoid())));
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentCapture, present_capture.PutVoid())));
        OS_REQUIRE(SUCCEEDED(queue->QueryInterface(
            IID_IOpenScarlettPresentWindow, present_window.PutVoid())));

        OS_REQUIRE(SUCCEEDED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, alloc.PutVoid())));
        OS_REQUIRE(SUCCEEDED(device->CreateCommandList(
            0u, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
            IID_ID3D12GraphicsCommandListX, cl.PutVoid())));
        OS_REQUIRE(SUCCEEDED(cl->Close()));

        OS_REQUIRE(SUCCEEDED(device->CreateFence(
            0ull, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.PutVoid())));
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        OS_REQUIRE(fence_event != nullptr);
    }

    void ResetList(ID3D12PipelineState* pso = nullptr) {
        OS_REQUIRE(SUCCEEDED(alloc->Reset()));
        OS_REQUIRE(SUCCEEDED(cl->Reset(alloc.Get(), pso)));
    }

    void SubmitAndWait() {
        OS_REQUIRE(SUCCEEDED(cl->Close()));
        ID3D12CommandList* lists[1] = {cl.Get()};
        queue->ExecuteCommandLists(1u, lists);
        ++fence_val;
        OS_REQUIRE(SUCCEEDED(queue->Signal(fence.Get(), fence_val)));
        if (fence->GetCompletedValue() < fence_val) {
            OS_REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(fence_val, fence_event)));
            OS_REQUIRE(WaitForSingleObject(fence_event, 15000u) == WAIT_OBJECT_0);
        }
    }

    ComPtr<ID3D12Resource> CreateBuffer(
        uint64_t bytes,
        D3D12_HEAP_TYPE heap_type,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initial_state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> res;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
            IID_ID3D12Resource, res.PutVoid())));
        return res;
    }

    ComPtr<ID3D12Resource> CreateRenderTarget(uint32_t w, uint32_t h) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1u;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource> rt;
        OS_REQUIRE(SUCCEEDED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_ID3D12Resource, rt.PutVoid())));
        return rt;
    }

    ComPtr<ID3D12RootSignature> CreateRootSig(const D3D12_ROOT_SIGNATURE_DESC& rsd) {
        ComPtr<ID3DBlob> blob, err;
        OS_REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.Put(), err.Put())));
        ComPtr<ID3D12RootSignature> rs;
        OS_REQUIRE(SUCCEEDED(device->CreateRootSignature(
            0u, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_ID3D12RootSignature, rs.PutVoid())));
        return rs;
    }
};

// =============================================================================
// Shader Container Builders (Real Xbox DXBC + PM4 + 24-byte ISGN/OSGN + RDNA2)
// =============================================================================

// Vertex shader reading BOTH v0 (SV_VertexID) AND v3 (SV_InstanceID):
//   Instance 0 (v3=0): triangle covering lower-left half  (-1,-1), (-1,+1), (0,-1), Param0.z = 0.25
//   Instance 1 (v3=1): triangle covering lower-right half ( 0,-1), ( 0,+1), (1,-1), Param0.z = 0.75
std::vector<uint8_t> BuildInstancedVsXboxDxbc24() {
    Rdna2Assembler vs;
    vs.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(3));                // v10 = float(v3 = SV_InstanceID)
    vs.EmitVop2(Rdna2Opcode::V_ADD_F32, 11, Rdna2Operand::InlineFloat(-1.0f), 10);     // v11 = base_x (-1.0 or 0.0)
    vs.EmitVop2(Rdna2Opcode::V_ADD_F32, 12, Rdna2Operand::InlineFloat(1.0f), 11);      // v12 = base_x + 1.0 (0.0 or 1.0)
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::Vgpr(11));                    // v1 = x = base_x
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);             // v0 == 2?
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 12);             // if v0==2: x = base_x + 1.0
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));          // v2 = y = -1.0
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineFloat(1.0f));          // v13 = +1.0
    vs.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);             // v0 == 1?
    vs.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 13);             // if v0==1: y = +1.0
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::InlineFloat(0.0f));          // z = 0.0
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::InlineFloat(1.0f));          // w = 1.0

    // Param0 = (u=0.5, v=0.5, inst_blue = 0.25 + 0.5*v10, a=1.0)
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    vs.EmitVop2(Rdna2Opcode::V_MUL_F32, 7, Rdna2Operand::InlineFloat(0.5f), 10);
    vs.EmitVop2(Rdna2Opcode::V_ADD_F32, 7, Rdna2Operand::LiteralF32(0.25f), 7);
    vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));

    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 2, 14, 15, 0xFu, false);
    vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 5, 6, 7, 8, 0xFu, true);
    vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Vertex;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.embed_pm4_in_shader_chunk = true;
    desc.raw_rdna_isa_words = vs.Words();
    desc.use_sig_v1 = false;  // 24-byte ISGN / OSGN!
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Vertex;
    desc.reg_setup.vgpr_count = 16u;
    desc.reg_setup.sgpr_count = 16u;
    desc.reg_setup.user_sgpr_count = 2u;

    XboxSignatureParamDesc vid{};
    vid.semantic_name = "SV_VertexID";
    vid.system_value_type = 6u;  // D3D_NAME_VERTEX_ID
    vid.component_type = 1u;     // UINT
    vid.register_index = 0u;
    vid.mask = 0x1u;
    vid.rw_mask = 0x1u;
    desc.input_signature.push_back(vid);

    XboxSignatureParamDesc iid{};
    iid.semantic_name = "SV_InstanceID";
    iid.system_value_type = 8u;  // D3D_NAME_INSTANCE_ID
    iid.component_type = 1u;     // UINT
    iid.register_index = 1u;
    iid.mask = 0x1u;
    iid.rw_mask = 0x1u;
    desc.input_signature.push_back(iid);

    XboxSignatureParamDesc out_pos{};
    out_pos.semantic_name = "SV_Position";
    out_pos.system_value_type = 1u;
    out_pos.component_type = 3u;
    out_pos.register_index = 0u;
    desc.output_signature.push_back(out_pos);

    XboxSignatureParamDesc out_tc{};
    out_tc.semantic_name = "TEXCOORD";
    out_tc.semantic_index = 0u;
    out_tc.component_type = 3u;
    out_tc.register_index = 1u;
    desc.output_signature.push_back(out_tc);

    return BuildFullXboxDxbcContainer(desc);
}

// Pixel shader loading texel (0,0) from unified Texture2D t0 via IMAGE_LOAD and combining
// (tex.r, tex.g, Param0.z, 1.0) so both unified texture updates and VS v3 instance tag are verified!
std::vector<uint8_t> BuildUnifiedTexPsXboxDxbc24() {
    Rdna2Assembler ps;
    // Save Param0.z (v2) before image_load, load texel (0,0) from t0 into v4..v7
    ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));  // coord x = 0
    ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(0));  // coord y = 0
    ps.EmitMimg(Rdna2Opcode::IMAGE_LOAD, 4, 10, 0, 0, 0xFu, 1u, false);   // v4..v7 = _tex0.Load(int3(0,0,0))
    ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 2, 8, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Pixel;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.embed_pm4_in_shader_chunk = false;  // Separate PM4R chunk
    desc.raw_rdna_isa_words = ps.Words();
    desc.use_sig_v1 = false;  // 24-byte ISGN / OSGN!
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Pixel;
    desc.reg_setup.vgpr_count = 16u;
    desc.reg_setup.sgpr_count = 16u;
    desc.reg_setup.user_sgpr_count = 2u;
    desc.reg_setup.persp_center_ena = true;

    XboxSignatureParamDesc in_pos{};
    in_pos.semantic_name = "SV_Position";
    in_pos.system_value_type = 1u;
    in_pos.component_type = 3u;
    in_pos.register_index = 0u;
    desc.input_signature.push_back(in_pos);

    XboxSignatureParamDesc in_tc{};
    in_tc.semantic_name = "TEXCOORD";
    in_tc.semantic_index = 0u;
    in_tc.component_type = 3u;
    in_tc.register_index = 1u;
    desc.input_signature.push_back(in_tc);

    XboxSignatureParamDesc out_rt{};
    out_rt.semantic_name = "SV_Target";
    out_rt.semantic_index = 0u;
    out_rt.component_type = 3u;
    out_rt.register_index = 0u;
    desc.output_signature.push_back(out_rt);

    XboxRootSignatureDesc rsd{};
    rsd.present = true;
    rsd.version = 2u;
    XboxRootParameter tbl{};
    tbl.type = XboxRootParamType::DescriptorTable;
    tbl.shader_visibility = 0u;
    XboxDescriptorRange r{};
    r.range_type = XboxDescriptorRangeType::Srv;
    r.num_descriptors = 1u;
    r.base_shader_register = 0u;
    tbl.descriptor_ranges.push_back(r);
    rsd.parameters.push_back(tbl);
    desc.root_signature = rsd;

    return BuildFullXboxDxbcContainer(desc);
}

// Compute shader in Xbox DXBC + PM4R + 24-byte ISGN/OSGN + RDNA chunk:
//   out[tid] = (in[tid] * 7u + 13u) ^ 0x5A5Au
std::vector<uint8_t> BuildComputeXboxDxbc24() {
    Rdna2Assembler cs;
    cs.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    cs.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0);
    cs.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(7));
    cs.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::InlineInt(13), 3);
    cs.EmitVop2(Rdna2Opcode::V_XOR_B32, 4, Rdna2Operand::LiteralU32(0x5A5Au), 3);
    cs.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4);
    cs.EmitSopp(Rdna2Opcode::S_ENDPGM);

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Compute;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.embed_pm4_in_shader_chunk = false;
    desc.raw_rdna_isa_words = cs.Words();
    desc.use_sig_v1 = false;  // 24-byte ISGN/OSGN
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Compute;
    desc.reg_setup.vgpr_count = 16u;
    desc.reg_setup.sgpr_count = 16u;
    desc.reg_setup.user_sgpr_count = 4u;
    desc.reg_setup.threadgroup_x = 64u;
    desc.reg_setup.threadgroup_y = 1u;
    desc.reg_setup.threadgroup_z = 1u;

    XboxSignatureParamDesc tid{};
    tid.semantic_name = "SV_DispatchThreadID";
    tid.component_type = 1u;
    tid.mask = 0x7u;
    desc.input_signature.push_back(tid);

    XboxRootSignatureDesc rsd{};
    rsd.present = true;
    rsd.version = 2u;
    XboxRootParameter u0{};
    u0.type = XboxRootParamType::Uav;
    u0.shader_register = 0u;
    XboxRootParameter u1{};
    u1.type = XboxRootParamType::Uav;
    u1.shader_register = 1u;
    rsd.parameters.push_back(u0);
    rsd.parameters.push_back(u1);
    desc.root_signature = rsd;

    return BuildFullXboxDxbcContainer(desc);
}

// Mesh shader WITHOUT RTS0 (xbox-emulator #129):
// Reads float R color channel from UAV buffer u0 at byte offset 0 via BUFFER_LOAD_DWORD,
// emits fullscreen triangle with Param0 = (R_from_u0, 0.5, 0.25, 1.0)!
std::vector<uint8_t> BuildMeshNoRts0XboxDxbc24() {
    Rdna2Assembler ms;
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
    ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
    ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));

    // Fullscreen triangle positions by lane v0 in {0, 1, 2}
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(-1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(0.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(1.0f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralF32(3.0f));
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 2, Rdna2Operand::Vgpr(2), 9);
    ms.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 0);
    ms.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 1, Rdna2Operand::Vgpr(1), 9);

    // Load R color float bits from u0[0] via MUBUF (triggers descriptor-table inference when RTS0 absent!)
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineInt(0));
    ms.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 5, 13, 0);
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::InlineFloat(0.5f));
    ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(0.25f));
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

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Mesh;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.embed_pm4_in_shader_chunk = true;
    desc.raw_rdna_isa_words = ms.Words();
    desc.use_sig_v1 = false;  // 24-byte OSGN
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Mesh;
    desc.reg_setup.vgpr_count = 16u;
    desc.reg_setup.sgpr_count = 16u;
    desc.reg_setup.user_sgpr_count = 4u;
    desc.reg_setup.threadgroup_x = 3u;
    desc.reg_setup.threadgroup_y = 1u;
    desc.reg_setup.threadgroup_z = 1u;
    // Deliberately NO RTS0 chunk (desc.root_signature.present == false)!

    XboxSignatureParamDesc out_pos{};
    out_pos.semantic_name = "SV_Position";
    out_pos.system_value_type = 1u;
    out_pos.component_type = 3u;
    out_pos.register_index = 0u;
    desc.output_signature.push_back(out_pos);

    XboxSignatureParamDesc out_tc{};
    out_tc.semantic_name = "TEXCOORD";
    out_tc.semantic_index = 0u;
    out_tc.component_type = 3u;
    out_tc.register_index = 1u;
    desc.output_signature.push_back(out_tc);

    return BuildFullXboxDxbcContainer(desc);
}

// Simple passthrough PS for Mesh pipeline (outputs Param0 directly)
std::vector<uint8_t> BuildPassthroughPsXboxDxbc24() {
    Rdna2Assembler ps;
    ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 0, 1, 2, 3, 0xFu, true, false, true);
    ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Pixel;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.raw_rdna_isa_words = ps.Words();
    desc.use_sig_v1 = false;
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Pixel;
    desc.reg_setup.vgpr_count = 8u;
    desc.reg_setup.sgpr_count = 8u;
    desc.reg_setup.persp_center_ena = true;

    XboxSignatureParamDesc in_tc{};
    in_tc.semantic_name = "TEXCOORD";
    in_tc.semantic_index = 0u;
    in_tc.component_type = 3u;
    in_tc.register_index = 0u;
    desc.input_signature.push_back(in_tc);

    XboxSignatureParamDesc out_rt{};
    out_rt.semantic_name = "SV_Target";
    out_rt.semantic_index = 0u;
    out_rt.component_type = 3u;
    out_rt.register_index = 0u;
    desc.output_signature.push_back(out_rt);

    return BuildFullXboxDxbcContainer(desc);
}

// RayQuery Compute shader in Xbox DXBC + PM4R + 24-byte ISGN + RDNA chunk containing
// real RDNA2 image_bvh64_intersect_ray (0xE7) or image_bvh_intersect_ray (0xE6)!
std::vector<uint8_t> BuildRayQueryXboxDxbc24(bool use_bvh64) {
    Rdna2Assembler cs;
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralF32(1000.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineFloat(0.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineFloat(0.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::InlineFloat(0.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::InlineFloat(0.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::InlineFloat(0.0f));
    cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 17, Rdna2Operand::InlineFloat(1.0f));

    std::vector<uint32_t> words = cs.Words();
    if (use_bvh64) {
        EmitRawBvh64IntersectRay(words, /*vdata=*/20u, /*vaddr=*/10u, /*srsrc=*/0u, 0xFu);
    } else {
        Rdna2Assembler bvh32;
        bvh32.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 20u, 10u, 0u, 0u, 0xFu, 0u, false);
        words.insert(words.end(), bvh32.Words().begin(), bvh32.Words().end());
    }
    Rdna2Assembler ep;
    ep.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20, 0, 0);
    ep.EmitSopp(Rdna2Opcode::S_ENDPGM);
    words.insert(words.end(), ep.Words().begin(), ep.Words().end());

    XboxFullDxbcBuildDesc desc{};
    desc.stage = ShaderStage::Compute;
    desc.xbox_chunk_fourcc = "RDNA";
    desc.embed_pm4_in_shader_chunk = false;
    desc.raw_rdna_isa_words = words;
    desc.use_sig_v1 = false;
    desc.reg_setup.present = true;
    desc.reg_setup.inferred_stage = ShaderStage::Compute;
    desc.reg_setup.vgpr_count = 32u;
    desc.reg_setup.sgpr_count = 16u;
    desc.reg_setup.user_sgpr_count = 6u;
    desc.reg_setup.threadgroup_x = 64u;
    desc.reg_setup.threadgroup_y = 1u;
    desc.reg_setup.threadgroup_z = 1u;

    XboxSignatureParamDesc tid{};
    tid.semantic_name = "SV_DispatchThreadID";
    tid.component_type = 1u;
    tid.mask = 0x7u;
    desc.input_signature.push_back(tid);

    // Explicit 3-SRV multi-window BVH4 layout (b0, t0..t2, u0) matching rdna2_rayquery_bridge fallback
    XboxRootSignatureDesc rsd{};
    rsd.present = true;
    rsd.version = 2u;
    XboxRootParameter b0{};
    b0.type = XboxRootParamType::Cbv;
    b0.shader_register = 0u;
    rsd.parameters.push_back(b0);
    for (uint32_t t = 0u; t < 3u; ++t) {
        XboxRootParameter srv{};
        srv.type = XboxRootParamType::Srv;
        srv.shader_register = t;
        rsd.parameters.push_back(srv);
    }
    XboxRootParameter u0{};
    u0.type = XboxRootParamType::Uav;
    u0.shader_register = 0u;
    rsd.parameters.push_back(u0);
    desc.root_signature = rsd;

    return BuildFullXboxDxbcContainer(desc);
}

// =============================================================================
// Suite 1: 24-byte ISGN/OSGN vs 32-byte ISG1/OSG1 + Detection + Bounds Safety
// =============================================================================
void TestContainerParsingAndBounds() {
    const auto vs_24 = BuildInstancedVsXboxDxbc24();
    OS_REQUIRE(IsXboxShaderContainerBlob(vs_24.data(), vs_24.size()));

    XboxParsedShaderContainer parsed_vs{};
    OS_REQUIRE(ParseFullXboxShaderContainer(vs_24, parsed_vs).ok());
    OS_REQUIRE(parsed_vs.has_xs2);
    OS_REQUIRE(parsed_vs.input_sig_element_stride == 24u);
    OS_REQUIRE(parsed_vs.output_sig_element_stride == 24u);
    OS_REQUIRE(parsed_vs.input_signature.size() == 2u);
    OS_REQUIRE(parsed_vs.input_signature[0].semantic_name == "SV_VertexID");
    OS_REQUIRE(parsed_vs.input_signature[1].semantic_name == "SV_InstanceID");
    OS_REQUIRE(parsed_vs.vs_prologue.uses_vertex_id_v0);
    OS_REQUIRE(parsed_vs.vs_prologue.uses_instance_id_v3);
    OS_REQUIRE(parsed_vs.vs_prologue.declared_vertex_id_in_sig);
    OS_REQUIRE(parsed_vs.vs_prologue.declared_instance_id_in_sig);

    // Also verify 32-byte ISG1/OSG1 variant parses with stride == 32
    XboxFullDxbcBuildDesc d32{};
    d32.stage = ShaderStage::Compute;
    d32.xbox_chunk_fourcc = "RDNA";
    Rdna2Assembler a;
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    d32.raw_rdna_isa_words = a.Words();
    d32.use_sig_v1 = true;
    XboxSignatureParamDesc sp{};
    sp.semantic_name = "SV_DispatchThreadID";
    sp.component_type = 1u;
    d32.input_signature.push_back(sp);
    d32.output_signature.push_back(sp);
    const auto cs_32 = BuildFullXboxDxbcContainer(d32);
    XboxParsedShaderContainer parsed_32{};
    OS_REQUIRE(ParseFullXboxShaderContainer(cs_32, parsed_32).ok());
    OS_REQUIRE(parsed_32.input_sig_element_stride == 32u);
    OS_REQUIRE(parsed_32.output_sig_element_stride == 32u);

    // Verify standard host PC DXBC is NOT misclassified as Xbox container
    const char* host_cs = "[numthreads(1,1,1)] void main() {}";
    ComPtr<ID3DBlob> host_bc, host_err;
    OS_REQUIRE(SUCCEEDED(D3DCompile(
        host_cs, std::strlen(host_cs), "host_noop.hlsl", nullptr, nullptr,
        "main", "cs_5_0", 0u, 0u, host_bc.Put(), host_err.Put())));
    OS_REQUIRE(!IsXboxShaderContainerBlob(host_bc->GetBufferPointer(), host_bc->GetBufferSize()));

    // Verify truncated buffer fails cleanly without crash or OOB read
    XboxParsedShaderContainer bad{};
    OS_REQUIRE(!ParseFullXboxShaderContainer(vs_24.data(), 28u, bad).ok());

    std::printf("[PASS] Suite 1: 24-byte ISGN/OSGN + 32-byte ISG1/OSG1 + VS v0/v3 prologue detection + bounds safety\n");
}

// =============================================================================
// Suite 2: Live Graphics (VS v0+v3 + PS Unified Texture) E2E on HW GPU & WARP
// =============================================================================
void TestGraphicsVsV0V3AndUnifiedTexLive(LiveDeviceFixture& fx, const char* tag) {
    const auto vs_dxbc = BuildInstancedVsXboxDxbc24();
    const auto ps_dxbc = BuildUnifiedTexPsXboxDxbc24();

    // Root signature with 1 SRV descriptor table (t0)
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1u;
    srv_range.BaseShaderRegister = 0u;
    srv_range.RegisterSpace = 0u;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 1u;
    rp.DescriptorTable.pDescriptorRanges = &srv_range;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1u;
    rsd.pParameters = &rp;
    auto rs = fx.CreateRootSig(rsd);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gpd{};
    gpd.pRootSignature = rs.Get();
    gpd.VS = {vs_dxbc.data(), vs_dxbc.size()};
    gpd.PS = {ps_dxbc.data(), ps_dxbc.size()};
    gpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gpd.SampleMask = 0xFFFFFFFFu;
    gpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gpd.NumRenderTargets = 1u;
    gpd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gpd.SampleDesc = {1u, 0u};

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = fx.device->CreateGraphicsPipelineState(&gpd, IID_ID3D12PipelineState, pso.PutVoid());
    OS_REQUIRE(SUCCEEDED(hr));
    OS_REQUIRE(pso.Get() != nullptr);

    // Create unified-memory 2D texture (16x16 RGBA8) backed by MEM_WRITE_WATCH CPU pages!
    constexpr uint32_t kTexW = 16u, kTexH = 16u;
    const size_t tex_bytes = static_cast<size_t>(kTexW) * kTexH * 4u;
    void* cpu_tex = VirtualAlloc(
        nullptr, 65536u, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    OS_REQUIRE(cpu_tex != nullptr);

    auto fill_cpu_tex = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        auto* p = static_cast<uint8_t*>(cpu_tex);
        for (size_t i = 0; i < kTexW * kTexH; ++i) {
            p[i * 4u + 0u] = r;
            p[i * 4u + 1u] = g;
            p[i * 4u + 2u] = b;
            p[i * 4u + 3u] = a;
        }
    };
    fill_cpu_tex(200u, 50u, 10u, 255u);

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = kTexW;
    tex_desc.Height = kTexH;
    tex_desc.DepthOrArraySize = 1u;
    tex_desc.MipLevels = 1u;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc = {1u, 0u};

    ComPtr<ID3D12Resource> unified_tex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreatePlacedResourceX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(cpu_tex),
        &tex_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
        IID_ID3D12Resource, unified_tex.PutVoid())));

    D3D12_DESCRIPTOR_HEAP_DESC srv_hd{};
    srv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_hd.NumDescriptors = 1u;
    srv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &srv_hd, IID_ID3D12DescriptorHeap, srv_heap.PutVoid())));
    fx.device->CreateShaderResourceView(
        unified_tex.Get(), nullptr, srv_heap->GetCPUDescriptorHandleForHeapStart());

    constexpr uint32_t kW = 64u, kH = 64u, kPitch = kW * 4u, kTotal = kPitch * kH;
    auto rt = fx.CreateRenderTarget(kW, kH);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 1u;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(
        &rtv_hd, IID_ID3D12DescriptorHeap, rtv_heap.PutVoid())));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    fx.device->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);
    auto rb = fx.CreateBuffer(kTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    auto draw_and_readback = [&](std::vector<uint8_t>& out_rgba) {
        out_rgba.assign(kTotal, 0u);
        fx.ResetList(pso.Get());
        const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        fx.cl->ClearRenderTargetView(rtv_h, clr, 0u, nullptr);
        fx.cl->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        fx.cl->RSSetViewports(1u, &vp);
        fx.cl->RSSetScissorRects(1u, &sc);
        ID3D12DescriptorHeap* heaps[1] = {srv_heap.Get()};
        fx.cl->SetDescriptorHeaps(1u, heaps);
        fx.cl->SetGraphicsRootSignature(rs.Get());
        fx.cl->SetGraphicsRootDescriptorTable(0u, srv_heap->GetGPUDescriptorHandleForHeapStart());
        fx.cl->SetPipelineState(pso.Get());
        fx.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Draw 2 instances! Instance 0 -> left triangle (B=64), Instance 1 -> right triangle (B=191)
        fx.cl->DrawInstanced(3u, 2u, 0u, 0u);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        fx.cl->ResourceBarrier(1u, &b);

        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = rt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0u;
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1u, kPitch};
        fx.cl->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        fx.cl->ResourceBarrier(1u, &b);
        fx.SubmitAndWait();

        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &mp)));
        std::memcpy(out_rgba.data(), mp, kTotal);
        rb->Unmap(0u, nullptr);
    };

    // Frame 1: unified texture = (200, 50, 10, 255)
    std::vector<uint8_t> frame1;
    draw_and_readback(frame1);
    const auto px_at = [](const std::vector<uint8_t>& img, uint32_t x, uint32_t y) -> const uint8_t* {
        return img.data() + static_cast<size_t>(y) * kPitch + static_cast<size_t>(x) * 4u;
    };
    const uint8_t* inst0_f1 = px_at(frame1, 8u, 56u);
    const uint8_t* inst1_f1 = px_at(frame1, 40u, 56u);
    const uint8_t* bg_f1 = px_at(frame1, 30u, 4u);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f1[0]) - 200) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f1[1]) - 50) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f1[2]) - 64) <= 2);
    OS_REQUIRE(inst0_f1[3] == 255u);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f1[0]) - 200) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f1[1]) - 50) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f1[2]) - 191) <= 2);
    OS_REQUIRE(inst1_f1[3] == 255u);
    OS_REQUIRE(bg_f1[0] == 0u && bg_f1[1] == 0u && bg_f1[2] == 0u && bg_f1[3] == 0u);

    // Frame 2: CPU mutates unified texture IN-PLACE to (40, 220, 10, 255) without NotifyVirtualRemap!
    fill_cpu_tex(40u, 220u, 10u, 255u);
    std::vector<uint8_t> frame2;
    draw_and_readback(frame2);
    const uint8_t* inst0_f2 = px_at(frame2, 8u, 56u);
    const uint8_t* inst1_f2 = px_at(frame2, 40u, 56u);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f2[0]) - 40) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f2[1]) - 220) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst0_f2[2]) - 64) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f2[0]) - 40) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f2[1]) - 220) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(inst1_f2[2]) - 191) <= 2);

    OS_REQUIRE(SUCCEEDED(fx.unified->UnregisterUnifiedTexture(unified_tex.Get())));
    (void)tex_bytes;
    VirtualFree(cpu_tex, 0u, MEM_RELEASE);

    std::printf("[%s] PASS Suite 2: Live Graphics VS(v0+v3 SV_InstanceID) + PS(Unified Tex auto-sync F1=%u,%u,%u/%u,%u,%u -> F2=%u,%u,%u/%u,%u,%u)\n",
                tag,
                inst0_f1[0], inst0_f1[1], inst0_f1[2], inst1_f1[0], inst1_f1[1], inst1_f1[2],
                inst0_f2[0], inst0_f2[1], inst0_f2[2], inst1_f2[0], inst1_f2[1], inst1_f2[2]);
}

// =============================================================================
// Suite 3: Live Compute Pipeline (Xbox DXBC + PM4R + 24-byte ISGN + RDNA) E2E
// =============================================================================
void TestComputeXboxDxbcLive(LiveDeviceFixture& fx, const char* tag) {
    const auto cs_dxbc = BuildComputeXboxDxbc24();
    OS_REQUIRE(IsXboxShaderContainerBlob(cs_dxbc.data(), cs_dxbc.size()));

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor.ShaderRegister = 0u;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 1u;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2u;
    rsd.pParameters = params;
    auto rs = fx.CreateRootSig(rsd);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = rs.Get();
    cpd.CS = {cs_dxbc.data(), cs_dxbc.size()};
    ComPtr<ID3D12PipelineState> pso;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateComputePipelineState(
        &cpd, IID_ID3D12PipelineState, pso.PutVoid())));
    OS_REQUIRE(pso.Get() != nullptr);

    constexpr uint32_t kN = 64u;
    constexpr uint64_t kBytes = kN * sizeof(uint32_t);
    auto in_up = fx.CreateBuffer(kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto in_gpu = fx.CreateBuffer(kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto out_gpu = fx.CreateBuffer(kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto rb = fx.CreateBuffer(kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    void* mp = nullptr;
    OS_REQUIRE(SUCCEEDED(in_up->Map(0u, nullptr, &mp)));
    auto* in_u32 = static_cast<uint32_t*>(mp);
    for (uint32_t i = 0u; i < kN; ++i) in_u32[i] = 100u + i * 3u;
    in_up->Unmap(0u, nullptr);

    fx.ResetList(pso.Get());
    fx.cl->CopyResource(in_gpu.Get(), in_up.Get());
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    fx.cl->ResourceBarrier(1u, &b);

    fx.cl->SetComputeRootSignature(rs.Get());
    fx.cl->SetComputeRootUnorderedAccessView(0u, in_gpu->GetGPUVirtualAddress());
    fx.cl->SetComputeRootUnorderedAccessView(1u, out_gpu->GetGPUVirtualAddress());
    fx.cl->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out_gpu.Get();
    fx.cl->ResourceBarrier(1u, &ub);
    b.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    fx.cl->ResourceBarrier(1u, &b);
    fx.cl->CopyResource(rb.Get(), out_gpu.Get());
    fx.SubmitAndWait();

    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &mp)));
    const auto* out_u32 = static_cast<const uint32_t*>(mp);
    for (uint32_t i = 0u; i < kN; ++i) {
        const uint32_t expected = ((100u + i * 3u) * 7u + 13u) ^ 0x5A5Au;
        OS_REQUIRE(out_u32[i] == expected);
    }
    rb->Unmap(0u, nullptr);

    std::printf("[%s] PASS Suite 3: Live Compute Xbox DXBC/PM4R/RDNA 64-lane Dispatch verified\n", tag);
}

// =============================================================================
// Suite 4: Mesh Pipeline WITHOUT RTS0 (Inferred Descriptor-Table Bindings #129)
// =============================================================================
void TestMeshNoRts0Live(LiveDeviceFixture& fx, const char* tag) {
    const auto ms_dxbc = BuildMeshNoRts0XboxDxbc24();
    const auto ps_dxbc = BuildPassthroughPsXboxDxbc24();

    // 1) Verify container parser infers descriptor-table bindings cleanly when RTS0 is absent
    XboxParsedShaderContainer parsed_ms{};
    OS_REQUIRE(ParseFullXboxShaderContainer(ms_dxbc, parsed_ms).ok());
    OS_REQUIRE(!parsed_ms.root_signature.present);
    OS_REQUIRE(parsed_ms.root_signature_inferred);
    OS_REQUIRE(parsed_ms.inferred_root_signature.present);
    OS_REQUIRE(!parsed_ms.sgpr_map.bindings.empty());
    bool found_inferred_uav_table = false;
    for (const auto& b : parsed_ms.sgpr_map.bindings) {
        if (b.kind == XboxUserSgprSlotKind::DescriptorTablePtr32) {
            for (const auto& r : b.descriptor_ranges) {
                if (r.range_type == XboxDescriptorRangeType::Uav && r.base_shader_register == 0u) {
                    found_inferred_uav_table = true;
                }
            }
        }
    }
    OS_REQUIRE(found_inferred_uav_table);

    // 2) On MeshShaderTier >= 1.0 GPUs: test BOTH omitted ROOT_SIGNATURE synthesis AND live DispatchMesh!
    if (fx.gpu_info.MeshShaderTier >= 10u) {
        // 2A: Omitted ROOT_SIGNATURE subobject -> DeviceX synthesizes host root sig from inferred table!
        MeshStreamNoRootSig no_rs_stream{};
        no_rs_stream.ms.inner = {ms_dxbc.data(), ms_dxbc.size()};
        no_rs_stream.ps.inner = {ps_dxbc.data(), ps_dxbc.size()};
        no_rs_stream.blend.inner.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        no_rs_stream.sample_mask.inner = 0xFFFFFFFFu;
        no_rs_stream.rasterizer.inner.FillMode = D3D12_FILL_MODE_SOLID;
        no_rs_stream.rasterizer.inner.CullMode = D3D12_CULL_MODE_NONE;
        no_rs_stream.depth_stencil.inner.DepthEnable = FALSE;
        no_rs_stream.rt_formats.inner.NumRenderTargets = 1u;
        no_rs_stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        no_rs_stream.sample_desc.inner = {1u, 0u};

        D3D12_PIPELINE_STATE_STREAM_DESC sdesc_nors{sizeof(no_rs_stream), &no_rs_stream};
        ComPtr<ID3D12PipelineState> synth_pso;
        OS_REQUIRE(SUCCEEDED(fx.device->CreatePipelineState(
            &sdesc_nors, IID_ID3D12PipelineState, synth_pso.PutVoid())));
        OS_REQUIRE(synth_pso.Get() != nullptr);

        // 2B: Live DispatchMesh with descriptor-table u0 binding (matching inferred table!)
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors = 1u;
        uav_range.BaseShaderRegister = 0u;
        uav_range.RegisterSpace = 0u;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1u;
        rp.DescriptorTable.pDescriptorRanges = &uav_range;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1u;
        rsd.pParameters = &rp;
        auto mesh_rs = fx.CreateRootSig(rsd);

        MeshStreamWithRootSig stream{};
        stream.root_sig.inner = mesh_rs.Get();
        stream.ms.inner = {ms_dxbc.data(), ms_dxbc.size()};
        stream.ps.inner = {ps_dxbc.data(), ps_dxbc.size()};
        stream.blend.inner.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.sample_mask.inner = 0xFFFFFFFFu;
        stream.rasterizer.inner.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rasterizer.inner.CullMode = D3D12_CULL_MODE_NONE;
        stream.depth_stencil.inner.DepthEnable = FALSE;
        stream.rt_formats.inner.NumRenderTargets = 1u;
        stream.rt_formats.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.sample_desc.inner = {1u, 0u};

        D3D12_PIPELINE_STATE_STREAM_DESC sdesc{sizeof(stream), &stream};
        ComPtr<ID3D12PipelineState> mesh_pso;
        OS_REQUIRE(SUCCEEDED(fx.device->CreatePipelineState(
            &sdesc, IID_ID3D12PipelineState, mesh_pso.PutVoid())));

        // Upload float 0.75f into Raw ByteAddressBuffer u0
        auto up_buf = fx.CreateBuffer(256u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto uav_buf = fx.CreateBuffer(256u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(up_buf->Map(0u, nullptr, &mp)));
        const float r_val = 0.75f;
        std::memcpy(mp, &r_val, sizeof(r_val));
        up_buf->Unmap(0u, nullptr);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1u;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;
        OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, heap.PutVoid())));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.Format = DXGI_FORMAT_R32_TYPELESS;
        uavd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavd.Buffer.FirstElement = 0u;
        uavd.Buffer.NumElements = 64u;
        uavd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        fx.device->CreateUnorderedAccessView(
            uav_buf.Get(), nullptr, &uavd, heap->GetCPUDescriptorHandleForHeapStart());

        constexpr uint32_t kW = 64u, kH = 64u, kPitch = kW * 4u, kTotal = kPitch * kH;
        auto rt = fx.CreateRenderTarget(kW, kH);
        D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
        rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_hd.NumDescriptors = 1u;
        ComPtr<ID3D12DescriptorHeap> rtv_heap;
        OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(&rtv_hd, IID_ID3D12DescriptorHeap, rtv_heap.PutVoid())));
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        fx.device->CreateRenderTargetView(rt.Get(), nullptr, rtv_h);
        auto rb = fx.CreateBuffer(kTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        fx.ResetList(mesh_pso.Get());
        fx.cl->CopyResource(uav_buf.Get(), up_buf.Get());
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {uav_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        fx.cl->ResourceBarrier(1u, &b);

        const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        fx.cl->ClearRenderTargetView(rtv_h, clr, 0u, nullptr);
        fx.cl->OMSetRenderTargets(1u, &rtv_h, FALSE, nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, static_cast<LONG>(kW), static_cast<LONG>(kH)};
        fx.cl->RSSetViewports(1u, &vp);
        fx.cl->RSSetScissorRects(1u, &sc);
        ID3D12DescriptorHeap* heaps[1] = {heap.Get()};
        fx.cl->SetDescriptorHeaps(1u, heaps);
        fx.cl->SetGraphicsRootSignature(mesh_rs.Get());
        fx.cl->SetGraphicsRootDescriptorTable(0u, heap->GetGPUDescriptorHandleForHeapStart());
        fx.cl->SetPipelineState(mesh_pso.Get());
        fx.cl->DispatchMesh(1u, 1u, 1u);

        b.Transition = {rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        fx.cl->ResourceBarrier(1u, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = rt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1u, kPitch};
        fx.cl->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
        fx.SubmitAndWait();

        OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &mp)));
        const auto* px = static_cast<const uint8_t*>(mp);
        // Expected RGBA = (0.75=191, 0.50=128, 0.25=64, 255)
        OS_REQUIRE(std::abs(static_cast<int>(px[0]) - 191) <= 2);
        OS_REQUIRE(std::abs(static_cast<int>(px[1]) - 128) <= 2);
        OS_REQUIRE(std::abs(static_cast<int>(px[2]) - 64) <= 2);
        OS_REQUIRE(px[3] == 255u);
        rb->Unmap(0u, nullptr);

        std::printf("[%s] PASS Suite 4: Mesh WITHOUT RTS0 inferred descriptor-table + omitted-RS synth + LIVE DispatchMesh (%u,%u,%u,%u)\n",
                    tag, px[0], px[1], px[2], px[3]);
    } else {
        // WARP (MeshTier == 0): validate lifted ms_6_5 DXIL & MeshEXT SPIR-V + inferred descriptor table
        XboxContainerLiftOptions xopts{};
        xopts.lift_options.emit_hlsl = true;
        xopts.lift_options.emit_spirv = true;
        xopts.lift_options.emit_mesh_spirv = true;
        xopts.lift_options.use_sm65 = true;
        xopts.lift_options.compile_dxil_with_dxc = true;
        XboxContainerLiftResult res{};
        OS_REQUIRE(LiftXboxShaderContainer(ms_dxbc.data(), ms_dxbc.size(), res, xopts).ok());
        OS_REQUIRE(res.root_signature_inferred);
        OS_REQUIRE(res.lift.hlsl_target == "ms_6_5");
        OS_REQUIRE(res.lift.pc_dxil.size() >= 32u);
        OS_REQUIRE(ValidateSpirvBinary(res.lift.spirv_words).ok());
        std::printf("[%s] PASS Suite 4: Mesh WITHOUT RTS0 inferred descriptor-table + validated ms_6_5 DXIL (%zu B) & MeshEXT SPIR-V\n",
                    tag, res.lift.pc_dxil.size());
    }
}

// =============================================================================
// Suite 5: RayQuery Compute Pipeline LIVE E2E via DeviceX (BVH64 + BVH32)
// =============================================================================
void TestRayQueryXboxContainerLive(LiveDeviceFixture& fx, const char* tag) {
    // 1) Build 2-instance 64-bit BVH4 scene (TLAS + 2 BLAS)
    constexpr uint64_t kBlas0Va = 0x0000010000100000ull;
    constexpr uint64_t kBlas1Va = 0x0000020000200000ull;
    constexpr uint64_t kTlasVa  = 0x0000030000300000ull;

    std::vector<RtTriangle> tris0(1);
    tris0[0].v0 = {-1.0f, -1.0f, 2.0f};
    tris0[0].v1 = { 1.0f, -1.0f, 2.0f};
    tris0[0].v2 = { 0.0f,  1.0f, 2.0f};

    std::vector<RtTriangle> tris1(1);
    tris1[0].v0 = {-1.0f, -1.0f, 0.0f};
    tris1[0].v1 = { 1.0f, -1.0f, 0.0f};
    tris1[0].v2 = { 0.0f,  1.0f, 0.0f};

    BvhAccelerationStructure bvh0, bvh1;
    OS_REQUIRE(bvh0.Build(tris0).ok());
    OS_REQUIRE(bvh1.Build(tris1).ok());

    Rdna2BvhEncodeOptions enc_opts{};
    enc_opts.use_box16 = false;
    enc_opts.geometry_flags = 1u;  // opaque

    std::vector<uint8_t> blas0_buf, blas1_buf, tlas_buf;
    Rdna2BvhHeader blas0_hdr{}, blas1_hdr{};
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(bvh0, enc_opts, blas0_buf, &blas0_hdr).ok());
    OS_REQUIRE(EncodeBvh2ToRdna2Bvh4(bvh1, enc_opts, blas1_buf, &blas1_hdr).ok());

    std::vector<Rdna2BvhTlasInstanceInput> inst_vec(2);
    inst_vec[0].blas_base_va = kBlas0Va;
    inst_vec[0].blas_root_ptr = blas0_hdr.root_node_ptr;
    inst_vec[0].blas_local_aabb = blas0_hdr.root_aabb;
    inst_vec[0].instance_id = 101u;
    inst_vec[0].instance_mask = 0xFFu;

    inst_vec[1].blas_base_va = kBlas1Va;
    inst_vec[1].blas_root_ptr = blas1_hdr.root_node_ptr;
    inst_vec[1].blas_local_aabb = blas1_hdr.root_aabb;
    inst_vec[1].instance_id = 202u;
    inst_vec[1].instance_mask = 0xFFu;
    // Translate BLAS 1 to (3, 0, 5)
    inst_vec[1].object_to_world[0][3] = 3.0f;
    inst_vec[1].object_to_world[2][3] = 5.0f;
    OS_REQUIRE(BuildRdna2Bvh4Tlas(inst_vec, {}, enc_opts, tlas_buf).ok());

    Rdna2Bvh64MultiWindow scene;
    OS_REQUIRE(scene.RegisterWindow(kBlas0Va, blas0_buf).ok());
    OS_REQUIRE(scene.RegisterWindow(kBlas1Va, blas1_buf).ok());
    OS_REQUIRE(scene.RegisterWindow(kTlasVa, tlas_buf).ok());

    std::vector<Rdna2GpuBvhWindowDesc> win_table;
    std::vector<uint32_t> bvh_dwords;
    uint32_t tlas_win_idx = 0u;
    OS_REQUIRE(scene.PackForGpuCompute(kTlasVa, win_table, bvh_dwords, tlas_win_idx).ok());

    // 4 test rays: 2 hits (inst 101 @ t=2, inst 202 @ t=5) + 2 misses
    std::vector<Rdna2RayQueryInput> rays(4);
    rays[0] = {{0.0f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u};
    rays[1] = {{3.0f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u};
    rays[2] = {{10.0f, 10.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u};
    rays[3] = {{0.0f, 0.0f, 0.0f}, 0.001f, {0.0f, 0.0f, -1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u};

    std::vector<Rdna2RayQueryHit> cpu_hits(rays.size());
    for (size_t i = 0; i < rays.size(); ++i) {
        OS_REQUIRE(scene.TraceRay64(kTlasVa, rays[i], cpu_hits[i]).ok());
    }
    OS_REQUIRE(cpu_hits[0].committed_status == kRdna2CommittedTriangleHit && cpu_hits[0].instance_id == 101u);
    OS_REQUIRE(cpu_hits[1].committed_status == kRdna2CommittedTriangleHit && cpu_hits[1].instance_id == 202u);
    OS_REQUIRE(cpu_hits[2].committed_status == kRdna2CommittedNothing);
    OS_REQUIRE(cpu_hits[3].committed_status == kRdna2CommittedNothing);

    // 2) Build Xbox DXBC/PM4/RDNA2 container containing real image_bvh64_intersect_ray (0xE7)
    //    and pass DIRECTLY to DeviceX::CreateComputePipelineState!
    const auto rq_dxbc64 = BuildRayQueryXboxDxbc24(/*use_bvh64=*/true);
    const auto rq_dxbc32 = BuildRayQueryXboxDxbc24(/*use_bvh64=*/false);
    XboxParsedShaderContainer rq_parsed{};
    OS_REQUIRE(ParseFullXboxShaderContainer(rq_dxbc64, rq_parsed).ok());
    OS_REQUIRE(rq_parsed.bvh_info.detected && rq_parsed.bvh_info.is_64bit);

    // Root signature matching rdna2_rayquery_bridge Compute BVH4 fallback ABI (b0, t0, t1, t2, u0)
    D3D12_ROOT_PARAMETER rps[5]{};
    rps[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rps[0].Descriptor.ShaderRegister = 0u;
    rps[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rps[1].Descriptor.ShaderRegister = 0u;
    rps[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rps[2].Descriptor.ShaderRegister = 1u;
    rps[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rps[3].Descriptor.ShaderRegister = 2u;
    rps[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rps[4].Descriptor.ShaderRegister = 0u;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5u;
    rsd.pParameters = rps;
    auto rs = fx.CreateRootSig(rsd);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
    cpd.pRootSignature = rs.Get();
    cpd.CS = {rq_dxbc64.data(), rq_dxbc64.size()};
    ComPtr<ID3D12PipelineState> rq_pso;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateComputePipelineState(
        &cpd, IID_ID3D12PipelineState, rq_pso.PutVoid())));
    OS_REQUIRE(rq_pso.Get() != nullptr);

    // Also verify image_bvh_intersect_ray (0xE6) container creates a valid PSO live!
    cpd.CS = {rq_dxbc32.data(), rq_dxbc32.size()};
    ComPtr<ID3D12PipelineState> rq_pso32;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateComputePipelineState(
        &cpd, IID_ID3D12PipelineState, rq_pso32.PutVoid())));
    OS_REQUIRE(rq_pso32.Get() != nullptr);

    // Upload CBV (b0), WindowTable (t0), BvhData (t1), RayInputs (t2), and allocate RayOutputs (u0)
    struct BvhDispatchParams {
        uint32_t ray_count;
        uint32_t tlas_window_index;
        uint32_t window_count;
        uint32_t default_flags;
    };
    BvhDispatchParams cb_data{
        static_cast<uint32_t>(rays.size()),
        tlas_win_idx,
        static_cast<uint32_t>(win_table.size()),
        kRdna2RayFlagNone};

    const uint64_t win_bytes = win_table.size() * sizeof(Rdna2GpuBvhWindowDesc);
    const uint64_t bvh_bytes = bvh_dwords.size() * sizeof(uint32_t);
    const uint64_t ray_bytes = rays.size() * sizeof(Rdna2RayQueryInput);
    const uint64_t out_bytes = rays.size() * sizeof(Rdna2RayQueryHit);

    auto cb_buf = fx.CreateBuffer(256u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto win_up = fx.CreateBuffer(AlignUp64(win_bytes, 256u), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto bvh_up = fx.CreateBuffer(AlignUp64(bvh_bytes, 256u), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto ray_up = fx.CreateBuffer(AlignUp64(ray_bytes, 256u), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto out_gpu = fx.CreateBuffer(AlignUp64(out_bytes, 256u), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto rb = fx.CreateBuffer(AlignUp64(out_bytes, 256u), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    auto upload = [](ID3D12Resource* buf, const void* src, size_t n) {
        void* p = nullptr;
        OS_REQUIRE(SUCCEEDED(buf->Map(0u, nullptr, &p)));
        std::memcpy(p, src, n);
        buf->Unmap(0u, nullptr);
    };
    upload(cb_buf.Get(), &cb_data, sizeof(cb_data));
    upload(win_up.Get(), win_table.data(), static_cast<size_t>(win_bytes));
    upload(bvh_up.Get(), bvh_dwords.data(), static_cast<size_t>(bvh_bytes));
    upload(ray_up.Get(), rays.data(), static_cast<size_t>(ray_bytes));

    fx.ResetList(rq_pso.Get());
    fx.cl->SetComputeRootSignature(rs.Get());
    fx.cl->SetComputeRootConstantBufferView(0u, cb_buf->GetGPUVirtualAddress());
    fx.cl->SetComputeRootShaderResourceView(1u, win_up->GetGPUVirtualAddress());
    fx.cl->SetComputeRootShaderResourceView(2u, bvh_up->GetGPUVirtualAddress());
    fx.cl->SetComputeRootShaderResourceView(3u, ray_up->GetGPUVirtualAddress());
    fx.cl->SetComputeRootUnorderedAccessView(4u, out_gpu->GetGPUVirtualAddress());
    fx.cl->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = out_gpu.Get();
    fx.cl->ResourceBarrier(1u, &ub);
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    fx.cl->ResourceBarrier(1u, &tb);
    fx.cl->CopyBufferRegion(rb.Get(), 0u, out_gpu.Get(), 0u, out_bytes);
    fx.SubmitAndWait();

    void* mp = nullptr;
    OS_REQUIRE(SUCCEEDED(rb->Map(0u, nullptr, &mp)));
    const auto* gpu_hits = static_cast<const Rdna2RayQueryHit*>(mp);
    for (size_t i = 0; i < rays.size(); ++i) {
        OS_REQUIRE(gpu_hits[i].committed_status == cpu_hits[i].committed_status);
        if (cpu_hits[i].committed_status == kRdna2CommittedTriangleHit) {
            OS_REQUIRE(std::fabs(gpu_hits[i].t - cpu_hits[i].t) < 1e-3f);
            OS_REQUIRE(std::fabs(gpu_hits[i].bary_u - cpu_hits[i].bary_u) < 1e-3f);
            OS_REQUIRE(std::fabs(gpu_hits[i].bary_v - cpu_hits[i].bary_v) < 1e-3f);
            OS_REQUIRE(gpu_hits[i].primitive_index == cpu_hits[i].primitive_index);
            OS_REQUIRE(gpu_hits[i].instance_id == cpu_hits[i].instance_id);
        }
    }
    rb->Unmap(0u, nullptr);

    std::printf("[%s] PASS Suite 5: RayQuery image_bvh64_intersect_ray Xbox DXBC LIVE via DeviceX (hit0 inst=%u t=%.2f, hit1 inst=%u t=%.2f, 2 misses == CPU)\n",
                tag, cpu_hits[0].instance_id, cpu_hits[0].t, cpu_hits[1].instance_id, cpu_hits[1].t);
}

// =============================================================================
// Suite 6: Title-Window (HWND) Console Screen Adoption & PresentX (xcl 9e759de)
// =============================================================================
LRESULT CALLBACK S13TitleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateHiddenTitleConsoleWindow(const wchar_t* title, int w, int h) {
    static const wchar_t* kCls = L"OpenScarlettS13TitleConsoleWnd";
    static bool s_reg = false;
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &S13TitleWndProc;
        wc.hInstance = hinst;
        wc.lpszClassName = kCls;
        OS_REQUIRE(RegisterClassExW(&wc) != 0);
        s_reg = true;
    }
    // Hidden/offscreen title window (no WS_VISIBLE) so it runs cleanly on headless CI too!
    HWND hwnd = CreateWindowExW(
        0u, kCls, title, WS_OVERLAPPEDWINDOW,
        64, 64, w, h, nullptr, nullptr, hinst, nullptr);
    OS_REQUIRE(hwnd != nullptr);
    return hwnd;
}

void TestTitleWindowPresentXLive(LiveDeviceFixture& fx, const char* tag) {
    SetEnvironmentVariableA("OPENSCARLETT_PRESENTX_HEADLESS", nullptr);

    // 1) Create a title-owned hidden/offscreen Win32 HWND AFTER device & queue creation
    //    and verify PresentX auto-adopts it (xcl 9e759de Minecraft pattern)!
    HWND title_hwnd = CreateHiddenTitleConsoleWindow(L"OpenScarlett S13 Title Console Screen", 256, 256);
    RECT cr{};
    OS_REQUIRE(GetClientRect(title_hwnd, &cr) != FALSE);
    const uint32_t expected_cw = static_cast<uint32_t>(cr.right - cr.left);
    const uint32_t expected_ch = static_cast<uint32_t>(cr.bottom - cr.top);
    OS_REQUIRE(expected_cw >= 64u && expected_ch >= 64u);

    constexpr uint32_t kW = 128u, kH = 128u;
    auto frame_rt = fx.CreateRenderTarget(kW, kH);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = 1u;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    OS_REQUIRE(SUCCEEDED(fx.device->CreateDescriptorHeap(&rtv_hd, IID_ID3D12DescriptorHeap, rtv_heap.PutVoid())));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv_h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    fx.device->CreateRenderTargetView(frame_rt.Get(), nullptr, rtv_h);

    // Render known non-black color (0.25, 0.50, 0.75, 1.0) into frame_rt
    fx.ResetList();
    const float col1[4] = {0.25f, 0.50f, 0.75f, 1.0f};
    fx.cl->ClearRenderTargetView(rtv_h, col1, 0u, nullptr);
    D3D12_RESOURCE_BARRIER to_common{};
    to_common.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_common.Transition = {frame_rt.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON};
    fx.cl->ResourceBarrier(1u, &to_common);
    fx.SubmitAndWait();

    OS_REQUIRE(SUCCEEDED(fx.present_capture->SetReadbackCaptureEnabled(TRUE)));
    ID3D12Resource* planes[1] = {frame_rt.Get()};
    D3D12XBOX_PRESENT_PLANE_PARAMETERS plane{};
    plane.Token = 1u;
    plane.ResourceCount = 1u;
    plane.ppResources = planes;
    plane.ResourceState = D3D12_RESOURCE_STATE_COMMON;
    D3D12XBOX_PRESENT_PARAMETERS pparams{};
    pparams.Flags = D3D12XBOX_PRESENT_FLAG_IMMEDIATE;
    // Deliberately leave pparams.OutputWindow = nullptr to test auto-adoption of title_hwnd!
    OS_REQUIRE(SUCCEEDED(fx.queue->PresentX(1u, &plane, &pparams)));

    D3D12XBOX_PRESENT_WINDOW_STATE ws{};
    OS_REQUIRE(SUCCEEDED(fx.present_window->GetPresentWindowState(&ws)));
    OS_REQUIRE(ws.present_count >= 1u);
    OS_REQUIRE(ws.width == expected_cw && ws.height == expected_ch);

    uint64_t hash1 = 0ull, pcnt1 = 0ull;
    uint32_t pw = 0u, ph = 0u;
    OS_REQUIRE(SUCCEEDED(fx.present_capture->GetLastPresentStats(&hash1, &pw, &ph, &pcnt1)));
    OS_REQUIRE(hash1 != 0ull && pw == kW && ph == kH && pcnt1 >= 1u);

    std::vector<uint8_t> captured(static_cast<size_t>(kW) * kH * 4u, 0u);
    uint32_t copied = 0u;
    OS_REQUIRE(SUCCEEDED(fx.present_capture->CopyLastPresentPixels(
        captured.data(), static_cast<uint32_t>(captured.size()), &copied)));
    OS_REQUIRE(copied == captured.size());
    OS_REQUIRE(std::abs(static_cast<int>(captured[0]) - 64) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(captured[1]) - 128) <= 2);
    OS_REQUIRE(std::abs(static_cast<int>(captured[2]) - 191) <= 2);
    OS_REQUIRE(captured[3] == 255u);

    // 2) Also test explicit OutputWindow binding on a second title HWND
    HWND title_hwnd2 = CreateHiddenTitleConsoleWindow(L"OpenScarlett S13 Explicit Screen", 320, 200);
    pparams.OutputWindow = title_hwnd2;
    OS_REQUIRE(SUCCEEDED(fx.present_capture->SetOutputWindow(title_hwnd2)));
    OS_REQUIRE(SUCCEEDED(fx.queue->PresentX(1u, &plane, &pparams)));
    OS_REQUIRE(SUCCEEDED(fx.present_window->GetPresentWindowState(&ws)));
    OS_REQUIRE(ws.present_count >= 2u);

    OS_REQUIRE(SUCCEEDED(fx.present_capture->SetOutputWindow(nullptr)));
    DestroyWindow(title_hwnd2);
    DestroyWindow(title_hwnd);

    std::printf("[%s] PASS Suite 6: Title-Window HWND auto-adoption (%ux%u) + explicit bind PresentX capture (hash=0x%016llX, rgba=%u,%u,%u,%u)\n",
                tag, expected_cw, expected_ch,
                static_cast<unsigned long long>(hash1),
                captured[0], captured[1], captured[2], captured[3]);
}

// =============================================================================
// Suite 7: Unified-Memory Texture Upload Coherency + Unified Heap Buffer Readback
// =============================================================================
void TestUnifiedMemoryTextureAndHeapBufferLive(LiveDeviceFixture& fx, const char* tag) {
    // 1) Unified-memory 2D texture (standard non-write-watch VirtualAlloc -> hash-tracked re-sync!)
    constexpr uint32_t kW = 16u, kH = 16u;
    constexpr size_t kTexBytes = static_cast<size_t>(kW) * kH * 4u;
    void* cpu_tex = VirtualAlloc(nullptr, 65536u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    OS_REQUIRE(cpu_tex != nullptr);

    auto write_checker = [&](uint32_t c0, uint32_t c1) {
        auto* p = static_cast<uint32_t*>(cpu_tex);
        for (uint32_t y = 0u; y < kH; ++y) {
            for (uint32_t x = 0u; x < kW; ++x) {
                p[y * kW + x] = (((x ^ y) & 1u) == 0u) ? c0 : c1;
            }
        }
    };
    write_checker(0xFF112233u, 0xFF445566u);

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = 1u;
    td.MipLevels = 1u;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = {1u, 0u};

    ComPtr<ID3D12Resource> utex;
    OS_REQUIRE(SUCCEEDED(fx.device->CreatePlacedResourceX(
        reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(cpu_tex),
        &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_ID3D12Resource, utex.PutVoid())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT nrows = 0u;
    UINT64 rbytes = 0ull, tbytes = 0ull;
    fx.device->GetCopyableFootprints(&td, 0u, 1u, 0ull, &fp, &nrows, &rbytes, &tbytes);
    auto tex_rb = fx.CreateBuffer(AlignUp64(tbytes, 256u), D3D12_HEAP_TYPE_READBACK,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    auto verify_tex_gpu = [&](uint32_t expect_c0, uint32_t expect_c1) {
        fx.ResetList();
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {utex.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
        fx.cl->ResourceBarrier(1u, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = utex.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = tex_rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        fx.cl->CopyTextureRegion(&dst, 0u, 0u, 0u, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        fx.cl->ResourceBarrier(1u, &b);
        fx.SubmitAndWait();

        void* mp = nullptr;
        OS_REQUIRE(SUCCEEDED(tex_rb->Map(0u, nullptr, &mp)));
        const auto* base = static_cast<const uint8_t*>(mp);
        for (uint32_t y = 0u; y < kH; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0u; x < kW; ++x) {
                OS_REQUIRE(row[x] == ((((x ^ y) & 1u) == 0u) ? expect_c0 : expect_c1));
            }
        }
        tex_rb->Unmap(0u, nullptr);
    };

    verify_tex_gpu(0xFF112233u, 0xFF445566u);
    // Second in-place CPU write (non-write-watch memory -> automatic content-hash dirty detection!)
    write_checker(0xFF99AABB, 0xFFCCDDEE);
    verify_tex_gpu(0xFF99AABB, 0xFFCCDDEE);

    D3D12XBOX_UNIFIED_TEXTURE_STATS ustats{};
    OS_REQUIRE(SUCCEEDED(fx.unified->GetUnifiedTextureStats(&ustats)));
    OS_REQUIRE(ustats.TotalFlushes >= 2u);
    OS_REQUIRE(ustats.TotalBytesUploaded >= kTexBytes * 2u);

    // 2) Unified Heap Buffer via OpenExistingHeapFromAddress + CreatePlacedResource (CPU->GPU & GPU->CPU!)
    constexpr size_t kBufBytes = 65536u;
    void* heap_mem = VirtualAlloc(nullptr, kBufBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    OS_REQUIRE(heap_mem != nullptr);
    auto* heap_u32 = static_cast<uint32_t*>(heap_mem);
    heap_u32[0] = 0x12345678u;
    heap_u32[1] = 0x9ABCDEF0u;

    ComPtr<ID3D12Heap> addr_heap;
    const HRESULT heap_hr = fx.device->OpenExistingHeapFromAddress(
        heap_mem, IID_ID3D12Heap, addr_heap.PutVoid());

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096u;
    bd.Height = 1u;
    bd.DepthOrArraySize = 1u;
    bd.MipLevels = 1u;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc = {1u, 0u};
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> unified_buf;
    bool used_open_existing_heap = false;
    if (SUCCEEDED(heap_hr) && addr_heap.Get() != nullptr) {
        if (SUCCEEDED(fx.device->CreatePlacedResource(
                addr_heap.Get(), 0ull, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_ID3D12Resource, unified_buf.PutVoid()))) {
            used_open_existing_heap = true;
        }
    }
    if (!unified_buf) {
        OS_REQUIRE(SUCCEEDED(fx.device->CreatePlacedResourceX(
            reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(heap_mem),
            &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_ID3D12Resource, unified_buf.PutVoid())));
    }

    auto buf_rb = fx.CreateBuffer(256u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto buf_up = fx.CreateBuffer(256u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

    // CPU -> GPU readback check #1
    fx.ResetList();
    fx.cl->CopyBufferRegion(buf_rb.Get(), 0u, unified_buf.Get(), 0u, 8u);
    fx.SubmitAndWait();
    void* mp = nullptr;
    OS_REQUIRE(SUCCEEDED(buf_rb->Map(0u, nullptr, &mp)));
    OS_REQUIRE(static_cast<const uint32_t*>(mp)[0] == 0x12345678u);
    OS_REQUIRE(static_cast<const uint32_t*>(mp)[1] == 0x9ABCDEF0u);
    buf_rb->Unmap(0u, nullptr);

    // CPU mutation -> GPU readback check #2
    heap_u32[0] = 0xCAFEBABEu;
    heap_u32[1] = 0xFEEDFACEu;
    fx.ResetList();
    fx.cl->CopyBufferRegion(buf_rb.Get(), 0u, unified_buf.Get(), 0u, 8u);
    fx.SubmitAndWait();
    OS_REQUIRE(SUCCEEDED(buf_rb->Map(0u, nullptr, &mp)));
    OS_REQUIRE(static_cast<const uint32_t*>(mp)[0] == 0xCAFEBABEu);
    OS_REQUIRE(static_cast<const uint32_t*>(mp)[1] == 0xFEEDFACEu);
    buf_rb->Unmap(0u, nullptr);

    // GPU -> CPU writeback check #3
    OS_REQUIRE(SUCCEEDED(buf_up->Map(0u, nullptr, &mp)));
    static_cast<uint32_t*>(mp)[0] = 0x0D15EA5Eu;
    static_cast<uint32_t*>(mp)[1] = 0xB16B00B5u;
    buf_up->Unmap(0u, nullptr);

    fx.ResetList();
    D3D12_RESOURCE_BARRIER to_dst{};
    to_dst.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_dst.Transition = {unified_buf.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
    fx.cl->ResourceBarrier(1u, &to_dst);
    fx.cl->CopyBufferRegion(unified_buf.Get(), 0u, buf_up.Get(), 0u, 8u);
    to_dst.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_dst.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    fx.cl->ResourceBarrier(1u, &to_dst);
    fx.SubmitAndWait();
    fx.queue->KickoffX();

    OS_REQUIRE(heap_u32[0] == 0x0D15EA5Eu);
    OS_REQUIRE(heap_u32[1] == 0xB16B00B5u);

    OS_REQUIRE(SUCCEEDED(fx.unified->UnregisterUnifiedTexture(utex.Get())));
    OS_REQUIRE(SUCCEEDED(fx.unified->UnregisterUnifiedTexture(unified_buf.Get())));
    unified_buf.Reset();
    addr_heap.Reset();
    utex.Reset();
    VirtualFree(heap_mem, 0u, MEM_RELEASE);
    VirtualFree(cpu_tex, 0u, MEM_RELEASE);

    std::printf("[%s] PASS Suite 7: Unified Texture auto-sync (flushes=%llu, bytes=%llu) + Unified Heap Buffer CPU<->GPU (%s)\n",
                tag,
                static_cast<unsigned long long>(ustats.TotalFlushes),
                static_cast<unsigned long long>(ustats.TotalBytesUploaded),
                used_open_existing_heap ? "OpenExistingHeapFromAddress" : "CreatePlacedResourceX");
}

void RunAllLiveSuites(bool prefer_warp, const char* tag) {
    std::printf("\n=== Sprint 13 Live Xbox Shader E2E Suite [%s] ===\n", tag);
    LiveDeviceFixture fx;
    fx.Init(prefer_warp);

    TestGraphicsVsV0V3AndUnifiedTexLive(fx, tag);
    TestComputeXboxDxbcLive(fx, tag);
    TestMeshNoRts0Live(fx, tag);
    TestRayQueryXboxContainerLive(fx, tag);
    TestTitleWindowPresentXLive(fx, tag);
    TestUnifiedMemoryTextureAndHeapBufferLive(fx, tag);
}

}  // namespace

int main() {
    // Isolate shader cache strictly under %TEMP% (E:\ under build_wt.cmd)
    std::filesystem::path cache_dir;
    {
        char env_buf[1024]{};
        const DWORD len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
        if (len > 0u && len < sizeof(env_buf)) {
            cache_dir = std::filesystem::path(std::string(env_buf, len)) / "openscarlett_s13_live_cache";
        } else {
            cache_dir = std::filesystem::temp_directory_path() / "openscarlett_s13_live_cache";
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", cache_dir.string().c_str());
    SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", "0");

    TestContainerParsingAndBounds();

    D3D12AdapterInfo hw_info{};
    const bool has_hw = HasHardwareGpu(hw_info);
    if (has_hw) {
        std::printf("Detected hardware D3D12 GPU: %s (SM=0x%02X, MeshTier=%u, RTTier=%u)\n",
                    hw_info.description.c_str(),
                    hw_info.highest_shader_model,
                    hw_info.mesh_shader_tier,
                    hw_info.raytracing_tier);
        RunAllLiveSuites(/*prefer_warp=*/false, "HW_GPU");
    } else {
        std::printf("No hardware D3D12 GPU detected; running WARP suite only.\n");
    }

    RunAllLiveSuites(/*prefer_warp=*/true, "WARP");

    SetEnvironmentVariableA("OPENSCARLETT_ASYNC_PSO", nullptr);
    SetEnvironmentVariableA("OPENSCARLETT_SHADER_CACHE", nullptr);
    std::filesystem::remove_all(cache_dir, ec);

    std::printf("\nALL Sprint 13 Live Xbox Shader E2E tests PASSED!\n");
    return 0;
}
