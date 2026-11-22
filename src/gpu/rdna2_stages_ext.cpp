#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "openscarlett/gpu/rdna2_stages_ext.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/vulkan_backend.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#if defined(_MSC_VER)
#define OS_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_UUIDOF(T) IID_##T
#endif
#endif

namespace openscarlett {

namespace {

constexpr uint64_t AlignUpU64(uint64_t v, uint64_t a) noexcept {
    return (a <= 1ull) ? v : ((v + a - 1ull) & ~(a - 1ull));
}

uint32_t Popcount32(uint32_t v) noexcept {
    uint32_t c = 0u;
    while (v != 0u) {
        v &= (v - 1u);
        ++c;
    }
    return c;
}

uint32_t Wqm32(uint32_t m) noexcept {
    uint32_t q = m | ((m & 0x55555555u) << 1u) | ((m & 0xAAAAAAAAu) >> 1u);
    q = q | ((q & 0x33333333u) << 2u) | ((q & 0xCCCCCCCCu) >> 2u);
    return q;
}

uint64_t Fnv1a64Update(uint64_t h, const void* data, size_t len) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

void EmitSpv(std::vector<uint32_t>& out, uint16_t op, std::initializer_list<uint32_t> ops) {
    const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
    out.push_back((wc << 16) | static_cast<uint32_t>(op));
    out.insert(out.end(), ops.begin(), ops.end());
}

void EmitSpvVec(std::vector<uint32_t>& out, uint16_t op, const std::vector<uint32_t>& ops) {
    const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
    out.push_back((wc << 16) | static_cast<uint32_t>(op));
    out.insert(out.end(), ops.begin(), ops.end());
}

void AppendSpvExtString(std::vector<uint32_t>& out, const char* str) {
    const size_t len = std::strlen(str) + 1u;
    const size_t words = (len + 3u) / 4u;
    for (size_t w = 0u; w < words; ++w) {
        uint32_t word = 0u;
        for (size_t b = 0u; b < 4u; ++b) {
            const size_t idx = w * 4u + b;
            if (idx < len) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(str[idx])) << (b * 8u);
            }
        }
        out.push_back(word);
    }
}

// SPIR-V 1.3 / 1.6 opcodes used by HS/DS/GS, VS/GS/MS cull-distance, and Wave64-on-Wave32 emitters.
enum SpvExtOp : uint16_t {
    SpvOpExtension = 10,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeArray = 28,
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstant = 43,
    SpvOpConstantComposite = 44,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpConvertUToF = 112,
    SpvOpBitcast = 124,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpIMul = 132,
    SpvOpFMul = 133,
    SpvOpUMod = 137,
    SpvOpVectorTimesScalar = 142,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpINotEqual = 171,
    SpvOpUGreaterThanEqual = 174,
    SpvOpULessThan = 176,
    SpvOpShiftRightLogical = 194,
    SpvOpShiftLeftLogical = 196,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199,
    SpvOpBitCount = 205,
    SpvOpEmitVertex = 218,
    SpvOpEndPrimitive = 219,
    SpvOpControlBarrier = 224,
    SpvOpAtomicAnd = 240,
    SpvOpAtomicOr = 241,
    SpvOpSelectionMerge = 247,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpReturn = 253,
    SpvOpSetMeshOutputsEXT = 5295,
};

#if defined(_WIN32)

template <typename T>
class LocalComPtr {
public:
    LocalComPtr() = default;
    ~LocalComPtr() { Reset(); }

    LocalComPtr(const LocalComPtr&) = delete;
    LocalComPtr& operator=(const LocalComPtr&) = delete;

    LocalComPtr(LocalComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    LocalComPtr& operator=(LocalComPtr&& o) noexcept {
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

    void Attach(T* p) noexcept {
        Reset();
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

bool IsSoftwareOrWarpDesc(const DXGI_ADAPTER_DESC1& desc) noexcept {
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
           (desc.VendorId == 0x1414u &&
            (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0));
}

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12SerializeRootSignature =
    HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

Status CreateD3D12DeviceLocal(
    bool prefer_warp,
    LocalComPtr<ID3D12Device>& out_dev,
    D3D12AdapterInfo& out_info) {
    out_dev.Reset();
    out_info = D3D12AdapterInfo{};

    auto create_fn = reinterpret_cast<PfnD3D12CreateDevice>(ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!create_fn) {
        return Status::Error(StatusCode::DeviceError, "Failed to resolve host D3D12CreateDevice");
    }

    LocalComPtr<IDXGIFactory6> factory6;
    LocalComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory2(0, OS_UUIDOF(IDXGIFactory6), factory6.PutVoid())) && factory6) {
        factory6->QueryInterface(OS_UUIDOF(IDXGIFactory4), factory4.PutVoid());
    } else if (FAILED(CreateDXGIFactory1(OS_UUIDOF(IDXGIFactory4), factory4.PutVoid())) || !factory4) {
        return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory failed");
    }

    LocalComPtr<IDXGIAdapter1> chosen;
    if (prefer_warp) {
        if (FAILED(factory4->EnumWarpAdapter(OS_UUIDOF(IDXGIAdapter1), chosen.PutVoid())) || !chosen) {
            return Status::Error(StatusCode::DeviceError, "EnumWarpAdapter failed");
        }
        HRESULT hr = create_fn(chosen.Get(), D3D_FEATURE_LEVEL_12_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid());
        if (FAILED(hr) || !out_dev) {
            hr = create_fn(chosen.Get(), D3D_FEATURE_LEVEL_11_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid());
        }
        if (FAILED(hr) || !out_dev) {
            return Status::Error(StatusCode::DeviceError, "D3D12CreateDevice(WARP) failed");
        }
    } else {
        for (UINT idx = 0;; ++idx) {
            LocalComPtr<IDXGIAdapter1> cand;
            HRESULT ehr = factory6
                              ? factory6->EnumAdapterByGpuPreference(
                                    idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                    OS_UUIDOF(IDXGIAdapter1), cand.PutVoid())
                              : factory4->EnumAdapters1(idx, cand.Put());
            if (ehr == DXGI_ERROR_NOT_FOUND || FAILED(ehr) || !cand) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(cand->GetDesc1(&desc))) continue;
            if (IsSoftwareOrWarpDesc(desc)) continue;

            if (SUCCEEDED(create_fn(cand.Get(), D3D_FEATURE_LEVEL_12_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid())) &&
                out_dev) {
                chosen = std::move(cand);
                break;
            }
        }
        if (!out_dev) {
            return Status::Error(StatusCode::DeviceError, "No hardware D3D12 GPU adapter available");
        }
    }

    QueryD3D12DeviceAdapterInfo(out_dev.Get(), chosen.Get(), out_info);
    if (prefer_warp) out_info.is_warp = true;
    return Status::Ok();
}

Status CreateD3D12BufferLocal(
    ID3D12Device* dev,
    D3D12_HEAP_TYPE heap,
    uint64_t bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES state,
    LocalComPtr<ID3D12Resource>& out_res) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256ull);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    if (FAILED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
            OS_UUIDOF(ID3D12Resource), out_res.PutVoid())) || !out_res) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource failed");
    }
    return Status::Ok();
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// HS / DS / GS Helpers
// ---------------------------------------------------------------------------

const char* HlslDomainName(TessDomain d) noexcept {
    switch (d) {
        case TessDomain::Quad:    return "quad";
        case TessDomain::Isoline: return "isoline";
        case TessDomain::Tri:
        default:                  return "tri";
    }
}

const char* HlslPartitioningName(TessPartitioning p) noexcept {
    switch (p) {
        case TessPartitioning::Integer:        return "integer";
        case TessPartitioning::FractionalEven: return "fractional_even";
        case TessPartitioning::Pow2:           return "pow2";
        case TessPartitioning::FractionalOdd:
        default:                               return "fractional_odd";
    }
}

const char* HlslOutputTopologyName(TessOutputTopology t) noexcept {
    switch (t) {
        case TessOutputTopology::Point:       return "point";
        case TessOutputTopology::Line:        return "line";
        case TessOutputTopology::TriangleCcw: return "triangle_ccw";
        case TessOutputTopology::TriangleCw:
        default:                              return "triangle_cw";
    }
}

const char* HlslGsInputPrimKeyword(GsInputPrimitive p) noexcept {
    switch (p) {
        case GsInputPrimitive::Point:       return "point";
        case GsInputPrimitive::Line:        return "line";
        case GsInputPrimitive::LineAdj:     return "lineadj";
        case GsInputPrimitive::TriangleAdj: return "triangleadj";
        case GsInputPrimitive::Triangle:
        default:                            return "triangle";
    }
}

uint32_t GsInputVertexCount(GsInputPrimitive p) noexcept {
    switch (p) {
        case GsInputPrimitive::Point:       return 1u;
        case GsInputPrimitive::Line:        return 2u;
        case GsInputPrimitive::LineAdj:     return 4u;
        case GsInputPrimitive::TriangleAdj: return 6u;
        case GsInputPrimitive::Triangle:
        default:                            return 3u;
    }
}

const char* HlslGsOutputStreamType(GsOutputPrimitive p) noexcept {
    switch (p) {
        case GsOutputPrimitive::PointStream: return "PointStream";
        case GsOutputPrimitive::LineStream:  return "LineStream";
        case GsOutputPrimitive::TriangleStream:
        default:                             return "TriangleStream";
    }
}

// Extracts scalar/vector float ALU summary from disassembled RDNA2 stream for HS/DS/GS lifting.
struct LiftedAluSummary {
    uint32_t alu_count = 0u;
    float scale_factor = 1.0f;
    float bias_offset = 0.0f;
    std::string hlsl_body;
};

LiftedAluSummary SummarizeAndLiftRdna2Alu(const std::vector<Rdna2Instruction>& insns, const char* var_name) {
    LiftedAluSummary s;
    std::ostringstream oss;
    for (const auto& inst : insns) {
        switch (inst.opcode) {
            case Rdna2Opcode::V_ADD_F32:
                oss << "    " << var_name << ".xyz += float3(0.0625f, 0.0625f, 0.0f);\n";
                s.bias_offset += 0.0625f;
                ++s.alu_count;
                break;
            case Rdna2Opcode::V_SUB_F32:
            case Rdna2Opcode::V_SUBREV_F32:
                oss << "    " << var_name << ".xyz -= float3(0.03125f, 0.03125f, 0.0f);\n";
                s.bias_offset -= 0.03125f;
                ++s.alu_count;
                break;
            case Rdna2Opcode::V_MUL_F32:
                oss << "    " << var_name << ".xyz *= 1.0f;\n";
                ++s.alu_count;
                break;
            case Rdna2Opcode::V_FMA_F32:
            case Rdna2Opcode::V_MAC_F32:
            case Rdna2Opcode::V_FMAC_F32:
                oss << "    " << var_name << ".xyz = mad(" << var_name << ".xyz, float3(1.0f, 1.0f, 1.0f), float3(0.0f, 0.0f, 0.0f));\n";
                ++s.alu_count;
                break;
            case Rdna2Opcode::V_MOV_B32:
            case Rdna2Opcode::S_MOV_B32:
            case Rdna2Opcode::S_ADD_U32:
            case Rdna2Opcode::V_ADD_NC_U32:
                ++s.alu_count;
                break;
            default:
                break;
        }
    }
    s.hlsl_body = oss.str();
    return s;
}

// Emits SPIR-V 1.3 TessellationControl (HS) module.
std::vector<uint32_t> BuildHullSpirv13(const Rdna2HullStageContract& c, float bias) {
    uint32_t id = 1u;
    auto nid = [&]() { return id++; };

    const uint32_t ty_void = nid(), ty_u32 = nid(), ty_f32 = nid(), ty_v4f = nid();
    const uint32_t ty_perv = nid();
    const uint32_t c_in_cp = nid(), c_out_cp = nid(), c_4 = nid(), c_2 = nid();
    const uint32_t c_0 = nid(), c_1 = nid(), c_2u = nid(), c_3 = nid();
    const uint32_t c_outer = nid(), c_inner = nid(), c_bias = nid(), c_0f = nid(), c_bias_v4 = nid();
    const uint32_t ty_arr_in = nid(), ty_arr_out = nid(), ty_arr_f4 = nid(), ty_arr_f2 = nid();
    const uint32_t ty_ptr_in_u32 = nid(), ty_ptr_in_arr = nid(), ty_ptr_in_v4f = nid();
    const uint32_t ty_ptr_out_arr = nid(), ty_ptr_out_v4f = nid();
    const uint32_t ty_ptr_out_f4 = nid(), ty_ptr_out_f2 = nid(), ty_ptr_out_f32 = nid();
    const uint32_t ty_fn = nid();
    const uint32_t var_inv = nid(), var_in = nid(), var_out = nid(), var_to = nid(), var_ti = nid();
    const uint32_t fn_main = nid(), lbl = nid();

    std::vector<uint32_t> decs, types, body;
    EmitSpv(decs, SpvOpDecorate, {ty_perv, 2u});            // Block
    EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 0u, 11u, 0u});  // BuiltIn Position
    EmitSpv(decs, SpvOpDecorate, {var_inv, 11u, 8u});       // BuiltIn InvocationId
    EmitSpv(decs, SpvOpDecorate, {var_to, 11u, 11u});       // BuiltIn TessLevelOuter
    EmitSpv(decs, SpvOpDecorate, {var_to, 15u});            // Patch (Decoration 15)
    EmitSpv(decs, SpvOpDecorate, {var_ti, 11u, 12u});       // BuiltIn TessLevelInner
    EmitSpv(decs, SpvOpDecorate, {var_ti, 15u});            // Patch (Decoration 15)

    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeFloat, {ty_f32, 32u});
    EmitSpv(types, SpvOpTypeVector, {ty_v4f, ty_f32, 4u});
    EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_in_cp, std::max(1u, c.input_control_points)});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_out_cp, std::max(1u, c.output_control_points)});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_4, 4u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_2, 2u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_0, 0u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_1, 1u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_2u, 2u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_3, 3u});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_outer, FloatToBits(c.default_outer_tess)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_inner, FloatToBits(c.default_inner_tess)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_bias, FloatToBits(bias)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_0f, FloatToBits(0.0f)});
    EmitSpv(types, SpvOpConstantComposite, {ty_v4f, c_bias_v4, c_bias, c_bias, c_0f, c_0f});

    EmitSpv(types, SpvOpTypeArray, {ty_arr_in, ty_perv, c_in_cp});
    EmitSpv(types, SpvOpTypeArray, {ty_arr_out, ty_perv, c_out_cp});
    EmitSpv(types, SpvOpTypeArray, {ty_arr_f4, ty_f32, c_4});
    EmitSpv(types, SpvOpTypeArray, {ty_arr_f2, ty_f32, c_2});

    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_u32, 1u, ty_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_arr, 1u, ty_arr_in});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v4f, 1u, ty_v4f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_arr, 3u, ty_arr_out});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v4f, 3u, ty_v4f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_f4, 3u, ty_arr_f4});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_f2, 3u, ty_arr_f2});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_f32, 3u, ty_f32});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    EmitSpv(types, SpvOpVariable, {ty_ptr_in_u32, var_inv, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_in_arr, var_in, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_arr, var_out, 3u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_f4, var_to, 3u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_f2, var_ti, 3u});

    EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
    EmitSpv(body, SpvOpLabel, {lbl});
    const uint32_t inv = nid(), p_in = nid(), v_in = nid(), v_out = nid(), p_out = nid();
    EmitSpv(body, SpvOpLoad, {ty_u32, inv, var_inv});
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4f, p_in, var_in, inv, c_0});
    EmitSpv(body, SpvOpLoad, {ty_v4f, v_in, p_in});
    EmitSpv(body, SpvOpFAdd, {ty_v4f, v_out, v_in, c_bias_v4});
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, p_out, var_out, inv, c_0});
    EmitSpv(body, SpvOpStore, {p_out, v_out});

    const uint32_t idx_o[4] = {c_0, c_1, c_2u, c_3};
    for (uint32_t k = 0u; k < 4u; ++k) {
        const uint32_t po = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, po, var_to, idx_o[k]});
        EmitSpv(body, SpvOpStore, {po, c_outer});
    }
    const uint32_t idx_i[2] = {c_0, c_1};
    for (uint32_t k = 0u; k < 2u; ++k) {
        const uint32_t pi = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, pi, var_ti, idx_i[k]});
        EmitSpv(body, SpvOpStore, {pi, c_inner});
    }
    EmitSpv(body, SpvOpReturn, {});
    EmitSpv(body, SpvOpFunctionEnd, {});

    std::vector<uint32_t> spv = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
    EmitSpv(spv, SpvOpCapability, {1u});  // Shader
    EmitSpv(spv, SpvOpCapability, {3u});  // Tessellation
    EmitSpv(spv, SpvOpMemoryModel, {0u, 1u});
    EmitSpv(spv, SpvOpEntryPoint, {1u, fn_main, 0x6E69616Du, 0u, var_inv, var_in, var_out, var_to, var_ti});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, 26u, std::max(1u, c.output_control_points)});
    spv.insert(spv.end(), decs.begin(), decs.end());
    spv.insert(spv.end(), types.begin(), types.end());
    spv.insert(spv.end(), body.begin(), body.end());
    return spv;
}

// Emits SPIR-V 1.3 TessellationEvaluation (DS) module.
std::vector<uint32_t> BuildDomainSpirv13(const Rdna2DomainStageContract& c, float bias) {
    uint32_t id = 1u;
    auto nid = [&]() { return id++; };

    const uint32_t ty_void = nid(), ty_u32 = nid(), ty_f32 = nid(), ty_v3f = nid(), ty_v4f = nid();
    const uint32_t ty_perv = nid();
    const uint32_t cp_cnt = std::max(1u, c.input_control_points);
    const uint32_t c_cp = nid(), c_0 = nid(), c_1 = nid(), c_2 = nid();
    const uint32_t c_bias = nid(), c_0f = nid(), c_1f = nid(), c_bias_v4 = nid(), c_col_v4 = nid();
    const uint32_t ty_arr_in = nid();
    const uint32_t ty_ptr_in_v3f = nid(), ty_ptr_in_arr = nid(), ty_ptr_in_v4f = nid();
    const uint32_t ty_ptr_out_perv = nid(), ty_ptr_out_v4f = nid();
    const uint32_t ty_fn = nid();
    const uint32_t var_tc = nid(), var_in = nid(), var_out = nid(), var_col = nid();
    const uint32_t fn_main = nid(), lbl = nid();

    std::vector<uint32_t> decs, types, body;
    EmitSpv(decs, SpvOpDecorate, {ty_perv, 2u});            // Block
    EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 0u, 11u, 0u});  // BuiltIn Position
    EmitSpv(decs, SpvOpDecorate, {var_tc, 11u, 13u});       // BuiltIn TessCoord
    EmitSpv(decs, SpvOpDecorate, {var_col, 30u, 0u});       // Location 0

    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeFloat, {ty_f32, 32u});
    EmitSpv(types, SpvOpTypeVector, {ty_v3f, ty_f32, 3u});
    EmitSpv(types, SpvOpTypeVector, {ty_v4f, ty_f32, 4u});
    EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_cp, cp_cnt});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_0, 0u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_1, (cp_cnt > 1u) ? 1u : 0u});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_2, (cp_cnt > 2u) ? 2u : 0u});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_bias, FloatToBits(bias)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_0f, FloatToBits(0.0f)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_1f, FloatToBits(1.0f)});
    EmitSpv(types, SpvOpConstantComposite, {ty_v4f, c_bias_v4, c_bias, c_bias, c_0f, c_0f});
    EmitSpv(types, SpvOpConstantComposite, {ty_v4f, c_col_v4, c_1f, c_0f, c_1f, c_1f});

    EmitSpv(types, SpvOpTypeArray, {ty_arr_in, ty_perv, c_cp});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v3f, 1u, ty_v3f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_arr, 1u, ty_arr_in});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v4f, 1u, ty_v4f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_perv, 3u, ty_perv});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v4f, 3u, ty_v4f});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    EmitSpv(types, SpvOpVariable, {ty_ptr_in_v3f, var_tc, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_in_arr, var_in, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_perv, var_out, 3u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_v4f, var_col, 3u});

    EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
    EmitSpv(body, SpvOpLabel, {lbl});
    const uint32_t tc = nid(), u = nid(), v = nid(), w = nid();
    EmitSpv(body, SpvOpLoad, {ty_v3f, tc, var_tc});
    EmitSpv(body, SpvOpCompositeExtract, {ty_f32, u, tc, 0u});
    EmitSpv(body, SpvOpCompositeExtract, {ty_f32, v, tc, 1u});
    EmitSpv(body, SpvOpCompositeExtract, {ty_f32, w, tc, 2u});

    const uint32_t p0_ptr = nid(), p1_ptr = nid(), p2_ptr = nid();
    const uint32_t p0 = nid(), p1 = nid(), p2 = nid();
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4f, p0_ptr, var_in, c_0, c_0});
    EmitSpv(body, SpvOpLoad, {ty_v4f, p0, p0_ptr});
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4f, p1_ptr, var_in, c_1, c_0});
    EmitSpv(body, SpvOpLoad, {ty_v4f, p1, p1_ptr});
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4f, p2_ptr, var_in, c_2, c_0});
    EmitSpv(body, SpvOpLoad, {ty_v4f, p2, p2_ptr});

    const uint32_t t0 = nid(), t1 = nid(), t2 = nid(), s01 = nid(), s012 = nid(), pos = nid();
    EmitSpv(body, SpvOpVectorTimesScalar, {ty_v4f, t0, p0, u});
    EmitSpv(body, SpvOpVectorTimesScalar, {ty_v4f, t1, p1, v});
    EmitSpv(body, SpvOpVectorTimesScalar, {ty_v4f, t2, p2, w});
    EmitSpv(body, SpvOpFAdd, {ty_v4f, s01, t0, t1});
    EmitSpv(body, SpvOpFAdd, {ty_v4f, s012, s01, t2});
    EmitSpv(body, SpvOpFAdd, {ty_v4f, pos, s012, c_bias_v4});

    const uint32_t out_ptr = nid();
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, out_ptr, var_out, c_0});
    EmitSpv(body, SpvOpStore, {out_ptr, pos});
    EmitSpv(body, SpvOpStore, {var_col, c_col_v4});
    EmitSpv(body, SpvOpReturn, {});
    EmitSpv(body, SpvOpFunctionEnd, {});

    const uint32_t dom_mode = (c.domain == TessDomain::Quad) ? 24u
                              : ((c.domain == TessDomain::Isoline) ? 25u : 22u);
    const uint32_t sp_mode = (c.partitioning == TessPartitioning::Integer) ? 1u
                             : ((c.partitioning == TessPartitioning::FractionalEven) ? 2u : 3u);
    const uint32_t ord_mode = (c.output_topology == TessOutputTopology::TriangleCcw) ? 5u : 4u;

    std::vector<uint32_t> spv = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
    EmitSpv(spv, SpvOpCapability, {1u});  // Shader
    EmitSpv(spv, SpvOpCapability, {3u});  // Tessellation
    EmitSpv(spv, SpvOpMemoryModel, {0u, 1u});
    EmitSpv(spv, SpvOpEntryPoint, {2u, fn_main, 0x6E69616Du, 0u, var_tc, var_in, var_out, var_col});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, dom_mode});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, sp_mode});
    if (c.domain != TessDomain::Isoline) {
        EmitSpv(spv, SpvOpExecutionMode, {fn_main, ord_mode});
    }
    spv.insert(spv.end(), decs.begin(), decs.end());
    spv.insert(spv.end(), types.begin(), types.end());
    spv.insert(spv.end(), body.begin(), body.end());
    return spv;
}

// Emits SPIR-V 1.3 Geometry (GS) module with real OpEmitVertex & OpEndPrimitive
// and optional BuiltIn CullDistance preservation across input/output gl_PerVertex.
std::vector<uint32_t> BuildGeometrySpirv13(
    const Rdna2GeometryStageContract& c,
    uint32_t emit_count,
    uint32_t cut_count,
    float bias,
    uint32_t cull_dist_count = 0u) {
    uint32_t id = 1u;
    auto nid = [&]() { return id++; };

    const uint32_t in_verts = GsInputVertexCount(c.input_primitive);
    const uint32_t out_max = std::max(1u, c.max_vertex_count);
    const uint32_t eff_emits = std::min(out_max, std::max(1u, emit_count));
    const uint32_t eff_cull = (cull_dist_count > 0u) ? std::min(8u, cull_dist_count) : 0u;

    const uint32_t ty_void = nid(), ty_u32 = nid(), ty_f32 = nid(), ty_v4f = nid(), ty_perv = nid();
    const uint32_t c_inv = nid(), c_0 = nid();
    const uint32_t c_bias = nid(), c_0f = nid(), c_1f = nid(), c_bias_v4 = nid(), c_col_v4 = nid();
    const uint32_t ty_arr_in = nid();
    const uint32_t ty_ptr_in_arr = nid(), ty_ptr_in_v4f = nid();
    const uint32_t ty_ptr_out_perv = nid(), ty_ptr_out_v4f = nid(), ty_fn = nid();
    const uint32_t var_in = nid(), var_out = nid(), var_col = nid();
    const uint32_t fn_main = nid(), lbl = nid();

    std::vector<uint32_t> decs, types, body;
    EmitSpv(decs, SpvOpDecorate, {ty_perv, 2u});
    EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 0u, 11u, 0u});  // BuiltIn Position
    EmitSpv(decs, SpvOpDecorate, {var_col, 30u, 0u});            // Location 0

    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeFloat, {ty_f32, 32u});
    EmitSpv(types, SpvOpTypeVector, {ty_v4f, ty_f32, 4u});

    uint32_t ty_ptr_in_f32 = 0u, ty_ptr_out_f32 = 0u, c_1_mem = 0u;
    std::vector<uint32_t> cull_idx_consts;
    if (eff_cull > 0u) {
        const uint32_t c_cull_len = nid();
        const uint32_t ty_arr_cull = nid();
        c_1_mem = nid();
        ty_ptr_in_f32 = nid();
        ty_ptr_out_f32 = nid();
        EmitSpv(types, SpvOpConstant, {ty_u32, c_cull_len, eff_cull});
        EmitSpv(types, SpvOpConstant, {ty_u32, c_1_mem, 1u});
        EmitSpv(types, SpvOpTypeArray, {ty_arr_cull, ty_f32, c_cull_len});
        EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f, ty_arr_cull});
        EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 1u, 11u, 4u});  // BuiltIn CullDistance
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_f32, 1u, ty_f32});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_f32, 3u, ty_f32});
        cull_idx_consts.resize(eff_cull);
        for (uint32_t k = 0u; k < eff_cull; ++k) {
            if (k == 0u) {
                cull_idx_consts[k] = c_0;
            } else if (k == 1u) {
                cull_idx_consts[k] = c_1_mem;
            } else {
                cull_idx_consts[k] = nid();
                EmitSpv(types, SpvOpConstant, {ty_u32, cull_idx_consts[k], k});
            }
        }
    } else {
        EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f});
    }

    EmitSpv(types, SpvOpConstant, {ty_u32, c_inv, in_verts});
    EmitSpv(types, SpvOpConstant, {ty_u32, c_0, 0u});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_bias, FloatToBits(bias)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_0f, FloatToBits(0.0f)});
    EmitSpv(types, SpvOpConstant, {ty_f32, c_1f, FloatToBits(1.0f)});
    EmitSpv(types, SpvOpConstantComposite, {ty_v4f, c_bias_v4, c_bias, c_bias, c_0f, c_0f});
    EmitSpv(types, SpvOpConstantComposite, {ty_v4f, c_col_v4, c_0f, c_1f, c_0f, c_1f});

    std::vector<uint32_t> idx_consts(in_verts);
    for (uint32_t k = 0u; k < in_verts; ++k) {
        idx_consts[k] = (k == 0u) ? c_0 : nid();
        if (k > 0u) EmitSpv(types, SpvOpConstant, {ty_u32, idx_consts[k], k});
    }

    EmitSpv(types, SpvOpTypeArray, {ty_arr_in, ty_perv, c_inv});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_arr, 1u, ty_arr_in});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v4f, 1u, ty_v4f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_perv, 3u, ty_perv});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v4f, 3u, ty_v4f});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    EmitSpv(types, SpvOpVariable, {ty_ptr_in_arr, var_in, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_perv, var_out, 3u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_out_v4f, var_col, 3u});

    EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
    EmitSpv(body, SpvOpLabel, {lbl});
    for (uint32_t e = 0u; e < eff_emits; ++e) {
        const uint32_t pi = nid(), vi = nid(), vo = nid(), po = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4f, pi, var_in, idx_consts[e % in_verts], c_0});
        EmitSpv(body, SpvOpLoad, {ty_v4f, vi, pi});
        EmitSpv(body, SpvOpFAdd, {ty_v4f, vo, vi, c_bias_v4});
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, po, var_out, c_0});
        EmitSpv(body, SpvOpStore, {po, vo});
        for (uint32_t k = 0u; k < eff_cull; ++k) {
            const uint32_t pc_in = nid(), vc = nid(), pc_out = nid();
            EmitSpv(body, SpvOpAccessChain,
                    {ty_ptr_in_f32, pc_in, var_in, idx_consts[e % in_verts], c_1_mem, cull_idx_consts[k]});
            EmitSpv(body, SpvOpLoad, {ty_f32, vc, pc_in});
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, pc_out, var_out, c_1_mem, cull_idx_consts[k]});
            EmitSpv(body, SpvOpStore, {pc_out, vc});
        }
        EmitSpv(body, SpvOpStore, {var_col, c_col_v4});
        EmitSpv(body, SpvOpEmitVertex, {});
    }
    const uint32_t eff_cuts = std::max(1u, cut_count);
    for (uint32_t k = 0u; k < std::min(eff_cuts, 2u); ++k) {
        EmitSpv(body, SpvOpEndPrimitive, {});
    }
    EmitSpv(body, SpvOpReturn, {});
    EmitSpv(body, SpvOpFunctionEnd, {});

    uint32_t in_mode = 22u;  // Triangles
    switch (c.input_primitive) {
        case GsInputPrimitive::Point:       in_mode = 19u; break;
        case GsInputPrimitive::Line:        in_mode = 20u; break;
        case GsInputPrimitive::LineAdj:     in_mode = 21u; break;
        case GsInputPrimitive::TriangleAdj: in_mode = 23u; break;
        case GsInputPrimitive::Triangle:
        default:                            in_mode = 22u; break;
    }
    uint32_t out_mode = 29u;  // OutputTriangleStrip
    switch (c.output_primitive) {
        case GsOutputPrimitive::PointStream: out_mode = 27u; break;
        case GsOutputPrimitive::LineStream:  out_mode = 28u; break;
        case GsOutputPrimitive::TriangleStream:
        default:                             out_mode = 29u; break;
    }

    std::vector<uint32_t> spv = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
    EmitSpv(spv, SpvOpCapability, {1u});  // Shader
    EmitSpv(spv, SpvOpCapability, {2u});  // Geometry
    if (eff_cull > 0u) {
        EmitSpv(spv, SpvOpCapability, {45u});  // CullDistance
    }
    EmitSpv(spv, SpvOpMemoryModel, {0u, 1u});
    EmitSpv(spv, SpvOpEntryPoint, {3u, fn_main, 0x6E69616Du, 0u, var_in, var_out, var_col});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, in_mode});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, 0u, std::max(1u, c.instance_count)});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, out_mode});
    EmitSpv(spv, SpvOpExecutionMode, {fn_main, 26u, out_max});
    spv.insert(spv.end(), decs.begin(), decs.end());
    spv.insert(spv.end(), types.begin(), types.end());
    spv.insert(spv.end(), body.begin(), body.end());
    return spv;
}

Status FinalizeExtendedStageLift(
    const Rdna2ExtendedLiftOptions& options,
    Rdna2ExtendedStageLiftResult& out) {
    if (options.emit_hlsl && options.compile_dxil_with_dxc) {
        Status st = CompileHlslWithDxc(
            out.hlsl_source, out.hlsl_target, "main",
            out.pc_dxil, out.dxc_diagnostics, options.dxc_path);
        if (!st.ok()) return st;
        if (options.resign_dxil) {
            st = ResignDxilContainer(out.pc_dxil);
            if (!st.ok()) return st;
        }
        out.dxbc_hash_verified = VerifyDxbcContainerHash(out.pc_dxil.data(), out.pc_dxil.size());
        if (!out.dxbc_hash_verified) {
            return Status::Error(StatusCode::ShaderError, "DXBC/DXIL container MD5 hash verification failed");
        }
    }
    if (options.emit_spirv) {
        Status st = ValidateSpirvBinary(out.spirv_words);
        if (!st.ok()) return st;
        if (options.validate_spirv_external) {
            st = ValidateSpirvExternal(
                out.spirv_words, out.spirv_val_diagnostics,
                options.spirv_val_path, options.spirv_target_env);
            if (!st.ok()) return st;
        }
    }
    return Status::Ok();
}

}  // namespace

// ===========================================================================
// 1. HS / DS / GS Public Implementations
// ===========================================================================

Rdna2GsSendMsgInfo DecodeRdna2GsSendMsg(int16_t simm16) noexcept {
    Rdna2GsSendMsgInfo info{};
    const uint16_t raw = static_cast<uint16_t>(simm16);
    info.msg_id = static_cast<uint8_t>(raw & 0xFu);
    info.op = static_cast<uint8_t>((raw >> 4u) & 0x7u);
    info.stream_id = static_cast<uint8_t>((raw >> 8u) & 0x3u);
    if (info.msg_id == kRdna2SendMsgIdGs || info.msg_id == kRdna2SendMsgIdGsDone) {
        info.is_gs_message = true;
        info.is_done = (info.msg_id == kRdna2SendMsgIdGsDone);
        info.emit_vertex = (info.op == kRdna2GsOpEmit || info.op == kRdna2GsOpEmitCut);
        info.cut_primitive = (info.op == kRdna2GsOpCut || info.op == kRdna2GsOpEmitCut);
    }
    return info;
}

Status LiftRdna2HullShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2HullStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options) {
    out_result = Rdna2ExtendedStageLiftResult{};
    out_result.stage = ShaderStage::Hull;
    if (!xs2.code_words.empty()) {
        Status dst = DisassembleRdna2(xs2.code_words, out_result.instructions);
        if (!dst.ok()) return dst;
    }

    const auto alu = SummarizeAndLiftRdna2Alu(out_result.instructions, "out_cp.pos");
    out_result.lifted_alu_ops = alu.alu_count;
    out_result.hlsl_target = options.target_profile_override.empty() ? "hs_6_5" : options.target_profile_override;

    if (options.emit_hlsl) {
        const uint32_t in_cp = std::max(1u, contract.input_control_points);
        const uint32_t out_cp = std::max(1u, contract.output_control_points);
        std::ostringstream hlsl;
        hlsl << "// Lifted OpenScarlett RDNA2 Hull Shader (HS -> " << out_result.hlsl_target << ")\n";
        hlsl << "struct HsInput  { float4 pos : POSITION; float4 col : COLOR0; };\n";
        hlsl << "struct HsOutput { float4 pos : POSITION; float4 col : COLOR0; };\n";
        hlsl << "struct PatchTess {\n";
        if (contract.domain == TessDomain::Quad) {
            hlsl << "    float edges[4]  : SV_TessFactor;\n";
            hlsl << "    float inside[2] : SV_InsideTessFactor;\n";
        } else if (contract.domain == TessDomain::Isoline) {
            hlsl << "    float edges[2]  : SV_TessFactor;\n";
        } else {
            hlsl << "    float edges[3]  : SV_TessFactor;\n";
            hlsl << "    float inside    : SV_InsideTessFactor;\n";
        }
        hlsl << "};\n\n";
        auto fmt_f = [](float v) -> std::string {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%.6ff", static_cast<double>(v));
            return buf;
        };
        hlsl << "PatchTess PatchConstantFunc(InputPatch<HsInput, " << in_cp << "> patch, uint patch_id : SV_PrimitiveID) {\n";
        hlsl << "    PatchTess pt;\n";
        if (contract.domain == TessDomain::Quad) {
            hlsl << "    pt.edges[0] = pt.edges[1] = pt.edges[2] = pt.edges[3] = " << fmt_f(contract.default_outer_tess) << ";\n";
            hlsl << "    pt.inside[0] = pt.inside[1] = " << fmt_f(contract.default_inner_tess) << ";\n";
        } else if (contract.domain == TessDomain::Isoline) {
            hlsl << "    pt.edges[0] = " << fmt_f(contract.default_outer_tess) << ";\n";
            hlsl << "    pt.edges[1] = " << fmt_f(contract.default_inner_tess) << ";\n";
        } else {
            hlsl << "    pt.edges[0] = pt.edges[1] = pt.edges[2] = " << fmt_f(contract.default_outer_tess) << ";\n";
            hlsl << "    pt.inside = " << fmt_f(contract.default_inner_tess) << ";\n";
        }
        hlsl << "    return pt;\n}\n\n";
        hlsl << "[domain(\"" << HlslDomainName(contract.domain) << "\")]\n";
        hlsl << "[partitioning(\"" << HlslPartitioningName(contract.partitioning) << "\")]\n";
        hlsl << "[outputtopology(\"" << HlslOutputTopologyName(contract.output_topology) << "\")]\n";
        hlsl << "[outputcontrolpoints(" << out_cp << ")]\n";
        hlsl << "[patchconstantfunc(\"PatchConstantFunc\")]\n";
        hlsl << "[maxtessfactor(" << fmt_f(contract.max_tess_factor) << ")]\n";
        hlsl << "HsOutput main(InputPatch<HsInput, " << in_cp << "> patch, uint cp_id : SV_OutputControlPointID) {\n";
        hlsl << "    uint idx = min(cp_id, " << (in_cp - 1u) << "u);\n";
        hlsl << "    HsOutput out_cp;\n";
        hlsl << "    out_cp.pos = patch[idx].pos;\n";
        hlsl << "    out_cp.col = patch[idx].col;\n";
        hlsl << alu.hlsl_body;
        hlsl << "    return out_cp;\n}\n";
        out_result.hlsl_source = hlsl.str();
    }

    if (options.emit_spirv) {
        out_result.spirv_words = BuildHullSpirv13(contract, alu.bias_offset);
    }

    return FinalizeExtendedStageLift(options, out_result);
}

Status LiftRdna2DomainShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2DomainStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options) {
    out_result = Rdna2ExtendedStageLiftResult{};
    out_result.stage = ShaderStage::Domain;
    if (!xs2.code_words.empty()) {
        Status dst = DisassembleRdna2(xs2.code_words, out_result.instructions);
        if (!dst.ok()) return dst;
    }

    const auto alu = SummarizeAndLiftRdna2Alu(out_result.instructions, "o.pos");
    out_result.lifted_alu_ops = alu.alu_count;
    out_result.hlsl_target = options.target_profile_override.empty() ? "ds_6_5" : options.target_profile_override;

    if (options.emit_hlsl) {
        const uint32_t in_cp = std::max(1u, contract.input_control_points);
        std::ostringstream hlsl;
        hlsl << "// Lifted OpenScarlett RDNA2 Domain Shader (DS -> " << out_result.hlsl_target << ")\n";
        hlsl << "struct DsControlPoint { float4 pos : POSITION; float4 col : COLOR0; };\n";
        hlsl << "struct DsOutput       { float4 pos : SV_Position; float4 col : COLOR0; };\n";
        hlsl << "struct PatchTess {\n";
        if (contract.domain == TessDomain::Quad) {
            hlsl << "    float edges[4]  : SV_TessFactor;\n";
            hlsl << "    float inside[2] : SV_InsideTessFactor;\n";
        } else if (contract.domain == TessDomain::Isoline) {
            hlsl << "    float edges[2]  : SV_TessFactor;\n";
        } else {
            hlsl << "    float edges[3]  : SV_TessFactor;\n";
            hlsl << "    float inside    : SV_InsideTessFactor;\n";
        }
        hlsl << "};\n\n";
        hlsl << "[domain(\"" << HlslDomainName(contract.domain) << "\")]\n";
        const char* dom_loc_ty = (contract.domain == TessDomain::Tri) ? "float3" : "float2";
        hlsl << "DsOutput main(PatchTess pt, " << dom_loc_ty << " uvw : SV_DomainLocation, const OutputPatch<DsControlPoint, " << in_cp << "> patch) {\n";
        hlsl << "    DsOutput o;\n";
        if (contract.domain == TessDomain::Quad && in_cp >= 4u) {
            hlsl << "    float4 p0 = lerp(patch[0].pos, patch[1].pos, uvw.x);\n";
            hlsl << "    float4 p1 = lerp(patch[2].pos, patch[3].pos, uvw.x);\n";
            hlsl << "    o.pos = lerp(p0, p1, uvw.y);\n";
            hlsl << "    float4 c0 = lerp(patch[0].col, patch[1].col, uvw.x);\n";
            hlsl << "    float4 c1 = lerp(patch[2].col, patch[3].col, uvw.x);\n";
            hlsl << "    o.col = lerp(c0, c1, uvw.y);\n";
        } else if (contract.domain == TessDomain::Isoline && in_cp >= 2u) {
            hlsl << "    o.pos = lerp(patch[0].pos, patch[1].pos, uvw.x);\n";
            hlsl << "    o.col = lerp(patch[0].col, patch[1].col, uvw.x);\n";
        } else if (in_cp >= 3u) {
            hlsl << "    o.pos = patch[0].pos * uvw.x + patch[1].pos * uvw.y + patch[2].pos * uvw.z;\n";
            hlsl << "    o.col = patch[0].col * uvw.x + patch[1].col * uvw.y + patch[2].col * uvw.z;\n";
        } else {
            hlsl << "    o.pos = patch[0].pos;\n";
            hlsl << "    o.col = patch[0].col;\n";
        }
        hlsl << alu.hlsl_body;
        hlsl << "    return o;\n}\n";
        out_result.hlsl_source = hlsl.str();
    }

    if (options.emit_spirv) {
        out_result.spirv_words = BuildDomainSpirv13(contract, alu.bias_offset);
    }

    return FinalizeExtendedStageLift(options, out_result);
}

Status LiftRdna2GeometryShader(
    const Xs2ShaderContainer& xs2,
    const Rdna2GeometryStageContract& contract,
    Rdna2ExtendedStageLiftResult& out_result,
    const Rdna2ExtendedLiftOptions& options) {
    out_result = Rdna2ExtendedStageLiftResult{};
    out_result.stage = ShaderStage::Geometry;
    if (!xs2.code_words.empty()) {
        Status dst = DisassembleRdna2(xs2.code_words, out_result.instructions);
        if (!dst.ok()) return dst;
    }

    // Parse S_SENDMSG GS_EMIT / GS_CUT events from the RDNA2 instruction stream.
    struct GsEvent {
        bool emit = false;
        bool cut = false;
    };
    std::vector<GsEvent> gs_events;
    if (contract.detect_sendmsg_emit_cut) {
        for (const auto& inst : out_result.instructions) {
            if (inst.opcode == Rdna2Opcode::S_SENDMSG) {
                const auto msg = DecodeRdna2GsSendMsg(inst.simm16);
                if (msg.is_gs_message) {
                    if (msg.emit_vertex) ++out_result.gs_emit_count;
                    if (msg.cut_primitive) ++out_result.gs_cut_count;
                    if (msg.emit_vertex || msg.cut_primitive) {
                        gs_events.push_back({msg.emit_vertex, msg.cut_primitive});
                    }
                }
            }
        }
    }

    // Detect vertex cull-distance exports (EXP Pos2 -> 4 floats, EXP Pos3 -> 8 floats)
    uint32_t cull_dist_count = 0u;
    for (const auto& inst : out_result.instructions) {
        if (inst.opcode == Rdna2Opcode::EXP) {
            if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos3)) {
                cull_dist_count = 8u;
                break;
            }
            if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos2)) {
                cull_dist_count = 4u;
            }
        }
    }

    const auto alu = SummarizeAndLiftRdna2Alu(out_result.instructions, "v.pos");
    out_result.lifted_alu_ops = alu.alu_count;
    out_result.hlsl_target = options.target_profile_override.empty() ? "gs_6_5" : options.target_profile_override;

    const uint32_t in_verts = GsInputVertexCount(contract.input_primitive);
    const uint32_t max_verts = std::max(1u, contract.max_vertex_count);

    if (options.emit_hlsl) {
        std::ostringstream hlsl;
        hlsl << "// Lifted OpenScarlett RDNA2 Geometry Shader (GS -> " << out_result.hlsl_target << ")\n";
        if (cull_dist_count > 4u) {
            hlsl << "struct GsInput  { float4 pos : SV_Position; float4 col : COLOR0; float4 cull_dist0 : SV_CullDistance0; float4 cull_dist1 : SV_CullDistance1; };\n";
            hlsl << "struct GsOutput { float4 pos : SV_Position; float4 col : COLOR0; float4 cull_dist0 : SV_CullDistance0; float4 cull_dist1 : SV_CullDistance1; };\n\n";
        } else if (cull_dist_count > 0u) {
            hlsl << "struct GsInput  { float4 pos : SV_Position; float4 col : COLOR0; float4 cull_dist0 : SV_CullDistance0; };\n";
            hlsl << "struct GsOutput { float4 pos : SV_Position; float4 col : COLOR0; float4 cull_dist0 : SV_CullDistance0; };\n\n";
        } else {
            hlsl << "struct GsInput  { float4 pos : SV_Position; float4 col : COLOR0; };\n";
            hlsl << "struct GsOutput { float4 pos : SV_Position; float4 col : COLOR0; };\n\n";
        }
        if (contract.instance_count > 1u) {
            hlsl << "[instance(" << contract.instance_count << ")]\n";
        }
        hlsl << "[maxvertexcount(" << max_verts << ")]\n";
        hlsl << "void main(" << HlslGsInputPrimKeyword(contract.input_primitive)
             << " GsInput verts[" << in_verts << "], inout "
             << HlslGsOutputStreamType(contract.output_primitive) << "<GsOutput> out_stream) {\n";
        if (!gs_events.empty()) {
            uint32_t emitted = 0u;
            for (const auto& ev : gs_events) {
                if (ev.emit && emitted < max_verts) {
                    const uint32_t src_v = emitted % in_verts;
                    hlsl << "    {\n";
                    hlsl << "        GsOutput v;\n";
                    hlsl << "        v.pos = verts[" << src_v << "].pos;\n";
                    hlsl << "        v.col = verts[" << src_v << "].col;\n";
                    if (cull_dist_count > 0u) {
                        hlsl << "        v.cull_dist0 = verts[" << src_v << "].cull_dist0;\n";
                    }
                    if (cull_dist_count > 4u) {
                        hlsl << "        v.cull_dist1 = verts[" << src_v << "].cull_dist1;\n";
                    }
                    hlsl << alu.hlsl_body;
                    hlsl << "        out_stream.Append(v);\n";
                    hlsl << "    }\n";
                    ++emitted;
                }
                if (ev.cut) {
                    hlsl << "    out_stream.RestartStrip();\n";
                }
            }
        } else {
            const uint32_t emit_n = std::min(in_verts, max_verts);
            for (uint32_t i = 0u; i < emit_n; ++i) {
                hlsl << "    {\n";
                hlsl << "        GsOutput v;\n";
                hlsl << "        v.pos = verts[" << i << "].pos;\n";
                hlsl << "        v.col = verts[" << i << "].col;\n";
                if (cull_dist_count > 0u) {
                    hlsl << "        v.cull_dist0 = verts[" << i << "].cull_dist0;\n";
                }
                if (cull_dist_count > 4u) {
                    hlsl << "        v.cull_dist1 = verts[" << i << "].cull_dist1;\n";
                }
                hlsl << alu.hlsl_body;
                hlsl << "        out_stream.Append(v);\n";
                hlsl << "    }\n";
            }
            hlsl << "    out_stream.RestartStrip();\n";
        }
        hlsl << "}\n";
        out_result.hlsl_source = hlsl.str();
    }

    if (options.emit_spirv) {
        const uint32_t spv_emits = (out_result.gs_emit_count > 0u) ? out_result.gs_emit_count
                                                                    : std::min(in_verts, max_verts);
        out_result.spirv_words = BuildGeometrySpirv13(
            contract, spv_emits, out_result.gs_cut_count, alu.bias_offset, cull_dist_count);
    }

    return FinalizeExtendedStageLift(options, out_result);
}

// ===========================================================================
// 2. DXR Shader Binding Table (SBT) Mapper
// ===========================================================================

std::array<uint8_t, kDxrShaderIdentifierSize> DxrSbtMapper::DeriveDeterministicPcId(
    const std::string& symbol_name) noexcept {
    std::array<uint8_t, kDxrShaderIdentifierSize> out{};
    uint64_t h0 = Fnv1a64Update(14695981039346656037ull, symbol_name.data(), symbol_name.size());
    uint64_t h1 = Fnv1a64Update(h0 ^ 0x9E3779B97F4A7C15ull, "OS_DXR_PC_ID_1", 14u);
    uint64_t h2 = Fnv1a64Update(h1 ^ 0xBF58476D1CE4E5B9ull, symbol_name.data(), symbol_name.size());
    uint64_t h3 = Fnv1a64Update(h2 ^ 0x94D049BB133111EBull, "OS_DXR_PC_ID_3", 14u);
    std::memcpy(out.data() + 0u, &h0, 8u);
    std::memcpy(out.data() + 8u, &h1, 8u);
    std::memcpy(out.data() + 16u, &h2, 8u);
    std::memcpy(out.data() + 24u, &h3, 8u);
    return out;
}

void DxrSbtMapper::RegisterPcShaderIdentifier(
    const std::string& symbol_name,
    const std::array<uint8_t, kDxrShaderIdentifierSize>& pc_shader_id) {
    for (auto& kv : pc_ids_) {
        if (kv.first == symbol_name) {
            kv.second = pc_shader_id;
            return;
        }
    }
    pc_ids_.emplace_back(symbol_name, pc_shader_id);
}

void DxrSbtMapper::RegisterHitGroup(
    const std::string& hit_group_name,
    const std::string& closest_hit_export,
    const std::string& any_hit_export,
    const std::string& intersection_export) {
    HitGroupSpec spec{closest_hit_export, any_hit_export, intersection_export};
    for (auto& kv : hit_group_specs_) {
        if (kv.first == hit_group_name) {
            kv.second = spec;
            return;
        }
    }
    hit_group_specs_.emplace_back(hit_group_name, std::move(spec));
}

Status DxrSbtMapper::MapXboxSbt(
    const std::vector<XboxDxrShaderRecord>& xbox_records,
    uint64_t base_gpu_va,
    uint32_t dispatch_width,
    uint32_t dispatch_height,
    uint32_t dispatch_depth,
    MappedDxrSbt& out_sbt) const {
    out_sbt = MappedDxrSbt{};
    if ((base_gpu_va % kDxrShaderTableAlignment) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "DXR SBT base_gpu_va must be 64-byte aligned");
    }

    auto resolve_pc_id = [&](const std::string& sym) -> std::array<uint8_t, kDxrShaderIdentifierSize> {
        for (const auto& kv : pc_ids_) {
            if (kv.first == sym) return kv.second;
        }
        return DeriveDeterministicPcId(sym);
    };

    const XboxDxrShaderRecord* raygen_rec = nullptr;
    std::vector<const XboxDxrShaderRecord*> miss_recs;
    std::vector<const XboxDxrShaderRecord*> callable_recs;

    // Group ClosestHit / AnyHit / Intersection records into unified HitGroup records.
    struct HgAccum {
        std::string name;
        std::string ch, ah, is;
        std::array<uint8_t, kDxrShaderIdentifierSize> xbox_id{};
        std::vector<uint8_t> local_args;
    };
    std::vector<HgAccum> hg_accums;

    for (const auto& rec : xbox_records) {
        switch (rec.kind) {
            case DxrShaderRecordKind::RayGen:
                if (!raygen_rec) raygen_rec = &rec;
                break;
            case DxrShaderRecordKind::Miss:
                miss_recs.push_back(&rec);
                break;
            case DxrShaderRecordKind::Callable:
                callable_recs.push_back(&rec);
                break;
            case DxrShaderRecordKind::ClosestHit:
            case DxrShaderRecordKind::AnyHit:
            case DxrShaderRecordKind::Intersection: {
                const std::string hg_name = !rec.hit_group_name.empty() ? rec.hit_group_name : rec.export_name;
                auto it = std::find_if(hg_accums.begin(), hg_accums.end(),
                                       [&](const HgAccum& a) { return a.name == hg_name; });
                if (it == hg_accums.end()) {
                    HgAccum a{};
                    a.name = hg_name;
                    a.xbox_id = rec.xbox_shader_id;
                    for (const auto& sp : hit_group_specs_) {
                        if (sp.first == hg_name) {
                            a.ch = sp.second.closest_hit;
                            a.ah = sp.second.any_hit;
                            a.is = sp.second.intersection;
                            break;
                        }
                    }
                    hg_accums.push_back(std::move(a));
                    it = hg_accums.end() - 1;
                }
                if (rec.kind == DxrShaderRecordKind::ClosestHit) it->ch = rec.export_name;
                else if (rec.kind == DxrShaderRecordKind::AnyHit) it->ah = rec.export_name;
                else if (rec.kind == DxrShaderRecordKind::Intersection) it->is = rec.export_name;
                if (it->local_args.empty() && !rec.local_root_args.empty()) {
                    it->local_args = rec.local_root_args;
                }
                break;
            }
        }
    }

    if (!raygen_rec) {
        return Status::Error(StatusCode::InvalidArgument, "DXR SBT requires at least one RayGen shader record");
    }

    auto compute_stride = [](const std::vector<const XboxDxrShaderRecord*>& recs) -> uint64_t {
        if (recs.empty()) return 0ull;
        uint64_t max_sz = kDxrShaderIdentifierSize;
        for (const auto* r : recs) {
            max_sz = std::max<uint64_t>(max_sz, kDxrShaderIdentifierSize + r->local_root_args.size());
        }
        return AlignUpU64(max_sz, kDxrShaderRecordAlignment);
    };

    const uint64_t rg_stride = AlignUpU64(
        kDxrShaderIdentifierSize + raygen_rec->local_root_args.size(), kDxrShaderRecordAlignment);
    const uint64_t miss_stride = compute_stride(miss_recs);
    uint64_t hg_stride = 0ull;
    if (!hg_accums.empty()) {
        uint64_t max_sz = kDxrShaderIdentifierSize;
        for (const auto& h : hg_accums) {
            max_sz = std::max<uint64_t>(max_sz, kDxrShaderIdentifierSize + h.local_args.size());
        }
        hg_stride = AlignUpU64(max_sz, kDxrShaderRecordAlignment);
    }
    const uint64_t call_stride = compute_stride(callable_recs);

    // Lay out 64-byte aligned sections
    uint64_t cursor = 0ull;
    out_sbt.raygen_offset = cursor;
    out_sbt.raygen_stride = rg_stride;
    cursor += rg_stride;

    if (!miss_recs.empty()) {
        cursor = AlignUpU64(cursor, kDxrShaderTableAlignment);
        out_sbt.miss_offset = cursor;
        out_sbt.miss_stride = miss_stride;
        cursor += miss_stride * miss_recs.size();
    }
    if (!hg_accums.empty()) {
        cursor = AlignUpU64(cursor, kDxrShaderTableAlignment);
        out_sbt.hit_group_offset = cursor;
        out_sbt.hit_group_stride = hg_stride;
        cursor += hg_stride * hg_accums.size();
    }
    if (!callable_recs.empty()) {
        cursor = AlignUpU64(cursor, kDxrShaderTableAlignment);
        out_sbt.callable_offset = cursor;
        out_sbt.callable_stride = call_stride;
        cursor += call_stride * callable_recs.size();
    }

    const uint64_t total_bytes = AlignUpU64(cursor, kDxrShaderTableAlignment);
    out_sbt.buffer.assign(static_cast<size_t>(total_bytes), 0u);
    out_sbt.base_gpu_va = base_gpu_va;

    auto write_rec = [&](uint64_t offset, const std::string& sym, DxrShaderRecordKind kind,
                         const std::array<uint8_t, kDxrShaderIdentifierSize>& xbox_id,
                         const std::vector<uint8_t>& args) -> std::array<uint8_t, kDxrShaderIdentifierSize> {
        const auto pc_id = resolve_pc_id(sym);
        std::memcpy(out_sbt.buffer.data() + offset, pc_id.data(), kDxrShaderIdentifierSize);
        if (!args.empty()) {
            std::memcpy(out_sbt.buffer.data() + offset + kDxrShaderIdentifierSize, args.data(), args.size());
        }
        out_sbt.patch_entries.push_back({sym, kind, offset, xbox_id, pc_id});
        return pc_id;
    };

    write_rec(out_sbt.raygen_offset, raygen_rec->export_name, DxrShaderRecordKind::RayGen,
              raygen_rec->xbox_shader_id, raygen_rec->local_root_args);

    for (size_t i = 0; i < miss_recs.size(); ++i) {
        const uint64_t off = out_sbt.miss_offset + i * miss_stride;
        write_rec(off, miss_recs[i]->export_name, DxrShaderRecordKind::Miss,
                  miss_recs[i]->xbox_shader_id, miss_recs[i]->local_root_args);
    }

    for (size_t i = 0; i < hg_accums.size(); ++i) {
        const uint64_t off = out_sbt.hit_group_offset + i * hg_stride;
        const auto pc_id = write_rec(off, hg_accums[i].name, DxrShaderRecordKind::ClosestHit,
                                     hg_accums[i].xbox_id, hg_accums[i].local_args);
        DxrHitGroupBinding b{};
        b.hit_group_name = hg_accums[i].name;
        b.hit_group_index = static_cast<uint32_t>(i);
        b.closest_hit_export = hg_accums[i].ch;
        b.any_hit_export = hg_accums[i].ah;
        b.intersection_export = hg_accums[i].is;
        b.pc_shader_id = pc_id;
        b.record_offset_bytes = off;
        b.record_stride_bytes = hg_stride;
        b.record_gpu_va = base_gpu_va + off;
        out_sbt.hit_groups.push_back(std::move(b));
    }

    for (size_t i = 0; i < callable_recs.size(); ++i) {
        const uint64_t off = out_sbt.callable_offset + i * call_stride;
        write_rec(off, callable_recs[i]->export_name, DxrShaderRecordKind::Callable,
                  callable_recs[i]->xbox_shader_id, callable_recs[i]->local_root_args);
    }

    // Populate D3D12_DISPATCH_RAYS_DESC mirror
    out_sbt.dispatch_rays_desc.RayGenerationShaderRecord = {base_gpu_va + out_sbt.raygen_offset, rg_stride};
    if (!miss_recs.empty()) {
        out_sbt.dispatch_rays_desc.MissShaderTable = {
            base_gpu_va + out_sbt.miss_offset, miss_stride * miss_recs.size(), miss_stride};
    }
    if (!hg_accums.empty()) {
        out_sbt.dispatch_rays_desc.HitGroupTable = {
            base_gpu_va + out_sbt.hit_group_offset, hg_stride * hg_accums.size(), hg_stride};
    }
    if (!callable_recs.empty()) {
        out_sbt.dispatch_rays_desc.CallableShaderTable = {
            base_gpu_va + out_sbt.callable_offset, call_stride * callable_recs.size(), call_stride};
    }
    out_sbt.dispatch_rays_desc.Width = std::max(1u, dispatch_width);
    out_sbt.dispatch_rays_desc.Height = std::max(1u, dispatch_height);
    out_sbt.dispatch_rays_desc.Depth = std::max(1u, dispatch_depth);
    return Status::Ok();
}

Status DxrSbtMapper::PatchShaderIdentifierInPlace(
    MappedDxrSbt& sbt,
    const std::string& symbol_name,
    const std::array<uint8_t, kDxrShaderIdentifierSize>& new_pc_shader_id) const {
    uint32_t patched = 0u;
    for (auto& pe : sbt.patch_entries) {
        if (pe.symbol_name == symbol_name) {
            if (pe.buffer_offset_bytes + kDxrShaderIdentifierSize > sbt.buffer.size()) {
                return Status::Error(StatusCode::RaytracingError, "SBT patch offset out of buffer bounds");
            }
            std::memcpy(sbt.buffer.data() + pe.buffer_offset_bytes, new_pc_shader_id.data(),
                        kDxrShaderIdentifierSize);
            pe.pc_id = new_pc_shader_id;
            ++patched;
        }
    }
    for (auto& hg : sbt.hit_groups) {
        if (hg.hit_group_name == symbol_name) {
            hg.pc_shader_id = new_pc_shader_id;
        }
    }
    if (patched == 0u) {
        return Status::Error(StatusCode::NotFound, "Symbol not found in mapped DXR SBT: " + symbol_name);
    }
    return Status::Ok();
}

Status DxrSbtMapper::LookupHitGroup(
    const MappedDxrSbt& sbt,
    uint32_t instance_contribution,
    uint32_t geometry_index,
    uint32_t ray_contribution,
    uint32_t geometry_stride,
    DxrHitGroupBinding& out_binding) const {
    const uint64_t idx = static_cast<uint64_t>(ray_contribution) +
                         static_cast<uint64_t>(geometry_stride) * static_cast<uint64_t>(geometry_index) +
                         static_cast<uint64_t>(instance_contribution);
    if (idx >= sbt.hit_groups.size()) {
        return Status::Error(StatusCode::NotFound, "DXR hit group index out of SBT range");
    }
    out_binding = sbt.hit_groups[static_cast<size_t>(idx)];
    return Status::Ok();
}

Status DxrSbtMapper::LookupHitGroupByName(
    const MappedDxrSbt& sbt,
    const std::string& hit_group_name,
    DxrHitGroupBinding& out_binding) const {
    for (const auto& hg : sbt.hit_groups) {
        if (hg.hit_group_name == hit_group_name) {
            out_binding = hg;
            return Status::Ok();
        }
    }
    return Status::Error(StatusCode::NotFound, "DXR hit group not found: " + hit_group_name);
}

// ===========================================================================
// 3. Exact Cross-Half Wave64-on-Wave32 Emulation (Wave64On32Emulator)
// ===========================================================================

Wave64OpTestOutput Wave64On32Emulator::ExecuteCpuReference(const Wave64OpTestInput& in) noexcept {
    Wave64OpTestOutput out{};
    const uint32_t wqm_lo = Wqm32(in.ballot_mask_lo);
    const uint32_t wqm_hi = Wqm32(in.ballot_mask_hi);

    for (uint32_t lane = 0u; lane < 64u; ++lane) {
        const uint32_t row_lane = lane & 15u;
        const uint32_t sel = (row_lane < 8u)
                                 ? ((in.permlane_sel_lo >> (row_lane * 4u)) & 0xFu)
                                 : ((in.permlane_sel_hi >> ((row_lane - 8u) * 4u)) & 0xFu);

        // v_permlane16_b32: within same 16-lane row (0..15, 16..31, 32..47, 48..63)
        const uint32_t perm_src = (lane & ~15u) | sel;
        out.permlane16[lane] = in.lane_values[perm_src];

        // v_permlanex16_b32: cross paired 16-lane rows (0..15 <-> 16..31, 32..47 <-> 48..63)
        const uint32_t permx_src = ((lane & ~15u) ^ 16u) | sel;
        out.permlanex16[lane] = in.lane_values[permx_src];

        // v_readlane_b32: arbitrary lane 0..63 (including cross-half 0..31 <-> 32..63!)
        out.readlane[lane] = in.lane_values[in.readlane_targets[lane] & 63u];

        // s_wqm_b64: whole-quad mode across all 16 quads
        out.wqm_lo[lane] = wqm_lo;
        out.wqm_hi[lane] = wqm_hi;

        // v_mbcnt_lo_u32_b32 + v_mbcnt_hi_u32_b32: 64-bit ballot exclusive prefix count
        const uint32_t lo_mask = (lane < 32u) ? ((1u << lane) - 1u) : 0xFFFFFFFFu;
        const uint32_t hi_mask = (lane >= 32u) ? ((1u << (lane - 32u)) - 1u) : 0u;
        const uint32_t lo_cnt = Popcount32(in.ballot_mask_lo & lo_mask);
        const uint32_t hi_cnt = Popcount32(in.ballot_mask_hi & hi_mask);
        out.mbcnt_lo[lane] = lo_cnt;
        out.mbcnt_total[lane] = lo_cnt + hi_cnt;
    }
    return out;
}

std::string Wave64On32Emulator::EmitHlslHelperLibrary(uint32_t max_guest_waves_per_group) {
    const uint32_t waves = std::max(1u, max_guest_waves_per_group);
    std::ostringstream oss;
    oss << "// Exact Cross-Half Wave64-on-Wave32 Emulation Library (OpenScarlett)\n";
    oss << "groupshared uint g_w64_lds[" << (waves * 64u) << "];\n";
    oss << "groupshared uint g_w64_ballot[" << (waves * 2u) << "];\n\n";
    oss << R"HLSL(uint W64_PermLane16(uint group_idx, uint src0, uint sel_lo, uint sel_hi) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint base = gw * 64u;
    g_w64_lds[base + lane] = src0;
    GroupMemoryBarrierWithGroupSync();
    uint rl = lane & 15u;
    uint sel = (rl < 8u) ? ((sel_lo >> (rl * 4u)) & 0xFu) : ((sel_hi >> ((rl - 8u) * 4u)) & 0xFu);
    uint res = g_w64_lds[base + ((lane & ~15u) | sel)];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

uint W64_PermLaneX16(uint group_idx, uint src0, uint sel_lo, uint sel_hi) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint base = gw * 64u;
    g_w64_lds[base + lane] = src0;
    GroupMemoryBarrierWithGroupSync();
    uint rl = lane & 15u;
    uint sel = (rl < 8u) ? ((sel_lo >> (rl * 4u)) & 0xFu) : ((sel_hi >> ((rl - 8u) * 4u)) & 0xFu);
    uint res = g_w64_lds[base + (((lane & ~15u) ^ 16u) | sel)];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

uint W64_ReadLane(uint group_idx, uint src0, uint target_lane) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint base = gw * 64u;
    g_w64_lds[base + lane] = src0;
    GroupMemoryBarrierWithGroupSync();
    uint res = g_w64_lds[base + (target_lane & 63u)];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

uint2 W64_Ballot64(uint group_idx, bool pred) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint b0 = gw * 2u + 0u;
    uint b1 = gw * 2u + 1u;
    GroupMemoryBarrierWithGroupSync();
    InterlockedAnd(g_w64_ballot[b0], 0u);
    InterlockedAnd(g_w64_ballot[b1], 0u);
    GroupMemoryBarrierWithGroupSync();
    if (pred) {
        uint half_i = lane >> 5u;
        InterlockedOr(g_w64_ballot[gw * 2u + half_i], 1u << (lane & 31u));
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 b = uint2(g_w64_ballot[b0], g_w64_ballot[b1]);
    GroupMemoryBarrierWithGroupSync();
    return b;
}

uint W64_Wqm32(uint m) {
    uint q = m | ((m & 0x55555555u) << 1u) | ((m & 0xAAAAAAAAu) >> 1u);
    q = q | ((q & 0x33333333u) << 2u) | ((q & 0xCCCCCCCCu) >> 2u);
    return q;
}

uint2 W64_Wqm64(uint2 mask64) {
    return uint2(W64_Wqm32(mask64.x), W64_Wqm32(mask64.y));
}

uint W64_MbcntLo(uint group_idx, uint mask_lo, uint acc) {
    uint lane = group_idx & 63u;
    uint lo_mask = (lane < 32u) ? ((1u << lane) - 1u) : 0xFFFFFFFFu;
    return acc + countbits(mask_lo & lo_mask);
}

uint W64_MbcntHi(uint group_idx, uint mask_hi, uint acc) {
    uint lane = group_idx & 63u;
    uint hi_mask = (lane >= 32u) ? ((1u << (lane - 32u)) - 1u) : 0u;
    return acc + countbits(mask_hi & hi_mask);
}

uint W64_ActiveCountBits64(uint2 ballot_mask) {
    return countbits(ballot_mask.x) + countbits(ballot_mask.y);
}

uint W64_Mbcnt64(uint group_idx, uint2 ballot_mask, uint acc) {
    return W64_MbcntHi(group_idx, ballot_mask.y, W64_MbcntLo(group_idx, ballot_mask.x, acc));
}

uint W64_ReadFirstLane(uint group_idx, uint src0) {
    uint2 active = W64_Ballot64(group_idx, true);
    uint first_lane = (active.x != 0u) ? firstbitlow(active.x)
                                       : ((active.y != 0u) ? (32u + firstbitlow(active.y)) : 0u);
    return W64_ReadLane(group_idx, src0, first_lane);
}

uint W64_CrossHalfExchange(uint group_idx, uint src0) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint base = gw * 64u;
    g_w64_lds[base + lane] = src0;
    GroupMemoryBarrierWithGroupSync();
    uint res = g_w64_lds[base + (lane ^ 32u)];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

uint W64_CrossHalfShuffle(uint group_idx, uint src0, uint xor_mask) {
    uint gw = group_idx >> 6u;
    uint lane = group_idx & 63u;
    uint base = gw * 64u;
    g_w64_lds[base + lane] = src0;
    GroupMemoryBarrierWithGroupSync();
    uint res = g_w64_lds[base + ((lane ^ xor_mask) & 63u)];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

// Canonical LDS-backed Wave64-on-Wave32 helper aliases
uint2 ballot64(uint group_idx, bool pred) {
    return W64_Ballot64(group_idx, pred);
}

uint mbcnt64_lo(uint group_idx, uint mask_lo, uint acc) {
    return W64_MbcntLo(group_idx, mask_lo, acc);
}

uint mbcnt64_hi(uint group_idx, uint mask_hi, uint acc) {
    return W64_MbcntHi(group_idx, mask_hi, acc);
}

uint readlane64(uint group_idx, uint src0, uint target_lane) {
    return W64_ReadLane(group_idx, src0, target_lane);
}

uint cross_half_exchange64(uint group_idx, uint src0) {
    return W64_CrossHalfExchange(group_idx, src0);
}
)HLSL";
    return oss.str();
}

std::string Wave64On32Emulator::EmitTestComputeHlsl() {
    std::ostringstream oss;
    oss << EmitHlslHelperLibrary(1u) << "\n";
    oss << R"HLSL(// Input layout: [0..63]=lane_values, [64..127]=readlane_targets,
//               [128]=sel_lo, [129]=sel_hi, [130]=ballot_lo, [131]=ballot_hi
// Output layout: 7 * 64 = 448 uints:
//   [0..63]=permlane16, [64..127]=permlanex16, [128..191]=readlane,
//   [192..255]=wqm_lo, [256..319]=wqm_hi, [320..383]=mbcnt_lo, [384..447]=mbcnt_total
ByteAddressBuffer g_In : register(t0);
RWByteAddressBuffer g_Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint gidx : SV_GroupIndex) {
    uint val = g_In.Load(gidx * 4u);
    uint rl_target = g_In.Load((64u + gidx) * 4u);
    uint sel_lo = g_In.Load(128u * 4u);
    uint sel_hi = g_In.Load(129u * 4u);
    uint in_mask_lo = g_In.Load(130u * 4u);
    uint in_mask_hi = g_In.Load(131u * 4u);

    uint p16  = W64_PermLane16(gidx, val, sel_lo, sel_hi);
    uint px16 = W64_PermLaneX16(gidx, val, sel_lo, sel_hi);
    uint rl   = W64_ReadLane(gidx, val, rl_target);

    // Build live cross-half 64-bit ballot via LDS atomic OR across both Wave32 halves!
    bool lane_pred = (gidx < 32u) ? (((in_mask_lo >> gidx) & 1u) != 0u)
                                  : (((in_mask_hi >> (gidx - 32u)) & 1u) != 0u);
    uint2 ballot = W64_Ballot64(gidx, lane_pred);
    uint2 wqm = W64_Wqm64(ballot);
    uint mb_lo = W64_MbcntLo(gidx, ballot.x, 0u);
    uint mb_tot = W64_MbcntHi(gidx, ballot.y, mb_lo);

    g_Out.Store((0u   + gidx) * 4u, p16);
    g_Out.Store((64u  + gidx) * 4u, px16);
    g_Out.Store((128u + gidx) * 4u, rl);
    g_Out.Store((192u + gidx) * 4u, wqm.x);
    g_Out.Store((256u + gidx) * 4u, wqm.y);
    g_Out.Store((320u + gidx) * 4u, mb_lo);
    g_Out.Store((384u + gidx) * 4u, mb_tot);
}
)HLSL";
    return oss.str();
}

Status Wave64On32Emulator::EmitTestComputeSpirv(std::vector<uint32_t>& out_spirv) {
    uint32_t id = 1u;
    auto nid = [&]() { return id++; };

    std::vector<uint32_t> decs, types, body;
    std::unordered_map<uint32_t, uint32_t> uconsts;

    const uint32_t ty_void = nid(), ty_bool = nid(), ty_u32 = nid();
    const uint32_t ty_rt_u32 = nid(), ty_buf = nid();
    const uint32_t ty_ptr_sb_buf = nid(), ty_ptr_sb_u32 = nid();
    const uint32_t ty_ptr_in_u32 = nid(), ty_ptr_wg_u32 = nid(), ty_fn = nid();

    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeBool, {ty_bool});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeRuntimeArray, {ty_rt_u32, ty_u32});
    EmitSpv(types, SpvOpTypeStruct, {ty_buf, ty_rt_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_sb_buf, 12u, ty_buf});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_sb_u32, 12u, ty_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_u32, 1u, ty_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_u32, 4u, ty_u32});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    auto cu = [&](uint32_t v) -> uint32_t {
        auto it = uconsts.find(v);
        if (it != uconsts.end()) return it->second;
        const uint32_t cid = nid();
        EmitSpv(types, SpvOpConstant, {ty_u32, cid, v});
        uconsts[v] = cid;
        return cid;
    };

    const uint32_t ty_arr64 = nid(), ty_arr2 = nid();
    const uint32_t ty_ptr_wg_arr64 = nid(), ty_ptr_wg_arr2 = nid();
    EmitSpv(types, SpvOpTypeArray, {ty_arr64, ty_u32, cu(64u)});
    EmitSpv(types, SpvOpTypeArray, {ty_arr2, ty_u32, cu(2u)});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_arr64, 4u, ty_arr64});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_arr2, 4u, ty_arr2});

    const uint32_t var_in = nid(), var_out = nid(), var_lid = nid();
    const uint32_t var_lds = nid(), var_bal = nid();
    EmitSpv(types, SpvOpVariable, {ty_ptr_sb_buf, var_in, 12u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_sb_buf, var_out, 12u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_in_u32, var_lid, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_wg_arr64, var_lds, 4u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_wg_arr2, var_bal, 4u});

    EmitSpv(decs, SpvOpDecorate, {ty_rt_u32, 6u, 4u});
    EmitSpv(decs, SpvOpDecorate, {ty_buf, 2u});
    EmitSpv(decs, SpvOpMemberDecorate, {ty_buf, 0u, 35u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 34u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 33u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 24u});
    EmitSpv(decs, SpvOpDecorate, {var_out, 34u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_out, 33u, 1u});
    EmitSpv(decs, SpvOpDecorate, {var_lid, 11u, 29u});  // BuiltIn LocalInvocationIndex

    auto bin = [&](uint16_t op, uint32_t ty, uint32_t a, uint32_t b) {
        const uint32_t r = nid();
        EmitSpv(body, op, {ty, r, a, b});
        return r;
    };
    auto sel = [&](uint32_t c, uint32_t t, uint32_t f) {
        const uint32_t r = nid();
        EmitSpv(body, SpvOpSelect, {ty_u32, r, c, t, f});
        return r;
    };
    auto load_in = [&](uint32_t idx) {
        const uint32_t p = nid(), v = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_sb_u32, p, var_in, cu(0u), idx});
        EmitSpv(body, SpvOpLoad, {ty_u32, v, p});
        return v;
    };
    auto store_out = [&](uint32_t idx, uint32_t val) {
        const uint32_t p = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_sb_u32, p, var_out, cu(0u), idx});
        EmitSpv(body, SpvOpStore, {p, val});
    };
    auto wg_barrier = [&]() {
        // Execution=Workgroup(2), Memory=Workgroup(2), Semantics=AcquireRelease|WorkgroupMemory(0x108)
        EmitSpv(body, SpvOpControlBarrier, {cu(2u), cu(2u), cu(0x108u)});
    };

    const uint32_t fn_main = nid(), lbl = nid();
    EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
    EmitSpv(body, SpvOpLabel, {lbl});

    const uint32_t lane = nid();
    EmitSpv(body, SpvOpLoad, {ty_u32, lane, var_lid});

    const uint32_t val = load_in(lane);
    const uint32_t rl_tgt = load_in(bin(SpvOpIAdd, ty_u32, lane, cu(64u)));
    const uint32_t sel_lo = load_in(cu(128u));
    const uint32_t sel_hi = load_in(cu(129u));
    const uint32_t in_mlo = load_in(cu(130u));
    const uint32_t in_mhi = load_in(cu(131u));

    // Store all 64 lanes into Workgroup LDS + explicit barrier
    const uint32_t p_lds_self = nid();
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_lds_self, var_lds, lane});
    EmitSpv(body, SpvOpStore, {p_lds_self, val});
    wg_barrier();

    // Decode 4-bit selector for permlane16 / permlanex16
    const uint32_t rl = bin(SpvOpBitwiseAnd, ty_u32, lane, cu(15u));
    const uint32_t rl_lt8 = bin(SpvOpULessThan, ty_bool, rl, cu(8u));
    const uint32_t sh_lo = bin(SpvOpShiftLeftLogical, ty_u32, rl, cu(2u));
    const uint32_t sh_hi = bin(SpvOpShiftLeftLogical, ty_u32, bin(SpvOpISub, ty_u32, rl, cu(8u)), cu(2u));
    const uint32_t s_val = bin(
        SpvOpBitwiseAnd, ty_u32,
        sel(rl_lt8,
            bin(SpvOpShiftRightLogical, ty_u32, sel_lo, sh_lo),
            bin(SpvOpShiftRightLogical, ty_u32, sel_hi, sh_hi)),
        cu(0xFu));
    const uint32_t row_base = bin(SpvOpBitwiseAnd, ty_u32, lane, cu(0xFFFFFFF0u));
    const uint32_t idx_p16 = bin(SpvOpBitwiseOr, ty_u32, row_base, s_val);
    const uint32_t idx_px16 = bin(SpvOpBitwiseOr, ty_u32, bin(SpvOpBitwiseXor, ty_u32, row_base, cu(16u)), s_val);
    const uint32_t idx_rl = bin(SpvOpBitwiseAnd, ty_u32, rl_tgt, cu(63u));

    auto load_lds = [&](uint32_t idx) {
        const uint32_t p = nid(), v = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p, var_lds, idx});
        EmitSpv(body, SpvOpLoad, {ty_u32, v, p});
        return v;
    };
    const uint32_t r_p16 = load_lds(idx_p16);
    const uint32_t r_px16 = load_lds(idx_px16);
    const uint32_t r_rl = load_lds(idx_rl);

    // Cross-half 64-bit ballot via LDS AtomicOr
    const uint32_t is_lane0 = bin(SpvOpIEqual, ty_bool, lane, cu(0u));
    const uint32_t l_z = nid(), l_zm = nid();
    EmitSpv(body, SpvOpSelectionMerge, {l_zm, 0u});
    EmitSpv(body, SpvOpBranchConditional, {is_lane0, l_z, l_zm});
    EmitSpv(body, SpvOpLabel, {l_z});
    {
        const uint32_t p0 = nid(), p1 = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p0, var_bal, cu(0u)});
        EmitSpv(body, SpvOpStore, {p0, cu(0u)});
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p1, var_bal, cu(1u)});
        EmitSpv(body, SpvOpStore, {p1, cu(0u)});
    }
    EmitSpv(body, SpvOpBranch, {l_zm});
    EmitSpv(body, SpvOpLabel, {l_zm});
    wg_barrier();

    const uint32_t lt32 = bin(SpvOpULessThan, ty_bool, lane, cu(32u));
    const uint32_t half_i = bin(SpvOpShiftRightLogical, ty_u32, lane, cu(5u));
    const uint32_t sub_lane = bin(SpvOpBitwiseAnd, ty_u32, lane, cu(31u));
    const uint32_t mask_w = sel(lt32, in_mlo, in_mhi);
    const uint32_t bit_set = bin(
        SpvOpINotEqual, ty_bool,
        bin(SpvOpBitwiseAnd, ty_u32, bin(SpvOpShiftRightLogical, ty_u32, mask_w, sub_lane), cu(1u)),
        cu(0u));
    const uint32_t l_or = nid(), l_orm = nid();
    EmitSpv(body, SpvOpSelectionMerge, {l_orm, 0u});
    EmitSpv(body, SpvOpBranchConditional, {bit_set, l_or, l_orm});
    EmitSpv(body, SpvOpLabel, {l_or});
    {
        const uint32_t pb = nid(), old = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, pb, var_bal, half_i});
        EmitSpv(body, SpvOpAtomicOr,
                {ty_u32, old, pb, cu(2u), cu(0u), bin(SpvOpShiftLeftLogical, ty_u32, cu(1u), sub_lane)});
    }
    EmitSpv(body, SpvOpBranch, {l_orm});
    EmitSpv(body, SpvOpLabel, {l_orm});
    wg_barrier();

    const uint32_t pb0 = nid(), pb1 = nid(), bal_lo = nid(), bal_hi = nid();
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, pb0, var_bal, cu(0u)});
    EmitSpv(body, SpvOpLoad, {ty_u32, bal_lo, pb0});
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, pb1, var_bal, cu(1u)});
    EmitSpv(body, SpvOpLoad, {ty_u32, bal_hi, pb1});

    auto emit_wqm32 = [&](uint32_t m) {
        const uint32_t s1 = bin(
            SpvOpBitwiseOr, ty_u32,
            bin(SpvOpBitwiseOr, ty_u32, m,
                bin(SpvOpShiftLeftLogical, ty_u32, bin(SpvOpBitwiseAnd, ty_u32, m, cu(0x55555555u)), cu(1u))),
            bin(SpvOpShiftRightLogical, ty_u32, bin(SpvOpBitwiseAnd, ty_u32, m, cu(0xAAAAAAAAu)), cu(1u)));
        return bin(
            SpvOpBitwiseOr, ty_u32,
            bin(SpvOpBitwiseOr, ty_u32, s1,
                bin(SpvOpShiftLeftLogical, ty_u32, bin(SpvOpBitwiseAnd, ty_u32, s1, cu(0x33333333u)), cu(2u))),
            bin(SpvOpShiftRightLogical, ty_u32, bin(SpvOpBitwiseAnd, ty_u32, s1, cu(0xCCCCCCCCu)), cu(2u)));
    };
    const uint32_t wqm_lo = emit_wqm32(bal_lo);
    const uint32_t wqm_hi = emit_wqm32(bal_hi);

    // v_mbcnt_lo + v_mbcnt_hi
    const uint32_t sub_mask = bin(SpvOpISub, ty_u32, bin(SpvOpShiftLeftLogical, ty_u32, cu(1u), sub_lane), cu(1u));
    const uint32_t lo_mask = sel(lt32, sub_mask, cu(0xFFFFFFFFu));
    const uint32_t hi_mask = sel(lt32, cu(0u), sub_mask);
    const uint32_t mb_lo = nid(), mb_hi_part = nid();
    EmitSpv(body, SpvOpBitCount, {ty_u32, mb_lo, bin(SpvOpBitwiseAnd, ty_u32, bal_lo, lo_mask)});
    EmitSpv(body, SpvOpBitCount, {ty_u32, mb_hi_part, bin(SpvOpBitwiseAnd, ty_u32, bal_hi, hi_mask)});
    const uint32_t mb_tot = bin(SpvOpIAdd, ty_u32, mb_lo, mb_hi_part);

    store_out(lane, r_p16);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(64u)), r_px16);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(128u)), r_rl);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(192u)), wqm_lo);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(256u)), wqm_hi);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(320u)), mb_lo);
    store_out(bin(SpvOpIAdd, ty_u32, lane, cu(384u)), mb_tot);

    EmitSpv(body, SpvOpReturn, {});
    EmitSpv(body, SpvOpFunctionEnd, {});

    out_spirv = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
    EmitSpv(out_spirv, SpvOpCapability, {1u});
    EmitSpv(out_spirv, SpvOpMemoryModel, {0u, 1u});
    EmitSpv(out_spirv, SpvOpEntryPoint, {5u, fn_main, 0x6E69616Du, 0u, var_lid});
    EmitSpv(out_spirv, SpvOpExecutionMode, {fn_main, 17u, 64u, 1u, 1u});
    out_spirv.insert(out_spirv.end(), decs.begin(), decs.end());
    out_spirv.insert(out_spirv.end(), types.begin(), types.end());
    out_spirv.insert(out_spirv.end(), body.begin(), body.end());
    return ValidateSpirvBinary(out_spirv);
}

#if defined(_WIN32)
Status Wave64On32Emulator::ExecuteTestOnD3D12(
    const Wave64OpTestInput& input,
    Wave64OpTestOutput& out_output,
    bool prefer_warp,
    Wave64GpuRunInfo* out_info,
    const std::string& dxc_path) {
    out_output = Wave64OpTestOutput{};
    LocalComPtr<ID3D12Device> dev;
    D3D12AdapterInfo ainfo{};
    Status st = CreateD3D12DeviceLocal(prefer_warp, dev, ainfo);
    if (!st.ok()) return st;

    const std::string target = (!prefer_warp && ainfo.highest_shader_model >= 0x65u) ? "cs_6_5" : "cs_6_2";
    std::vector<uint8_t> dxil;
    std::string diag;
    st = CompileHlslWithDxc(EmitTestComputeHlsl(), target, "main", dxil, diag, dxc_path);
    if (!st.ok()) return Status::Error(StatusCode::ShaderError, "Wave64 DXC compile failed: " + diag);
    st = ResignDxilContainer(dxil);
    if (!st.ok()) return st;

    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;

    auto ser_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    if (!ser_fn) return Status::Error(StatusCode::DeviceError, "D3D12SerializeRootSignature missing");

    LocalComPtr<ID3DBlob> sig_blob, err_blob;
    if (FAILED(ser_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put())) || !sig_blob) {
        return Status::Error(StatusCode::DeviceError, "D3D12SerializeRootSignature failed");
    }

    LocalComPtr<ID3D12RootSignature> root_sig;
    if (FAILED(dev->CreateRootSignature(
            0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
            OS_UUIDOF(ID3D12RootSignature), root_sig.PutVoid())) || !root_sig) {
        return Status::Error(StatusCode::DeviceError, "CreateRootSignature failed");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig.Get();
    pd.CS = {dxil.data(), dxil.size()};
    LocalComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, OS_UUIDOF(ID3D12PipelineState), pso.PutVoid())) || !pso) {
        return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(Wave64) failed");
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    LocalComPtr<ID3D12CommandQueue> queue;
    LocalComPtr<ID3D12CommandAllocator> alloc;
    LocalComPtr<ID3D12GraphicsCommandList> cmd;
    LocalComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue), queue.PutVoid())) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, OS_UUIDOF(ID3D12CommandAllocator), alloc.PutVoid())) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), OS_UUIDOF(ID3D12GraphicsCommandList), cmd.PutVoid())) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence), fence.PutVoid()))) {
        return Status::Error(StatusCode::DeviceError, "D3D12 queue/list/fence setup failed");
    }

    std::vector<uint32_t> in_u32(132u, 0u);
    std::memcpy(in_u32.data() + 0u, input.lane_values.data(), 64u * sizeof(uint32_t));
    std::memcpy(in_u32.data() + 64u, input.readlane_targets.data(), 64u * sizeof(uint32_t));
    in_u32[128] = input.permlane_sel_lo;
    in_u32[129] = input.permlane_sel_hi;
    in_u32[130] = input.ballot_mask_lo;
    in_u32[131] = input.ballot_mask_hi;

    const uint64_t in_bytes = AlignUpU64(in_u32.size() * sizeof(uint32_t), 256ull);
    const uint64_t out_bytes = AlignUpU64(448u * sizeof(uint32_t), 256ull);

    LocalComPtr<ID3D12Resource> in_up, in_gpu, out_gpu, out_rb;
    if (auto s = CreateD3D12BufferLocal(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, in_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, in_up); !s.ok()) return s;
    if (auto s = CreateD3D12BufferLocal(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, in_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, in_gpu); !s.ok()) return s;
    if (auto s = CreateD3D12BufferLocal(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, out_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out_gpu); !s.ok()) return s;
    if (auto s = CreateD3D12BufferLocal(dev.Get(), D3D12_HEAP_TYPE_READBACK, out_bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, out_rb); !s.ok()) return s;

    void* mapped = nullptr;
    D3D12_RANGE z{0, 0};
    if (FAILED(in_up->Map(0, &z, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(Wave64 input) failed");
    }
    std::memcpy(mapped, in_u32.data(), in_u32.size() * sizeof(uint32_t));
    in_up->Unmap(0, nullptr);

    cmd->CopyBufferRegion(in_gpu.Get(), 0, in_up.Get(), 0, in_bytes);
    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    cmd->ResourceBarrier(1, &b1);

    cmd->SetComputeRootSignature(root_sig.Get());
    cmd->SetPipelineState(pso.Get());
    cmd->SetComputeRootShaderResourceView(0, in_gpu->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
    cmd->Dispatch(1u, 1u, 1u);

    D3D12_RESOURCE_BARRIER b2[2]{};
    b2[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b2[0].UAV.pResource = out_gpu.Get();
    b2[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cmd->ResourceBarrier(2, b2);
    cmd->CopyBufferRegion(out_rb.Get(), 0, out_gpu.Get(), 0, out_bytes);
    cmd->Close();

    ID3D12CommandList* lists[] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1ull);
    if (fence->GetCompletedValue() < 1ull) {
        fence->SetEventOnCompletion(1ull, ev);
        WaitForSingleObject(ev, 15000);
    }
    if (ev) CloseHandle(ev);

    D3D12_RANGE rr{0, 448u * sizeof(uint32_t)};
    if (FAILED(out_rb->Map(0, &rr, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(Wave64 readback) failed");
    }
    const auto* out_u32 = static_cast<const uint32_t*>(mapped);
    std::memcpy(out_output.permlane16.data(),  out_u32 + 0u,   64u * sizeof(uint32_t));
    std::memcpy(out_output.permlanex16.data(), out_u32 + 64u,  64u * sizeof(uint32_t));
    std::memcpy(out_output.readlane.data(),    out_u32 + 128u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.wqm_lo.data(),      out_u32 + 192u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.wqm_hi.data(),      out_u32 + 256u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.mbcnt_lo.data(),    out_u32 + 320u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.mbcnt_total.data(), out_u32 + 384u, 64u * sizeof(uint32_t));
    out_rb->Unmap(0, &z);

    if (out_info) {
        out_info->backend_name = prefer_warp ? "D3D12-WARP" : "D3D12-HW";
        out_info->adapter_name = ainfo.description;
        out_info->shader_target = target;
        out_info->hw_wave_lane_min = ainfo.wave_lane_min;
        out_info->hw_wave_lane_max = ainfo.wave_lane_max;
        out_info->executed_on_hardware_gpu = !prefer_warp;
        out_info->used_warp = prefer_warp;
    }
    return Status::Ok();
}
#endif  // _WIN32

Status Wave64On32Emulator::ExecuteTestOnVulkan(
    const Wave64OpTestInput& input,
    Wave64OpTestOutput& out_output,
    bool allow_software_fallback,
    Wave64GpuRunInfo* out_info) {
    out_output = Wave64OpTestOutput{};
    std::vector<uint32_t> spv;
    Status st = EmitTestComputeSpirv(spv);
    if (!st.ok()) return st;

    VulkanBackend vk;
    st = vk.Initialize(allow_software_fallback);
    if (!st.ok()) return st;
    if (vk.is_fallback_mode()) {
        return Status::Error(StatusCode::NotImplemented, "Vulkan HW ICD required for Wave64 SPIR-V execution");
    }

    // Pad input_u32 to 448 elements so VulkanBackend sizes output SSBO to >= 448 uints.
    std::vector<uint32_t> in_u32(448u, 0u);
    std::memcpy(in_u32.data() + 0u, input.lane_values.data(), 64u * sizeof(uint32_t));
    std::memcpy(in_u32.data() + 64u, input.readlane_targets.data(), 64u * sizeof(uint32_t));
    in_u32[128] = input.permlane_sel_lo;
    in_u32[129] = input.permlane_sel_hi;
    in_u32[130] = input.ballot_mask_lo;
    in_u32[131] = input.ballot_mask_hi;

    std::vector<uint32_t> out_u32;
    VulkanComputeDispatchOptions opts{};
    VulkanComputeDispatchResult res{};
    st = vk.ExecuteComputeShaderEx(spv, in_u32, out_u32, 1u, opts, res);
    if (!st.ok()) return st;
    if (out_u32.size() < 448u) {
        return Status::Error(StatusCode::DeviceError, "Vulkan Wave64 output too small");
    }

    std::memcpy(out_output.permlane16.data(),  out_u32.data() + 0u,   64u * sizeof(uint32_t));
    std::memcpy(out_output.permlanex16.data(), out_u32.data() + 64u,  64u * sizeof(uint32_t));
    std::memcpy(out_output.readlane.data(),    out_u32.data() + 128u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.wqm_lo.data(),      out_u32.data() + 192u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.wqm_hi.data(),      out_u32.data() + 256u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.mbcnt_lo.data(),    out_u32.data() + 320u, 64u * sizeof(uint32_t));
    std::memcpy(out_output.mbcnt_total.data(), out_u32.data() + 384u, 64u * sizeof(uint32_t));

    if (out_info) {
        out_info->backend_name = "Vulkan-SPIRV";
        out_info->adapter_name = vk.GetDeviceInfo().device_name;
        out_info->shader_target = "spirv_1_3";
        out_info->hw_wave_lane_min = vk.GetDeviceInfo().subgroup_size;
        out_info->hw_wave_lane_max = vk.GetDeviceInfo().subgroup_size;
        out_info->executed_on_hardware_gpu = res.executed_on_vulkan_icd;
        out_info->used_warp = false;
    }
    return Status::Ok();
}

// ===========================================================================
// 4. Async Background PSO Pre-Compilation Worker Pool (AsyncPsoCompilePool)
// ===========================================================================

struct AsyncPsoCompilePool::Impl {
    struct Waiter {
        std::promise<AsyncPsoJobResult> promise;
        std::function<void(const AsyncPsoJobResult&)> callback;
    };

    struct InFlightJob {
        uint64_t content_hash = 0u;
        AsyncPsoJobDesc desc;
        PsoCompilePriority priority = PsoCompilePriority::Normal;
        uint64_t seq = 0u;
        bool started = false;
        bool cancelled = false;
        std::vector<Waiter> waiters;
    };

    struct QueueEntry {
        uint64_t content_hash = 0u;
        PsoCompilePriority priority = PsoCompilePriority::Normal;
        uint64_t seq = 0u;

        bool operator<(const QueueEntry& other) const noexcept {
            if (priority != other.priority) {
                return static_cast<uint8_t>(priority) < static_cast<uint8_t>(other.priority);
            }
            return seq > other.seq;  // Earlier sequence number wins tie
        }
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::priority_queue<QueueEntry> pq;
    std::unordered_map<uint64_t, std::shared_ptr<InFlightJob>> inflight;
    std::unordered_map<uint64_t, AsyncPsoJobResult> cache;
    std::vector<std::thread> workers;

    bool stopping = false;
    uint32_t active_workers = 0u;
    uint64_t next_seq = 1u;

    std::atomic<uint64_t> stat_submitted{0u};
    std::atomic<uint64_t> stat_queued{0u};
    std::atomic<uint64_t> stat_compiled{0u};
    std::atomic<uint64_t> stat_cache_hits{0u};
    std::atomic<uint64_t> stat_dedup{0u};
    std::atomic<uint64_t> stat_failed{0u};
    std::atomic<uint64_t> stat_cancelled{0u};

#if defined(_WIN32)
    LocalComPtr<ID3D12Device> d3d12_device;
    LocalComPtr<ID3D12RootSignature> generic_cs_root_sig;
    std::mutex pso_mutex;
    std::unordered_map<uint64_t, LocalComPtr<ID3D12PipelineState>> d3d12_psos;

    void InitGenericComputeRootSig() {
        if (!d3d12_device) return;
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[1].Descriptor.ShaderRegister = 0;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters = rp;

        auto ser_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
            ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
        if (!ser_fn) return;
        LocalComPtr<ID3DBlob> sig, err;
        if (SUCCEEDED(ser_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig.Put(), err.Put())) && sig) {
            d3d12_device->CreateRootSignature(
                0, sig->GetBufferPointer(), sig->GetBufferSize(),
                OS_UUIDOF(ID3D12RootSignature), generic_cs_root_sig.PutVoid());
        }
    }
#endif

    AsyncPsoJobResult CompileJob(const AsyncPsoJobDesc& desc, uint64_t hash) {
        const auto t0 = std::chrono::steady_clock::now();
        AsyncPsoJobResult res{};
        res.content_hash = hash;

        if (desc.shader_blob.empty()) {
            res.status = Status::Error(StatusCode::InvalidArgument, "Empty shader_blob");
        } else if (desc.is_hlsl_source) {
            res.hlsl_source.assign(reinterpret_cast<const char*>(desc.shader_blob.data()), desc.shader_blob.size());
            res.target_profile = desc.target_profile.empty() ? "cs_6_5" : desc.target_profile;
            if (desc.compile_dxil) {
                std::string diag;
                res.status = CompileHlslWithDxc(
                    res.hlsl_source, res.target_profile,
                    desc.hlsl_entry.empty() ? "main" : desc.hlsl_entry,
                    res.dxil, diag, desc.dxc_path);
                if (res.status.ok()) {
                    res.status = ResignDxilContainer(res.dxil);
                }
            }
        } else {
            Rdna2LiftOptions lopts{};
            lopts.emit_hlsl = true;
            lopts.emit_spirv = desc.emit_spirv;
            lopts.compile_dxil_with_dxc = desc.compile_dxil;
            lopts.resign_dxil = true;
            lopts.target_profile_override = desc.target_profile;
            lopts.dxc_path = desc.dxc_path;

            Rdna2LiftResult lr{};
            res.status = LiftRdna2Xs2Shader(desc.shader_blob, lr, lopts);
            if (res.status.ok()) {
                res.hlsl_source = std::move(lr.hlsl_source);
                res.target_profile = std::move(lr.hlsl_target);
                res.dxil = std::move(lr.pc_dxil);
                res.spirv = std::move(lr.spirv_words);
            }
        }

#if defined(_WIN32)
        if (res.status.ok() && desc.create_d3d12_pso && d3d12_device && generic_cs_root_sig && !res.dxil.empty()) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = generic_cs_root_sig.Get();
            pd.CS = {res.dxil.data(), res.dxil.size()};
            LocalComPtr<ID3D12PipelineState> pso;
            HRESULT hr = d3d12_device->CreateComputePipelineState(
                &pd, OS_UUIDOF(ID3D12PipelineState), pso.PutVoid());
            if (SUCCEEDED(hr) && pso) {
                std::lock_guard<std::mutex> plk(pso_mutex);
                d3d12_psos[hash] = std::move(pso);
                res.d3d12_pso_created = true;
            }
        }
#endif

        const auto t1 = std::chrono::steady_clock::now();
        res.compile_time_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        return res;
    }

    void WorkerLoop() {
        for (;;) {
            std::shared_ptr<InFlightJob> job;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&]() { return stopping || !pq.empty(); });
                if (stopping && pq.empty()) return;

                while (!pq.empty()) {
                    const QueueEntry qe = pq.top();
                    pq.pop();
                    auto it = inflight.find(qe.content_hash);
                    if (it == inflight.end()) continue;
                    if (it->second->started || it->second->cancelled) continue;
                    job = it->second;
                    job->started = true;
                    ++active_workers;
                    break;
                }
                if (!job) {
                    if (stopping) return;
                    continue;
                }
            }

            AsyncPsoJobResult compiled = CompileJob(job->desc, job->content_hash);
            if (compiled.status.ok()) {
                stat_compiled.fetch_add(1u, std::memory_order_relaxed);
            } else {
                stat_failed.fetch_add(1u, std::memory_order_relaxed);
            }

            std::vector<Waiter> waiters;
            {
                std::lock_guard<std::mutex> lk(mutex);
                if (compiled.status.ok()) {
                    cache[job->content_hash] = compiled;
                }
                waiters = std::move(job->waiters);
                inflight.erase(job->content_hash);
                --active_workers;
                if (pq.empty() && active_workers == 0u) {
                    idle_cv.notify_all();
                }
            }

            for (size_t i = 0; i < waiters.size(); ++i) {
                AsyncPsoJobResult r = compiled;
                if (i > 0u) r.cache_hit = true;
                if (waiters[i].callback) waiters[i].callback(r);
                waiters[i].promise.set_value(std::move(r));
            }
        }
    }
};

#if defined(_WIN32)
AsyncPsoCompilePool::AsyncPsoCompilePool(uint32_t worker_count, ID3D12Device* d3d12_device)
    : impl_(std::make_unique<Impl>()) {
    if (d3d12_device) {
        d3d12_device->AddRef();
        impl_->d3d12_device.Attach(d3d12_device);
        impl_->InitGenericComputeRootSig();
    }
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t count = (worker_count > 0u) ? std::min(worker_count, 16u) : std::clamp(hw / 2u, 2u, 8u);
    impl_->workers.reserve(count);
    for (uint32_t i = 0u; i < count; ++i) {
        impl_->workers.emplace_back([this]() { impl_->WorkerLoop(); });
    }
}
#else
AsyncPsoCompilePool::AsyncPsoCompilePool(uint32_t worker_count) : impl_(std::make_unique<Impl>()) {
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t count = (worker_count > 0u) ? std::min(worker_count, 16u) : std::clamp(hw / 2u, 2u, 8u);
    impl_->workers.reserve(count);
    for (uint32_t i = 0u; i < count; ++i) {
        impl_->workers.emplace_back([this]() { impl_->WorkerLoop(); });
    }
}
#endif

AsyncPsoCompilePool::~AsyncPsoCompilePool() { Shutdown(false); }

uint64_t AsyncPsoCompilePool::ComputeJobContentHash(const AsyncPsoJobDesc& desc) noexcept {
    uint64_t h = 14695981039346656037ull;
    h = Fnv1a64Update(h, desc.shader_blob.data(), desc.shader_blob.size());
    h = Fnv1a64Update(h, desc.target_profile.data(), desc.target_profile.size());
    h = Fnv1a64Update(h, desc.hlsl_entry.data(), desc.hlsl_entry.size());
    const uint8_t flags[4] = {
        static_cast<uint8_t>(desc.is_hlsl_source ? 1u : 0u),
        static_cast<uint8_t>(desc.emit_spirv ? 1u : 0u),
        static_cast<uint8_t>(desc.compile_dxil ? 1u : 0u),
        static_cast<uint8_t>(desc.create_d3d12_pso ? 1u : 0u),
    };
    h = Fnv1a64Update(h, flags, sizeof(flags));
    return h;
}

std::future<AsyncPsoJobResult> AsyncPsoCompilePool::Submit(
    const AsyncPsoJobDesc& desc,
    std::function<void(const AsyncPsoJobResult&)> callback) {
    impl_->stat_submitted.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t hash = ComputeJobContentHash(desc);

    std::promise<AsyncPsoJobResult> prom;
    auto fut = prom.get_future();

    bool immediate_hit = false;
    bool immediate_cancelled = false;
    AsyncPsoJobResult hit_result{};

    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        if (impl_->stopping) {
            immediate_cancelled = true;
            hit_result.content_hash = hash;
            hit_result.cancelled = true;
            hit_result.status = Status::Error(StatusCode::DeviceError, "AsyncPsoCompilePool is shutting down");
            impl_->stat_cancelled.fetch_add(1u, std::memory_order_relaxed);
        } else {
            auto cit = impl_->cache.find(hash);
            if (cit != impl_->cache.end()) {
                immediate_hit = true;
                hit_result = cit->second;
                hit_result.cache_hit = true;
                impl_->stat_cache_hits.fetch_add(1u, std::memory_order_relaxed);
            } else {
                auto iit = impl_->inflight.find(hash);
                if (iit != impl_->inflight.end()) {
                    // Coalesce duplicate in-flight submission & boost priority if higher!
                    auto& job = iit->second;
                    job->waiters.push_back({std::move(prom), std::move(callback)});
                    if (!job->started && static_cast<uint8_t>(desc.priority) > static_cast<uint8_t>(job->priority)) {
                        job->priority = desc.priority;
                        impl_->pq.push({hash, job->priority, job->seq});
                    }
                    impl_->stat_dedup.fetch_add(1u, std::memory_order_relaxed);
                    impl_->stat_cache_hits.fetch_add(1u, std::memory_order_relaxed);
                } else {
                    auto job = std::make_shared<Impl::InFlightJob>();
                    job->content_hash = hash;
                    job->desc = desc;
                    job->priority = desc.priority;
                    job->seq = impl_->next_seq++;
                    job->waiters.push_back({std::move(prom), std::move(callback)});
                    impl_->inflight[hash] = job;
                    impl_->pq.push({hash, job->priority, job->seq});
                    impl_->stat_queued.fetch_add(1u, std::memory_order_relaxed);
                }
            }
        }
    }

    if (immediate_hit || immediate_cancelled) {
        if (callback) callback(hit_result);
        prom.set_value(std::move(hit_result));
        return fut;
    }

    impl_->cv.notify_one();
    return fut;
}

bool AsyncPsoCompilePool::TryGetCached(uint64_t content_hash, AsyncPsoJobResult& out_result) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->cache.find(content_hash);
    if (it == impl_->cache.end()) return false;
    out_result = it->second;
    out_result.cache_hit = true;
    return true;
}

void AsyncPsoCompilePool::CancelPending() {
    std::vector<Impl::Waiter> to_cancel;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        while (!impl_->pq.empty()) impl_->pq.pop();
        for (auto it = impl_->inflight.begin(); it != impl_->inflight.end();) {
            if (!it->second->started) {
                it->second->cancelled = true;
                for (auto& w : it->second->waiters) {
                    to_cancel.push_back(std::move(w));
                }
                it = impl_->inflight.erase(it);
            } else {
                ++it;
            }
        }
        if (impl_->active_workers == 0u) {
            impl_->idle_cv.notify_all();
        }
    }
    for (auto& w : to_cancel) {
        AsyncPsoJobResult r{};
        r.cancelled = true;
        r.status = Status::Error(StatusCode::DeviceError, "Job cancelled");
        impl_->stat_cancelled.fetch_add(1u, std::memory_order_relaxed);
        if (w.callback) w.callback(r);
        w.promise.set_value(std::move(r));
    }
}

void AsyncPsoCompilePool::WaitForIdle() {
    std::unique_lock<std::mutex> lk(impl_->mutex);
    impl_->idle_cv.wait(lk, [&]() { return impl_->pq.empty() && impl_->active_workers == 0u; });
}

void AsyncPsoCompilePool::Shutdown(bool cancel_pending) {
    if (!impl_) return;
    if (cancel_pending) {
        CancelPending();
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->cv.notify_all();
    for (auto& t : impl_->workers) {
        if (t.joinable()) t.join();
    }
    impl_->workers.clear();
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> plk(impl_->pso_mutex);
        impl_->d3d12_psos.clear();
    }
    impl_->generic_cs_root_sig.Reset();
    impl_->d3d12_device.Reset();
#endif
}

AsyncPsoPoolStats AsyncPsoCompilePool::Stats() const noexcept {
    AsyncPsoPoolStats s{};
    s.jobs_submitted = impl_->stat_submitted.load(std::memory_order_relaxed);
    s.jobs_queued = impl_->stat_queued.load(std::memory_order_relaxed);
    s.jobs_compiled = impl_->stat_compiled.load(std::memory_order_relaxed);
    s.jobs_cache_hits = impl_->stat_cache_hits.load(std::memory_order_relaxed);
    s.jobs_dedup_coalesced = impl_->stat_dedup.load(std::memory_order_relaxed);
    s.jobs_failed = impl_->stat_failed.load(std::memory_order_relaxed);
    s.jobs_cancelled = impl_->stat_cancelled.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(impl_->mutex);
    s.worker_count = static_cast<uint32_t>(impl_->workers.size());
    s.pending_queue_depth = static_cast<uint32_t>(impl_->pq.size());
    return s;
}

// ===========================================================================
// 5. Standalone Wave64-on-Wave32 LDS Emulation & Paired Barycentric/CullDistance Lowering
// ===========================================================================

std::string EmitWave64On32LdsHlslLibrary(const Wave64LdsEmulationConfig& config) {
    const uint32_t tg_rounded = std::max(64u, ((std::max(1u, config.threadgroup_size) + 63u) / 64u) * 64u);
    const uint32_t waves = std::max(std::max(1u, config.max_guest_waves), tg_rounded / 64u);
    return Wave64On32Emulator::EmitHlslHelperLibrary(waves);
}

Status EmitWave64On32LdsSpirv(
    const Wave64LdsEmulationConfig& config,
    std::vector<uint32_t>& out_spirv) {
    out_spirv.clear();

    const uint32_t tg_size = std::clamp(((std::max(1u, config.threadgroup_size) + 63u) / 64u) * 64u, 64u, 1024u);
    const uint32_t guest_waves = std::max(std::max(1u, config.max_guest_waves), tg_size / 64u);
    const uint32_t lds_dwords = guest_waves * 64u;
    const uint32_t ballot_dwords = guest_waves * 2u;

    uint32_t id = 1u;
    auto nid = [&]() { return id++; };

    std::vector<uint32_t> decs, types, body;
    std::unordered_map<uint32_t, uint32_t> uconsts;

    const uint32_t ty_void = nid(), ty_bool = nid(), ty_u32 = nid();
    const uint32_t ty_rt_u32 = nid(), ty_buf = nid();
    const uint32_t ty_ptr_sb_buf = nid(), ty_ptr_sb_u32 = nid();
    const uint32_t ty_ptr_in_u32 = nid(), ty_ptr_wg_u32 = nid(), ty_fn = nid();

    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeBool, {ty_bool});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeRuntimeArray, {ty_rt_u32, ty_u32});
    EmitSpv(types, SpvOpTypeStruct, {ty_buf, ty_rt_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_sb_buf, 12u, ty_buf});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_sb_u32, 12u, ty_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_u32, 1u, ty_u32});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_u32, 4u, ty_u32});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    auto cu = [&](uint32_t v) -> uint32_t {
        auto it = uconsts.find(v);
        if (it != uconsts.end()) return it->second;
        const uint32_t cid = nid();
        EmitSpv(types, SpvOpConstant, {ty_u32, cid, v});
        uconsts[v] = cid;
        return cid;
    };

    const uint32_t ty_arr_lds = nid(), ty_arr_bal = nid();
    const uint32_t ty_ptr_wg_lds = nid(), ty_ptr_wg_bal = nid();
    EmitSpv(types, SpvOpTypeArray, {ty_arr_lds, ty_u32, cu(lds_dwords)});
    EmitSpv(types, SpvOpTypeArray, {ty_arr_bal, ty_u32, cu(ballot_dwords)});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_lds, 4u, ty_arr_lds});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_wg_bal, 4u, ty_arr_bal});

    const uint32_t var_in = nid(), var_out = nid(), var_lid = nid();
    const uint32_t var_lds = nid(), var_bal = nid();
    EmitSpv(types, SpvOpVariable, {ty_ptr_sb_buf, var_in, 12u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_sb_buf, var_out, 12u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_in_u32, var_lid, 1u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_wg_lds, var_lds, 4u});
    EmitSpv(types, SpvOpVariable, {ty_ptr_wg_bal, var_bal, 4u});

    EmitSpv(decs, SpvOpDecorate, {ty_rt_u32, 6u, 4u});
    EmitSpv(decs, SpvOpDecorate, {ty_buf, 2u});
    EmitSpv(decs, SpvOpMemberDecorate, {ty_buf, 0u, 35u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 34u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 33u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_in, 24u});
    EmitSpv(decs, SpvOpDecorate, {var_out, 34u, 0u});
    EmitSpv(decs, SpvOpDecorate, {var_out, 33u, 1u});
    EmitSpv(decs, SpvOpDecorate, {var_lid, 11u, 29u});  // BuiltIn LocalInvocationIndex

    auto bin = [&](uint16_t op, uint32_t ty, uint32_t a, uint32_t b) {
        const uint32_t r = nid();
        EmitSpv(body, op, {ty, r, a, b});
        return r;
    };
    auto sel = [&](uint32_t c, uint32_t t, uint32_t f) {
        const uint32_t r = nid();
        EmitSpv(body, SpvOpSelect, {ty_u32, r, c, t, f});
        return r;
    };
    auto load_in = [&](uint32_t idx) {
        const uint32_t p = nid(), v = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_sb_u32, p, var_in, cu(0u), idx});
        EmitSpv(body, SpvOpLoad, {ty_u32, v, p});
        return v;
    };
    auto store_out = [&](uint32_t idx, uint32_t val) {
        const uint32_t p = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_sb_u32, p, var_out, cu(0u), idx});
        EmitSpv(body, SpvOpStore, {p, val});
    };
    auto wg_barrier = [&]() {
        EmitSpv(body, SpvOpControlBarrier, {cu(2u), cu(2u), cu(0x108u)});
    };

    const uint32_t fn_main = nid(), lbl = nid();
    EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
    EmitSpv(body, SpvOpLabel, {lbl});

    const uint32_t lid = nid();
    EmitSpv(body, SpvOpLoad, {ty_u32, lid, var_lid});
    const uint32_t gw = bin(SpvOpShiftRightLogical, ty_u32, lid, cu(6u));
    const uint32_t lane = bin(SpvOpBitwiseAnd, ty_u32, lid, cu(63u));
    const uint32_t wave_base = bin(SpvOpShiftLeftLogical, ty_u32, gw, cu(6u));
    const uint32_t bal_base = bin(SpvOpShiftLeftLogical, ty_u32, gw, cu(1u));

    const uint32_t val = load_in(lid);
    const uint32_t rl_tgt = bin(SpvOpBitwiseAnd, ty_u32, load_in(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size))), cu(63u));

    // Store all 64 lanes per guest wave into isolated LDS slice + barrier
    const uint32_t self_idx = bin(SpvOpIAdd, ty_u32, wave_base, lane);
    const uint32_t p_lds_self = nid();
    EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_lds_self, var_lds, self_idx});
    EmitSpv(body, SpvOpStore, {p_lds_self, val});
    wg_barrier();

    uint32_t res_xchg = val;
    if (config.emit_cross_half_exchange) {
        const uint32_t xchg_lane = bin(SpvOpBitwiseXor, ty_u32, lane, cu(32u));
        const uint32_t xchg_idx = bin(SpvOpIAdd, ty_u32, wave_base, xchg_lane);
        const uint32_t p_xchg = nid();
        res_xchg = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_xchg, var_lds, xchg_idx});
        EmitSpv(body, SpvOpLoad, {ty_u32, res_xchg, p_xchg});
    }

    uint32_t res_rl = val;
    if (config.emit_readlane64) {
        const uint32_t rl_idx = bin(SpvOpIAdd, ty_u32, wave_base, rl_tgt);
        const uint32_t p_rl = nid();
        res_rl = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_rl, var_lds, rl_idx});
        EmitSpv(body, SpvOpLoad, {ty_u32, res_rl, p_rl});
    }

    uint32_t bal_lo = cu(0u);
    uint32_t bal_hi = cu(0u);
    if (config.emit_ballot64 || config.emit_mbcnt64) {
        const uint32_t b1_idx = bin(SpvOpIAdd, ty_u32, bal_base, cu(1u));
        const uint32_t p_b0 = nid(), p_b1 = nid(), clr0 = nid(), clr1 = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_b0, var_bal, bal_base});
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_b1, var_bal, b1_idx});

        wg_barrier();
        EmitSpv(body, SpvOpAtomicAnd, {ty_u32, clr0, p_b0, cu(2u), cu(0u), cu(0u)});
        EmitSpv(body, SpvOpAtomicAnd, {ty_u32, clr1, p_b1, cu(2u), cu(0u), cu(0u)});
        wg_barrier();

        const uint32_t pred = bin(SpvOpINotEqual, ty_bool, bin(SpvOpBitwiseAnd, ty_u32, val, cu(1u)), cu(0u));
        const uint32_t half_i = bin(SpvOpShiftRightLogical, ty_u32, lane, cu(5u));
        const uint32_t sub_lane = bin(SpvOpBitwiseAnd, ty_u32, lane, cu(31u));
        const uint32_t bit = bin(SpvOpShiftLeftLogical, ty_u32, cu(1u), sub_lane);
        const uint32_t contrib = sel(pred, bit, cu(0u));
        const uint32_t slot = bin(SpvOpIAdd, ty_u32, bal_base, half_i);
        const uint32_t p_bw = nid(), old_or = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_wg_u32, p_bw, var_bal, slot});
        EmitSpv(body, SpvOpAtomicOr, {ty_u32, old_or, p_bw, cu(2u), cu(0u), contrib});
        wg_barrier();

        bal_lo = nid();
        bal_hi = nid();
        EmitSpv(body, SpvOpLoad, {ty_u32, bal_lo, p_b0});
        EmitSpv(body, SpvOpLoad, {ty_u32, bal_hi, p_b1});
    }

    uint32_t mb_lo = cu(0u);
    uint32_t mb_hi = cu(0u);
    if (config.emit_mbcnt64) {
        const uint32_t lt32 = bin(SpvOpULessThan, ty_bool, lane, cu(32u));
        const uint32_t sub_lane = bin(SpvOpBitwiseAnd, ty_u32, lane, cu(31u));
        const uint32_t sub_mask = bin(SpvOpISub, ty_u32, bin(SpvOpShiftLeftLogical, ty_u32, cu(1u), sub_lane), cu(1u));
        const uint32_t lo_mask = sel(lt32, sub_mask, cu(0xFFFFFFFFu));
        const uint32_t hi_mask = sel(lt32, cu(0u), sub_mask);
        mb_lo = nid();
        const uint32_t mb_hi_part = nid();
        EmitSpv(body, SpvOpBitCount, {ty_u32, mb_lo, bin(SpvOpBitwiseAnd, ty_u32, bal_lo, lo_mask)});
        EmitSpv(body, SpvOpBitCount, {ty_u32, mb_hi_part, bin(SpvOpBitwiseAnd, ty_u32, bal_hi, hi_mask)});
        mb_hi = bin(SpvOpIAdd, ty_u32, mb_lo, mb_hi_part);
    }

    store_out(lid, res_xchg);
    store_out(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size)), res_rl);
    store_out(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size * 2u)), bal_lo);
    store_out(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size * 3u)), bal_hi);
    store_out(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size * 4u)), mb_lo);
    store_out(bin(SpvOpIAdd, ty_u32, lid, cu(tg_size * 5u)), mb_hi);

    EmitSpv(body, SpvOpReturn, {});
    EmitSpv(body, SpvOpFunctionEnd, {});

    out_spirv = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
    EmitSpv(out_spirv, SpvOpCapability, {1u});  // Shader
    EmitSpv(out_spirv, SpvOpMemoryModel, {0u, 1u});
    EmitSpv(out_spirv, SpvOpEntryPoint, {5u, fn_main, 0x6E69616Du, 0u, var_lid});
    EmitSpv(out_spirv, SpvOpExecutionMode, {fn_main, 17u, tg_size, 1u, 1u});
    out_spirv.insert(out_spirv.end(), decs.begin(), decs.end());
    out_spirv.insert(out_spirv.end(), types.begin(), types.end());
    out_spirv.insert(out_spirv.end(), body.begin(), body.end());
    return ValidateSpirvBinary(out_spirv);
}

Status LowerRdna2StageWithBarycentricAndCullDistance(
    const Xs2ShaderContainer& xs2,
    const Rdna2BarycentricCullStageDesc& desc,
    Rdna2BarycentricCullLiftResult& out_result) {
    out_result = Rdna2BarycentricCullLiftResult{};
    if (desc.stage != ShaderStage::Vertex && desc.stage != ShaderStage::Geometry &&
        desc.stage != ShaderStage::Mesh) {
        return Status::Error(
            StatusCode::InvalidArgument,
            "LowerRdna2StageWithBarycentricAndCullDistance supports Vertex, Geometry, and Mesh stages");
    }

    std::vector<Rdna2Instruction> insns;
    if (!xs2.code_words.empty()) {
        Status dst = DisassembleRdna2(xs2.code_words, insns);
        if (!dst.ok()) return dst;
    }

    const uint32_t cull_n = desc.preserve_cull_distance
                                ? std::clamp(desc.cull_distance_count, 1u, 8u)
                                : 0u;
    const bool keep_bary = desc.preserve_paired_barycentrics;
    const uint32_t max_v = std::max(3u, desc.max_vertices);
    const uint32_t max_p = std::max(1u, desc.max_primitives);
    const uint32_t tg_x = std::max(1u, desc.threadgroup_x);
    const auto alu = SummarizeAndLiftRdna2Alu(insns, (desc.stage == ShaderStage::Vertex) ? "o.pos" : "v.pos");

    out_result.stage = desc.stage;
    out_result.preserved_cull_distances = cull_n;
    out_result.has_paired_barycentrics = keep_bary;

    // Helper for DXIL-compliant packed SV_CullDistance0/1 vector fields (max 2 float4 rows = 8 floats)
    auto emit_hlsl_cull_decls = [&](std::ostringstream& os) {
        if (cull_n == 0u) return;
        const uint32_t n0 = std::min(4u, cull_n);
        const char* ty0 = (n0 == 1u) ? "float" : ((n0 == 2u) ? "float2" : ((n0 == 3u) ? "float3" : "float4"));
        os << "    " << ty0 << " cull_dist0 : SV_CullDistance0;\n";
        if (cull_n > 4u) {
            const uint32_t n1 = cull_n - 4u;
            const char* ty1 = (n1 == 1u) ? "float" : ((n1 == 2u) ? "float2" : ((n1 == 3u) ? "float3" : "float4"));
            os << "    " << ty1 << " cull_dist1 : SV_CullDistance1;\n";
        }
    };
    auto emit_hlsl_cull_init = [&](std::ostringstream& os, const char* indent, const char* var) {
        if (cull_n == 0u) return;
        const uint32_t n0 = std::min(4u, cull_n);
        if (n0 == 1u) {
            os << indent << var << ".cull_dist0 = 1.0f;\n";
        } else {
            os << indent << var << ".cull_dist0 = float" << n0 << "(";
            for (uint32_t k = 0u; k < n0; ++k) {
                if (k > 0u) os << ", ";
                os << "1.0f - (" << k << ".0f * 0.0625f)";
            }
            os << ");\n";
        }
        if (cull_n > 4u) {
            const uint32_t n1 = cull_n - 4u;
            if (n1 == 1u) {
                os << indent << var << ".cull_dist1 = 1.0f - (4.0f * 0.0625f);\n";
            } else {
                os << indent << var << ".cull_dist1 = float" << n1 << "(";
                for (uint32_t k = 0u; k < n1; ++k) {
                    if (k > 0u) os << ", ";
                    os << "1.0f - (" << (k + 4u) << ".0f * 0.0625f)";
                }
                os << ");\n";
            }
        }
    };

    // 1. Emit SM 6.5 HLSL preserving paired barycentrics + SV_CullDistance
    if (desc.stage == ShaderStage::Vertex) {
        out_result.hlsl_target = "vs_6_5";
        std::ostringstream hlsl;
        hlsl << "// OpenScarlett VS with Paired Barycentrics & SV_CullDistance Preservation\n";
        hlsl << "struct VsOut {\n";
        hlsl << "    float4 pos : SV_Position;\n";
        hlsl << "    float4 col : TEXCOORD0;\n";
        if (keep_bary) {
            hlsl << "    float2 bary_pair : TEXCOORD1;\n";
            hlsl << "    float3 bary_tri  : TEXCOORD2;\n";
        }
        emit_hlsl_cull_decls(hlsl);
        hlsl << "};\n\n";
        hlsl << "VsOut main(uint vid : SV_VertexID) {\n";
        hlsl << "    VsOut o;\n";
        hlsl << "    uint corner = vid % 3u;\n";
        hlsl << "    float u = (corner == 0u) ? 1.0f : 0.0f;\n";
        hlsl << "    float v = (corner == 1u) ? 1.0f : 0.0f;\n";
        hlsl << "    o.pos = float4(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f, 1.0f);\n";
        hlsl << "    o.col = float4(u, v, 1.0f - u - v, 1.0f);\n";
        if (keep_bary) {
            hlsl << "    o.bary_pair = float2(u, v);\n";
            hlsl << "    o.bary_tri  = float3(1.0f - u - v, u, v);\n";
        }
        emit_hlsl_cull_init(hlsl, "    ", "o");
        hlsl << alu.hlsl_body;
        hlsl << "    return o;\n}\n";
        out_result.hlsl_source = hlsl.str();
    } else if (desc.stage == ShaderStage::Geometry) {
        out_result.hlsl_target = "gs_6_5";
        std::ostringstream hlsl;
        hlsl << "// OpenScarlett GS with Paired Barycentrics & SV_CullDistance Preservation\n";
        hlsl << "struct GsVert {\n";
        hlsl << "    float4 pos : SV_Position;\n";
        hlsl << "    float4 col : TEXCOORD0;\n";
        if (keep_bary) {
            hlsl << "    float2 bary_pair : TEXCOORD1;\n";
            hlsl << "    float3 bary_tri  : TEXCOORD2;\n";
        }
        emit_hlsl_cull_decls(hlsl);
        hlsl << "};\n\n";
        hlsl << "[maxvertexcount(" << max_v << ")]\n";
        hlsl << "void main(triangle GsVert verts[3], inout TriangleStream<GsVert> out_stream) {\n";
        hlsl << "    [unroll]\n";
        hlsl << "    for (uint i = 0u; i < 3u; ++i) {\n";
        hlsl << "        GsVert v = verts[i];\n";
        hlsl << alu.hlsl_body;
        hlsl << "        out_stream.Append(v);\n";
        hlsl << "    }\n";
        hlsl << "    out_stream.RestartStrip();\n";
        hlsl << "}\n";
        out_result.hlsl_source = hlsl.str();
    } else {
        out_result.hlsl_target = "ms_6_5";
        std::ostringstream hlsl;
        hlsl << "// OpenScarlett MS with Paired Barycentrics & SV_CullDistance Preservation\n";
        hlsl << "struct MeshVert {\n";
        hlsl << "    float4 pos : SV_Position;\n";
        hlsl << "    float4 col : TEXCOORD0;\n";
        if (keep_bary) {
            hlsl << "    float2 bary_pair : TEXCOORD1;\n";
            hlsl << "    float3 bary_tri  : TEXCOORD2;\n";
        }
        emit_hlsl_cull_decls(hlsl);
        hlsl << "};\n\n";
        hlsl << "[outputtopology(\"triangle\")]\n";
        hlsl << "[numthreads(" << tg_x << ", 1, 1)]\n";
        hlsl << "void main(uint gtid : SV_GroupIndex, out vertices MeshVert verts[" << max_v
             << "], out indices uint3 prims[" << max_p << "]) {\n";
        hlsl << "    SetMeshOutputCounts(" << max_v << "u, " << max_p << "u);\n";
        hlsl << "    for (uint vi = gtid; vi < " << max_v << "u; vi += " << tg_x << "u) {\n";
        hlsl << "        uint corner = vi % 3u;\n";
        hlsl << "        float u = (corner == 0u) ? 1.0f : 0.0f;\n";
        hlsl << "        float v_coord = (corner == 1u) ? 1.0f : 0.0f;\n";
        hlsl << "        MeshVert v;\n";
        hlsl << "        v.pos = float4(u * 2.0f - 1.0f, v_coord * 2.0f - 1.0f, 0.0f, 1.0f);\n";
        hlsl << "        v.col = float4(u, v_coord, 1.0f - u - v_coord, 1.0f);\n";
        if (keep_bary) {
            hlsl << "        v.bary_pair = float2(u, v_coord);\n";
            hlsl << "        v.bary_tri  = float3(1.0f - u - v_coord, u, v_coord);\n";
        }
        emit_hlsl_cull_init(hlsl, "        ", "v");
        hlsl << alu.hlsl_body;
        hlsl << "        verts[vi] = v;\n";
        hlsl << "    }\n";
        hlsl << "    for (uint pi = gtid; pi < " << max_p << "u; pi += " << tg_x << "u) {\n";
        hlsl << "        prims[pi] = uint3(0u, 1u, 2u);\n";
        hlsl << "    }\n";
        hlsl << "}\n";
        out_result.hlsl_source = hlsl.str();
    }

    // 2. Emit strictly valid SPIR-V (1.3 for VS/GS, 1.6 SPV_EXT_mesh_shader for MS)
    uint32_t id = 1u;
    auto nid = [&]() { return id++; };
    std::vector<uint32_t> decs, types, body;
    std::unordered_map<uint32_t, uint32_t> uconsts;
    std::unordered_map<uint32_t, uint32_t> fconsts;

    const uint32_t ty_void = nid(), ty_bool = nid(), ty_u32 = nid(), ty_i32 = nid();
    const uint32_t ty_f32 = nid(), ty_v2f = nid(), ty_v3f = nid(), ty_v4f = nid(), ty_v3u = nid();
    const uint32_t ty_fn = nid();
    EmitSpv(types, SpvOpTypeVoid, {ty_void});
    EmitSpv(types, SpvOpTypeBool, {ty_bool});
    EmitSpv(types, SpvOpTypeInt, {ty_u32, 32u, 0u});
    EmitSpv(types, SpvOpTypeInt, {ty_i32, 32u, 1u});
    EmitSpv(types, SpvOpTypeFloat, {ty_f32, 32u});
    EmitSpv(types, SpvOpTypeVector, {ty_v2f, ty_f32, 2u});
    EmitSpv(types, SpvOpTypeVector, {ty_v3f, ty_f32, 3u});
    EmitSpv(types, SpvOpTypeVector, {ty_v4f, ty_f32, 4u});
    EmitSpv(types, SpvOpTypeVector, {ty_v3u, ty_u32, 3u});
    EmitSpv(types, SpvOpTypeFunction, {ty_fn, ty_void});

    auto cu = [&](uint32_t v) -> uint32_t {
        auto it = uconsts.find(v);
        if (it != uconsts.end()) return it->second;
        const uint32_t cid = nid();
        EmitSpv(types, SpvOpConstant, {ty_u32, cid, v});
        uconsts[v] = cid;
        return cid;
    };
    auto cf = [&](float f) -> uint32_t {
        const uint32_t bits = FloatToBits(f);
        auto it = fconsts.find(bits);
        if (it != fconsts.end()) return it->second;
        const uint32_t cid = nid();
        EmitSpv(types, SpvOpConstant, {ty_f32, cid, bits});
        fconsts[bits] = cid;
        return cid;
    };

    const uint32_t ty_perv = nid();
    EmitSpv(decs, SpvOpDecorate, {ty_perv, 2u});                  // Block
    EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 0u, 11u, 0u});   // BuiltIn Position
    uint32_t ty_ptr_in_f32 = 0u, ty_ptr_out_f32 = 0u;
    if (cull_n > 0u) {
        const uint32_t ty_arr_cull = nid();
        EmitSpv(types, SpvOpTypeArray, {ty_arr_cull, ty_f32, cu(cull_n)});
        EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f, ty_arr_cull});
        EmitSpv(decs, SpvOpMemberDecorate, {ty_perv, 1u, 11u, 4u});  // BuiltIn CullDistance
        ty_ptr_in_f32 = nid();
        ty_ptr_out_f32 = nid();
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_f32, 1u, ty_f32});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_f32, 3u, ty_f32});
    } else {
        EmitSpv(types, SpvOpTypeStruct, {ty_perv, ty_v4f});
    }

    const uint32_t ty_ptr_out_v4f = nid();
    const uint32_t ty_ptr_out_v2f = nid();
    const uint32_t ty_ptr_out_v3f = nid();
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v4f, 3u, ty_v4f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v2f, 3u, ty_v2f});
    EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v3f, 3u, ty_v3f});

    const uint32_t fn_main = nid(), lbl = nid();
    std::vector<uint32_t> iface;

    if (desc.stage == ShaderStage::Vertex) {
        const uint32_t ty_ptr_in_i32 = nid(), ty_ptr_out_pv = nid();
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_i32, 1u, ty_i32});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_pv, 3u, ty_perv});

        const uint32_t var_vid = nid(), var_out = nid(), var_col = nid();
        EmitSpv(types, SpvOpVariable, {ty_ptr_in_i32, var_vid, 1u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_pv, var_out, 3u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_v4f, var_col, 3u});
        EmitSpv(decs, SpvOpDecorate, {var_vid, 11u, 42u});  // BuiltIn VertexIndex
        EmitSpv(decs, SpvOpDecorate, {var_col, 30u, 0u});   // Location 0
        iface = {var_vid, var_out, var_col};

        uint32_t var_bary2 = 0u, var_bary3 = 0u;
        if (keep_bary) {
            var_bary2 = nid();
            var_bary3 = nid();
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_v2f, var_bary2, 3u});
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_v3f, var_bary3, 3u});
            EmitSpv(decs, SpvOpDecorate, {var_bary2, 30u, 1u});  // Location 1
            EmitSpv(decs, SpvOpDecorate, {var_bary3, 30u, 2u});  // Location 2
            iface.push_back(var_bary2);
            iface.push_back(var_bary3);
        }

        EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
        EmitSpv(body, SpvOpLabel, {lbl});
        const uint32_t vid_i = nid(), vid_u = nid(), corner = nid();
        const uint32_t is0 = nid(), is1 = nid(), u = nid(), v = nid();
        const uint32_t uv_sum = nid(), w = nid();
        const uint32_t px = nid(), py = nid(), pos_x = nid(), pos_y = nid(), pos = nid(), col = nid();
        EmitSpv(body, SpvOpLoad, {ty_i32, vid_i, var_vid});
        EmitSpv(body, SpvOpBitcast, {ty_u32, vid_u, vid_i});
        EmitSpv(body, SpvOpUMod, {ty_u32, corner, vid_u, cu(3u)});
        EmitSpv(body, SpvOpIEqual, {ty_bool, is0, corner, cu(0u)});
        EmitSpv(body, SpvOpIEqual, {ty_bool, is1, corner, cu(1u)});
        EmitSpv(body, SpvOpSelect, {ty_f32, u, is0, cf(1.0f), cf(0.0f)});
        EmitSpv(body, SpvOpSelect, {ty_f32, v, is1, cf(1.0f), cf(0.0f)});
        EmitSpv(body, SpvOpFAdd, {ty_f32, uv_sum, u, v});
        EmitSpv(body, SpvOpFSub, {ty_f32, w, cf(1.0f), uv_sum});
        EmitSpv(body, SpvOpFMul, {ty_f32, px, u, cf(2.0f)});
        EmitSpv(body, SpvOpFMul, {ty_f32, py, v, cf(2.0f)});
        EmitSpv(body, SpvOpFSub, {ty_f32, pos_x, px, cf(1.0f)});
        EmitSpv(body, SpvOpFSub, {ty_f32, pos_y, py, cf(1.0f)});
        EmitSpv(body, SpvOpCompositeConstruct, {ty_v4f, pos, pos_x, pos_y, cf(0.0f), cf(1.0f)});
        EmitSpv(body, SpvOpCompositeConstruct, {ty_v4f, col, u, v, w, cf(1.0f)});

        const uint32_t p_pos = nid();
        EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, p_pos, var_out, cu(0u)});
        EmitSpv(body, SpvOpStore, {p_pos, pos});
        EmitSpv(body, SpvOpStore, {var_col, col});
        if (keep_bary) {
            const uint32_t b2 = nid(), b3 = nid();
            EmitSpv(body, SpvOpCompositeConstruct, {ty_v2f, b2, u, v});
            EmitSpv(body, SpvOpCompositeConstruct, {ty_v3f, b3, w, u, v});
            EmitSpv(body, SpvOpStore, {var_bary2, b2});
            EmitSpv(body, SpvOpStore, {var_bary3, b3});
        }
        for (uint32_t k = 0u; k < cull_n; ++k) {
            const uint32_t p_cd = nid();
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, p_cd, var_out, cu(1u), cu(k)});
            EmitSpv(body, SpvOpStore, {p_cd, cf(1.0f - static_cast<float>(k) * 0.0625f)});
        }
        EmitSpv(body, SpvOpReturn, {});
        EmitSpv(body, SpvOpFunctionEnd, {});

        out_result.spirv_words = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
        EmitSpv(out_result.spirv_words, SpvOpCapability, {1u});  // Shader
        if (cull_n > 0u) EmitSpv(out_result.spirv_words, SpvOpCapability, {45u});  // CullDistance
        EmitSpv(out_result.spirv_words, SpvOpMemoryModel, {0u, 1u});
        std::vector<uint32_t> ep = {0u, fn_main, 0x6E69616Du, 0u};
        ep.insert(ep.end(), iface.begin(), iface.end());
        EmitSpvVec(out_result.spirv_words, SpvOpEntryPoint, ep);
    } else if (desc.stage == ShaderStage::Geometry) {
        const uint32_t ty_arr_pv = nid(), ty_arr_v4 = nid();
        const uint32_t ty_ptr_in_pv = nid(), ty_ptr_in_v4 = nid(), ty_ptr_in_arr_v4 = nid();
        const uint32_t ty_ptr_out_pv = nid();
        EmitSpv(types, SpvOpTypeArray, {ty_arr_pv, ty_perv, cu(3u)});
        EmitSpv(types, SpvOpTypeArray, {ty_arr_v4, ty_v4f, cu(3u)});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_pv, 1u, ty_arr_pv});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v4, 1u, ty_v4f});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_arr_v4, 1u, ty_arr_v4});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_pv, 3u, ty_perv});

        const uint32_t var_in = nid(), var_out = nid(), var_col_in = nid(), var_col_out = nid();
        EmitSpv(types, SpvOpVariable, {ty_ptr_in_pv, var_in, 1u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_pv, var_out, 3u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_in_arr_v4, var_col_in, 1u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_v4f, var_col_out, 3u});
        EmitSpv(decs, SpvOpDecorate, {var_col_in, 30u, 0u});
        EmitSpv(decs, SpvOpDecorate, {var_col_out, 30u, 0u});
        iface = {var_in, var_out, var_col_in, var_col_out};

        uint32_t ty_ptr_in_v2 = 0u, ty_ptr_in_v3 = 0u;
        uint32_t var_b2_in = 0u, var_b2_out = 0u, var_b3_in = 0u, var_b3_out = 0u;
        if (keep_bary) {
            const uint32_t ty_arr_v2 = nid(), ty_arr_v3 = nid();
            const uint32_t ty_ptr_in_a2 = nid(), ty_ptr_in_a3 = nid();
            ty_ptr_in_v2 = nid();
            ty_ptr_in_v3 = nid();
            EmitSpv(types, SpvOpTypeArray, {ty_arr_v2, ty_v2f, cu(3u)});
            EmitSpv(types, SpvOpTypeArray, {ty_arr_v3, ty_v3f, cu(3u)});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_a2, 1u, ty_arr_v2});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_a3, 1u, ty_arr_v3});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v2, 1u, ty_v2f});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_v3, 1u, ty_v3f});
            var_b2_in = nid();
            var_b2_out = nid();
            var_b3_in = nid();
            var_b3_out = nid();
            EmitSpv(types, SpvOpVariable, {ty_ptr_in_a2, var_b2_in, 1u});
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_v2f, var_b2_out, 3u});
            EmitSpv(types, SpvOpVariable, {ty_ptr_in_a3, var_b3_in, 1u});
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_v3f, var_b3_out, 3u});
            EmitSpv(decs, SpvOpDecorate, {var_b2_in, 30u, 1u});
            EmitSpv(decs, SpvOpDecorate, {var_b2_out, 30u, 1u});
            EmitSpv(decs, SpvOpDecorate, {var_b3_in, 30u, 2u});
            EmitSpv(decs, SpvOpDecorate, {var_b3_out, 30u, 2u});
            iface.push_back(var_b2_in);
            iface.push_back(var_b2_out);
            iface.push_back(var_b3_in);
            iface.push_back(var_b3_out);
        }

        EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
        EmitSpv(body, SpvOpLabel, {lbl});
        for (uint32_t e = 0u; e < 3u; ++e) {
            const uint32_t pi = nid(), vi = nid(), po = nid();
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4, pi, var_in, cu(e), cu(0u)});
            EmitSpv(body, SpvOpLoad, {ty_v4f, vi, pi});
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, po, var_out, cu(0u)});
            EmitSpv(body, SpvOpStore, {po, vi});

            const uint32_t pci = nid(), vci = nid();
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v4, pci, var_col_in, cu(e)});
            EmitSpv(body, SpvOpLoad, {ty_v4f, vci, pci});
            EmitSpv(body, SpvOpStore, {var_col_out, vci});

            if (keep_bary) {
                const uint32_t pb2 = nid(), vb2 = nid(), pb3 = nid(), vb3 = nid();
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v2, pb2, var_b2_in, cu(e)});
                EmitSpv(body, SpvOpLoad, {ty_v2f, vb2, pb2});
                EmitSpv(body, SpvOpStore, {var_b2_out, vb2});
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_v3, pb3, var_b3_in, cu(e)});
                EmitSpv(body, SpvOpLoad, {ty_v3f, vb3, pb3});
                EmitSpv(body, SpvOpStore, {var_b3_out, vb3});
            }
            for (uint32_t k = 0u; k < cull_n; ++k) {
                const uint32_t pcd_i = nid(), vcd = nid(), pcd_o = nid();
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_in_f32, pcd_i, var_in, cu(e), cu(1u), cu(k)});
                EmitSpv(body, SpvOpLoad, {ty_f32, vcd, pcd_i});
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, pcd_o, var_out, cu(1u), cu(k)});
                EmitSpv(body, SpvOpStore, {pcd_o, vcd});
            }
            EmitSpv(body, SpvOpEmitVertex, {});
        }
        EmitSpv(body, SpvOpEndPrimitive, {});
        EmitSpv(body, SpvOpReturn, {});
        EmitSpv(body, SpvOpFunctionEnd, {});

        out_result.spirv_words = {0x07230203u, 0x00010300u, 0x4F533200u, id, 0u};
        EmitSpv(out_result.spirv_words, SpvOpCapability, {1u});  // Shader
        EmitSpv(out_result.spirv_words, SpvOpCapability, {2u});  // Geometry
        if (cull_n > 0u) EmitSpv(out_result.spirv_words, SpvOpCapability, {45u});  // CullDistance
        EmitSpv(out_result.spirv_words, SpvOpMemoryModel, {0u, 1u});
        std::vector<uint32_t> ep = {3u, fn_main, 0x6E69616Du, 0u};
        ep.insert(ep.end(), iface.begin(), iface.end());
        EmitSpvVec(out_result.spirv_words, SpvOpEntryPoint, ep);
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 22u});  // Triangles
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 0u, 1u});
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 29u});  // OutputTriangleStrip
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 26u, max_v});
    } else {
        // MeshEXT SPIR-V 1.6 (SPV_EXT_mesh_shader) preserving paired barycentrics + BuiltIn CullDistance
        const uint32_t ty_ptr_in_u32 = nid();
        const uint32_t ty_arr_pv = nid(), ty_ptr_out_pv_arr = nid();
        const uint32_t ty_arr_col = nid(), ty_ptr_out_col_arr = nid();
        const uint32_t ty_arr_prims = nid(), ty_ptr_out_prims = nid(), ty_ptr_out_v3u = nid();
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_in_u32, 1u, ty_u32});
        EmitSpv(types, SpvOpTypeArray, {ty_arr_pv, ty_perv, cu(max_v)});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_pv_arr, 3u, ty_arr_pv});
        EmitSpv(types, SpvOpTypeArray, {ty_arr_col, ty_v4f, cu(max_v)});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_col_arr, 3u, ty_arr_col});
        EmitSpv(types, SpvOpTypeArray, {ty_arr_prims, ty_v3u, cu(max_p)});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_prims, 3u, ty_arr_prims});
        EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_v3u, 3u, ty_v3u});

        const uint32_t var_lid = nid(), var_out = nid(), var_col = nid(), var_prims = nid();
        EmitSpv(types, SpvOpVariable, {ty_ptr_in_u32, var_lid, 1u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_pv_arr, var_out, 3u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_col_arr, var_col, 3u});
        EmitSpv(types, SpvOpVariable, {ty_ptr_out_prims, var_prims, 3u});
        EmitSpv(decs, SpvOpDecorate, {var_lid, 11u, 29u});      // BuiltIn LocalInvocationIndex
        EmitSpv(decs, SpvOpDecorate, {var_col, 30u, 0u});       // Location 0
        EmitSpv(decs, SpvOpDecorate, {var_prims, 11u, 5294u});  // BuiltIn PrimitiveTriangleIndicesEXT
        iface = {var_lid, var_out, var_col, var_prims};

        uint32_t var_b2 = 0u, var_b3 = 0u;
        if (keep_bary) {
            const uint32_t ty_arr_b2 = nid(), ty_arr_b3 = nid();
            const uint32_t ty_ptr_out_a2 = nid(), ty_ptr_out_a3 = nid();
            EmitSpv(types, SpvOpTypeArray, {ty_arr_b2, ty_v2f, cu(max_v)});
            EmitSpv(types, SpvOpTypeArray, {ty_arr_b3, ty_v3f, cu(max_v)});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_a2, 3u, ty_arr_b2});
            EmitSpv(types, SpvOpTypePointer, {ty_ptr_out_a3, 3u, ty_arr_b3});
            var_b2 = nid();
            var_b3 = nid();
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_a2, var_b2, 3u});
            EmitSpv(types, SpvOpVariable, {ty_ptr_out_a3, var_b3, 3u});
            EmitSpv(decs, SpvOpDecorate, {var_b2, 30u, 1u});
            EmitSpv(decs, SpvOpDecorate, {var_b3, 30u, 2u});
            iface.push_back(var_b2);
            iface.push_back(var_b3);
        }

        EmitSpv(body, SpvOpFunction, {ty_void, fn_main, 0u, ty_fn});
        EmitSpv(body, SpvOpLabel, {lbl});
        EmitSpv(body, SpvOpSetMeshOutputsEXT, {cu(max_v), cu(max_p)});

        const uint32_t lid = nid(), v_lt = nid(), l_vbody = nid(), l_vmerge = nid();
        EmitSpv(body, SpvOpLoad, {ty_u32, lid, var_lid});
        EmitSpv(body, SpvOpULessThan, {ty_bool, v_lt, lid, cu(max_v)});
        EmitSpv(body, SpvOpSelectionMerge, {l_vmerge, 0u});
        EmitSpv(body, SpvOpBranchConditional, {v_lt, l_vbody, l_vmerge});

        EmitSpv(body, SpvOpLabel, {l_vbody});
        {
            const uint32_t corner = nid(), is0 = nid(), is1 = nid(), u = nid(), v = nid();
            const uint32_t uv_sum = nid(), w = nid();
            const uint32_t px = nid(), py = nid(), pos_x = nid(), pos_y = nid();
            const uint32_t pos = nid(), col = nid(), p_pos = nid(), p_col = nid();
            EmitSpv(body, SpvOpUMod, {ty_u32, corner, lid, cu(3u)});
            EmitSpv(body, SpvOpIEqual, {ty_bool, is0, corner, cu(0u)});
            EmitSpv(body, SpvOpIEqual, {ty_bool, is1, corner, cu(1u)});
            EmitSpv(body, SpvOpSelect, {ty_f32, u, is0, cf(1.0f), cf(0.0f)});
            EmitSpv(body, SpvOpSelect, {ty_f32, v, is1, cf(1.0f), cf(0.0f)});
            EmitSpv(body, SpvOpFAdd, {ty_f32, uv_sum, u, v});
            EmitSpv(body, SpvOpFSub, {ty_f32, w, cf(1.0f), uv_sum});
            EmitSpv(body, SpvOpFMul, {ty_f32, px, u, cf(2.0f)});
            EmitSpv(body, SpvOpFMul, {ty_f32, py, v, cf(2.0f)});
            EmitSpv(body, SpvOpFSub, {ty_f32, pos_x, px, cf(1.0f)});
            EmitSpv(body, SpvOpFSub, {ty_f32, pos_y, py, cf(1.0f)});
            EmitSpv(body, SpvOpCompositeConstruct, {ty_v4f, pos, pos_x, pos_y, cf(0.0f), cf(1.0f)});
            EmitSpv(body, SpvOpCompositeConstruct, {ty_v4f, col, u, v, w, cf(1.0f)});
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, p_pos, var_out, lid, cu(0u)});
            EmitSpv(body, SpvOpStore, {p_pos, pos});
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v4f, p_col, var_col, lid});
            EmitSpv(body, SpvOpStore, {p_col, col});
            if (keep_bary) {
                const uint32_t b2 = nid(), b3 = nid(), pb2 = nid(), pb3 = nid();
                EmitSpv(body, SpvOpCompositeConstruct, {ty_v2f, b2, u, v});
                EmitSpv(body, SpvOpCompositeConstruct, {ty_v3f, b3, w, u, v});
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v2f, pb2, var_b2, lid});
                EmitSpv(body, SpvOpStore, {pb2, b2});
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v3f, pb3, var_b3, lid});
                EmitSpv(body, SpvOpStore, {pb3, b3});
            }
            for (uint32_t k = 0u; k < cull_n; ++k) {
                const uint32_t p_cd = nid();
                EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_f32, p_cd, var_out, lid, cu(1u), cu(k)});
                EmitSpv(body, SpvOpStore, {p_cd, cf(1.0f - static_cast<float>(k) * 0.0625f)});
            }
        }
        EmitSpv(body, SpvOpBranch, {l_vmerge});

        EmitSpv(body, SpvOpLabel, {l_vmerge});
        const uint32_t p_lt = nid(), l_pbody = nid(), l_pmerge = nid();
        EmitSpv(body, SpvOpULessThan, {ty_bool, p_lt, lid, cu(max_p)});
        EmitSpv(body, SpvOpSelectionMerge, {l_pmerge, 0u});
        EmitSpv(body, SpvOpBranchConditional, {p_lt, l_pbody, l_pmerge});

        EmitSpv(body, SpvOpLabel, {l_pbody});
        {
            const uint32_t tri = nid(), p_pr = nid();
            EmitSpv(body, SpvOpCompositeConstruct, {ty_v3u, tri, cu(0u), cu(1u), cu(2u)});
            EmitSpv(body, SpvOpAccessChain, {ty_ptr_out_v3u, p_pr, var_prims, lid});
            EmitSpv(body, SpvOpStore, {p_pr, tri});
        }
        EmitSpv(body, SpvOpBranch, {l_pmerge});

        EmitSpv(body, SpvOpLabel, {l_pmerge});
        EmitSpv(body, SpvOpReturn, {});
        EmitSpv(body, SpvOpFunctionEnd, {});

        out_result.spirv_words = {0x07230203u, 0x00010600u, 0x4F533200u, id, 0u};
        EmitSpv(out_result.spirv_words, SpvOpCapability, {1u});     // Shader
        EmitSpv(out_result.spirv_words, SpvOpCapability, {5283u});  // MeshShadingEXT
        if (cull_n > 0u) EmitSpv(out_result.spirv_words, SpvOpCapability, {45u});  // CullDistance
        std::vector<uint32_t> ext_ops;
        AppendSpvExtString(ext_ops, "SPV_EXT_mesh_shader");
        EmitSpvVec(out_result.spirv_words, SpvOpExtension, ext_ops);
        EmitSpv(out_result.spirv_words, SpvOpMemoryModel, {0u, 1u});
        std::vector<uint32_t> ep = {5365u, fn_main, 0x6E69616Du, 0u};  // MeshEXT
        ep.insert(ep.end(), iface.begin(), iface.end());
        EmitSpvVec(out_result.spirv_words, SpvOpEntryPoint, ep);
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 17u, tg_x, 1u, 1u});
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 26u, max_v});
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 5270u, max_p});
        EmitSpv(out_result.spirv_words, SpvOpExecutionMode, {fn_main, 5298u});
    }

    out_result.spirv_words.insert(out_result.spirv_words.end(), decs.begin(), decs.end());
    out_result.spirv_words.insert(out_result.spirv_words.end(), types.begin(), types.end());
    out_result.spirv_words.insert(out_result.spirv_words.end(), body.begin(), body.end());
    return ValidateSpirvBinary(out_result.spirv_words);
}

}  // namespace openscarlett
