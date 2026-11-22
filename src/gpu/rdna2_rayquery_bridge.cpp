#include "openscarlett/gpu/rdna2_rayquery_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_lifter.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#endif

namespace openscarlett {

namespace {

constexpr uint64_t AlignUpU64(uint64_t v, uint64_t align) noexcept {
    return (v + align - 1ull) & ~(align - 1ull);
}

// ---------------------------------------------------------------------------
// SPIR-V 1.4 + SPV_KHR_ray_query Specification Constants (Khronos Registry)
// ---------------------------------------------------------------------------
constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kSpvVersion14 = 0x00010400u;
constexpr uint32_t kSpvGeneratorOpenScarlett = 0x4F533200u;

enum SpvOpCode : uint16_t {
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
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstant = 43,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpVectorShuffle = 79,
    SpvOpCompositeExtract = 81,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpUGreaterThanEqual = 174,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseAnd = 199,
    SpvOpLoopMerge = 246,
    SpvOpSelectionMerge = 247,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpReturn = 253,
    // SPV_KHR_ray_query opcodes
    SpvOpTypeRayQueryKHR = 4472,
    SpvOpRayQueryInitializeKHR = 4473,
    SpvOpRayQueryProceedKHR = 4477,
    SpvOpRayQueryGetIntersectionTypeKHR = 4479,
    SpvOpTypeAccelerationStructureKHR = 5341,
    SpvOpRayQueryGetIntersectionTKHR = 6018,
    SpvOpRayQueryGetIntersectionInstanceCustomIndexKHR = 6019,
    SpvOpRayQueryGetIntersectionInstanceIdKHR = 6020,
    SpvOpRayQueryGetIntersectionInstanceSbtOffsetKHR = 6021,
    SpvOpRayQueryGetIntersectionGeometryIndexKHR = 6022,
    SpvOpRayQueryGetIntersectionPrimitiveIndexKHR = 6023,
    SpvOpRayQueryGetIntersectionBarycentricsKHR = 6024,
    SpvOpRayQueryGetIntersectionFrontFaceKHR = 6025,
};

constexpr uint32_t kSpvCapShader = 1u;
constexpr uint32_t kSpvCapRayQueryKHR = 4472u;
constexpr uint32_t kSpvAddrLogical = 0u;
constexpr uint32_t kSpvMemGlsl450 = 1u;
constexpr uint32_t kSpvExecModelGlCompute = 5u;
constexpr uint32_t kSpvExecModeLocalSize = 17u;

constexpr uint32_t kSpvScUniformConstant = 0u;
constexpr uint32_t kSpvScInput = 1u;
constexpr uint32_t kSpvScUniform = 2u;
constexpr uint32_t kSpvScFunction = 7u;
constexpr uint32_t kSpvScStorageBuffer = 12u;

constexpr uint32_t kSpvDecBlock = 2u;
constexpr uint32_t kSpvDecArrayStride = 6u;
constexpr uint32_t kSpvDecBuiltIn = 11u;
constexpr uint32_t kSpvDecNonWritable = 24u;
constexpr uint32_t kSpvDecBinding = 33u;
constexpr uint32_t kSpvDecDescriptorSet = 34u;
constexpr uint32_t kSpvDecOffset = 35u;
constexpr uint32_t kSpvBuiltInGlobalInvocationId = 28u;

void EmitInst(std::vector<uint32_t>& out, uint16_t op, std::initializer_list<uint32_t> operands) {
    const uint32_t wc = static_cast<uint32_t>(1u + operands.size());
    out.push_back((wc << 16u) | static_cast<uint32_t>(op));
    for (uint32_t w : operands) {
        out.push_back(w);
    }
}

void EmitInstVec(std::vector<uint32_t>& out, uint16_t op, const std::vector<uint32_t>& operands) {
    const uint32_t wc = static_cast<uint32_t>(1u + operands.size());
    out.push_back((wc << 16u) | static_cast<uint32_t>(op));
    out.insert(out.end(), operands.begin(), operands.end());
}

void AppendSpvString(std::vector<uint32_t>& operands, const char* str) {
    const size_t len = std::strlen(str) + 1u;  // include NUL terminator
    const size_t word_count = (len + 3u) / 4u;
    for (size_t w = 0; w < word_count; ++w) {
        uint32_t packed = 0u;
        for (size_t b = 0; b < 4u; ++b) {
            const size_t idx = w * 4u + b;
            if (idx < len) {
                packed |= static_cast<uint32_t>(static_cast<uint8_t>(str[idx])) << (b * 8u);
            }
        }
        operands.push_back(packed);
    }
}

std::string DecodeSpvString(const std::vector<uint32_t>& words, size_t start, size_t end) {
    std::string out;
    for (size_t i = start; i < end; ++i) {
        const uint32_t w = words[i];
        for (uint32_t b = 0u; b < 4u; ++b) {
            const char ch = static_cast<char>((w >> (b * 8u)) & 0xFFu);
            if (ch == '\0') {
                return out;
            }
            out.push_back(ch);
        }
    }
    return out;
}

uint32_t FloatToU32Bits(float f) noexcept {
    uint32_t bits = 0u;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

#if defined(_WIN32)
template <typename T>
void SafeReleaseCom(T*& p) noexcept {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

Status CreateD3D12Buffer(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    uint64_t byte_size,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out_res) {
    if (!device || !out_res || byte_size == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "Invalid D3D12 buffer parameters");
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1u;
    hp.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0ull;
    rd.Width = byte_size;
    rd.Height = 1u;
    rd.DepthOrArraySize = 1u;
    rd.MipLevels = 1u;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1u;
    rd.SampleDesc.Quality = 0u;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    const HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr, IID_PPV_ARGS(out_res));
    if (FAILED(hr) || !*out_res) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource failed in RayQueryExecutor");
    }
    return Status::Ok();
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// 1. DXR 1.1 HLSL SM 6.5 Emission (RayQuery<RAY_FLAG_NONE>)
// ---------------------------------------------------------------------------
Status EmitRdna2RayQueryDxr11Hlsl(
    const Rdna2BvhIntersectDesc& desc,
    std::string& out_hlsl) {
    const uint32_t tg_x = std::clamp<uint32_t>(desc.threadgroup_x, 1u, 1024u);
    const char* op_name = (desc.op == Rdna2BvhIntersectOp::ImageBvh64IntersectRay)
                              ? "image_bvh64_intersect_ray"
                              : "image_bvh_intersect_ray";

    std::ostringstream ss;
    ss << "// OpenScarlett RDNA2 -> DXR 1.1 Inline RayQuery (SM 6.5)\n"
       << "// Source RDNA2 op: " << op_name
       << " (a16=" << (desc.a16 ? "1" : "0")
       << ", r128=" << (desc.r128 ? "1" : "0") << ")\n\n"
       << "struct RayInput {\n"
       << "    float3 origin;\n"
       << "    float  t_min;\n"
       << "    float3 direction;\n"
       << "    float  t_max;\n"
       << "    uint   ray_flags;\n"
       << "    uint   instance_mask;\n"
       << "    uint   pad0;\n"
       << "    uint   pad1;\n"
       << "};\n\n"
       << "struct RayHitOutput {\n"
       << "    uint  committed_status;\n"
       << "    float t;\n"
       << "    float bary_u;\n"
       << "    float bary_v;\n"
       << "    uint  primitive_index;\n"
       << "    uint  instance_index;\n"
       << "    uint  instance_id;\n"
       << "    uint  geometry_index;\n"
       << "    uint  sbt_offset;\n"
       << "    uint  front_face;\n"
       << "    uint  pad0;\n"
       << "    uint  pad1;\n"
       << "};\n\n"
       << "cbuffer RayQueryDispatchParams : register(b0) {\n"
       << "    uint RayCount;\n"
       << "    uint DefaultRayFlags;\n"
       << "    uint DefaultInstanceMask;\n"
       << "    uint Reserved0;\n"
       << "};\n\n"
       << "RaytracingAccelerationStructure SceneTlas : register(t0);\n"
       << "StructuredBuffer<RayInput> RayInputs : register(t1);\n"
       << "RWStructuredBuffer<RayHitOutput> RayOutputs : register(u0);\n\n"
       << "[numthreads(" << tg_x << ", 1, 1)]\n"
       << "void main(uint3 tid : SV_DispatchThreadID) {\n"
       << "    if (tid.x >= RayCount) {\n"
       << "        return;\n"
       << "    }\n\n"
       << "    RayInput rin = RayInputs[tid.x];\n"
       << "    RayDesc ray;\n"
       << "    ray.Origin    = rin.origin;\n"
       << "    ray.TMin      = rin.t_min;\n"
       << "    ray.Direction = rin.direction;\n"
       << "    ray.TMax      = rin.t_max;\n\n"
       << "    uint flags = rin.ray_flags | DefaultRayFlags | " << desc.ray_flags << "u;\n"
       << "    uint mask  = (rin.instance_mask & DefaultInstanceMask & " << (desc.instance_mask & 0xFFu) << "u);\n\n"
       << "    RayQuery<RAY_FLAG_NONE> q;\n"
       << "    q.TraceRayInline(SceneTlas, flags, mask, ray);\n"
       << "    while (q.Proceed()) {\n"
       << "        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {\n"
       << "            q.CommitNonOpaqueTriangleHit();\n"
       << "        }\n"
       << "    }\n\n"
       << "    RayHitOutput hout;\n"
       << "    hout.pad0 = 0u;\n"
       << "    hout.pad1 = 0u;\n"
       << "    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {\n"
       << "        float2 bary = q.CommittedTriangleBarycentrics();\n"
       << "        hout.committed_status = 1u;\n"
       << "        hout.t                = q.CommittedRayT();\n"
       << "        hout.bary_u           = bary.x;\n"
       << "        hout.bary_v           = bary.y;\n"
       << "        hout.primitive_index  = q.CommittedPrimitiveIndex();\n"
       << "        hout.instance_index   = q.CommittedInstanceIndex();\n"
       << "        hout.instance_id      = q.CommittedInstanceID();\n"
       << "        hout.geometry_index   = q.CommittedGeometryIndex();\n"
       << "        hout.sbt_offset       = q.CommittedInstanceContributionToHitGroupIndex();\n"
       << "        hout.front_face       = q.CommittedTriangleFrontFace() ? 1u : 0u;\n"
       << "    } else {\n"
       << "        hout.committed_status = 0u;\n"
       << "        hout.t                = rin.t_max;\n"
       << "        hout.bary_u           = 0.0f;\n"
       << "        hout.bary_v           = 0.0f;\n"
       << "        hout.primitive_index  = 0xFFFFFFFFu;\n"
       << "        hout.instance_index   = 0xFFFFFFFFu;\n"
       << "        hout.instance_id      = 0xFFFFFFFFu;\n"
       << "        hout.geometry_index   = 0u;\n"
       << "        hout.sbt_offset       = 0u;\n"
       << "        hout.front_face       = 1u;\n"
       << "    }\n"
       << "    RayOutputs[tid.x] = hout;\n"
       << "}\n";

    out_hlsl = ss.str();
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// 2. Software Compute BVH4 Fallback HLSL (cs_6_5 and cs_5_0 compatible)
//    Executes live multi-window 64-bit RDNA2 BVH4 traversal on ANY GPU / WARP.
// ---------------------------------------------------------------------------
Status EmitRdna2ComputeBvh4FallbackHlsl(
    const Rdna2BvhIntersectDesc& desc,
    std::string& out_hlsl) {
    const uint32_t tg_x = std::clamp<uint32_t>(desc.threadgroup_x, 1u, 1024u);
    const uint32_t stack_depth = std::clamp<uint32_t>(desc.max_stack_depth, 16u, 64u);

    std::ostringstream ss;
    ss << "// OpenScarlett RDNA2 BVH64 Multi-Window Compute Traversal Fallback (SM 6.5 / SM 5.0)\n"
       << "struct GpuBvhWindowDesc {\n"
       << "    uint base_va_lo;\n"
       << "    uint base_va_hi;\n"
       << "    uint dword_offset;\n"
       << "    uint dword_count;\n"
       << "    uint root_node_ptr;\n"
       << "    uint as_type;\n"
       << "    uint pad0;\n"
       << "    uint pad1;\n"
       << "};\n\n"
       << "struct RayInput {\n"
       << "    float3 origin;\n"
       << "    float  t_min;\n"
       << "    float3 direction;\n"
       << "    float  t_max;\n"
       << "    uint   ray_flags;\n"
       << "    uint   instance_mask;\n"
       << "    uint   pad0;\n"
       << "    uint   pad1;\n"
       << "};\n\n"
       << "struct RayHitOutput {\n"
       << "    uint  committed_status;\n"
       << "    float t;\n"
       << "    float bary_u;\n"
       << "    float bary_v;\n"
       << "    uint  primitive_index;\n"
       << "    uint  instance_index;\n"
       << "    uint  instance_id;\n"
       << "    uint  geometry_index;\n"
       << "    uint  sbt_offset;\n"
       << "    uint  front_face;\n"
       << "    uint  pad0;\n"
       << "    uint  pad1;\n"
       << "};\n\n"
       << "cbuffer BvhDispatchParams : register(b0) {\n"
       << "    uint RayCount;\n"
       << "    uint TlasWindowIndex;\n"
       << "    uint WindowCount;\n"
       << "    uint DefaultRayFlags;\n"
       << "};\n\n"
       << "StructuredBuffer<GpuBvhWindowDesc> WindowTable : register(t0);\n"
       << "StructuredBuffer<uint> BvhData : register(t1);\n"
       << "StructuredBuffer<RayInput> RayInputs : register(t2);\n"
       << "RWStructuredBuffer<RayHitOutput> RayOutputs : register(u0);\n\n"
       << "bool IntersectAabbSlab(float3 bmin, float3 bmax, float3 orig, float3 dir, float ray_tmin, float t_best, out float out_tnear) {\n"
       << "    float tmin = ray_tmin;\n"
       << "    float tmax = t_best;\n"
       << "    // X axis\n"
       << "    if (abs(dir.x) < 1e-8f) {\n"
       << "        if (orig.x < bmin.x || orig.x > bmax.x) { out_tnear = 1e30f; return false; }\n"
       << "    } else {\n"
       << "        float inv = 1.0f / dir.x;\n"
       << "        float t0 = (bmin.x - orig.x) * inv;\n"
       << "        float t1 = (bmax.x - orig.x) * inv;\n"
       << "        tmin = max(tmin, min(t0, t1));\n"
       << "        tmax = min(tmax, max(t0, t1));\n"
       << "        if (tmax < tmin) { out_tnear = 1e30f; return false; }\n"
       << "    }\n"
       << "    // Y axis\n"
       << "    if (abs(dir.y) < 1e-8f) {\n"
       << "        if (orig.y < bmin.y || orig.y > bmax.y) { out_tnear = 1e30f; return false; }\n"
       << "    } else {\n"
       << "        float inv = 1.0f / dir.y;\n"
       << "        float t0 = (bmin.y - orig.y) * inv;\n"
       << "        float t1 = (bmax.y - orig.y) * inv;\n"
       << "        tmin = max(tmin, min(t0, t1));\n"
       << "        tmax = min(tmax, max(t0, t1));\n"
       << "        if (tmax < tmin) { out_tnear = 1e30f; return false; }\n"
       << "    }\n"
       << "    // Z axis\n"
       << "    if (abs(dir.z) < 1e-8f) {\n"
       << "        if (orig.z < bmin.z || orig.z > bmax.z) { out_tnear = 1e30f; return false; }\n"
       << "    } else {\n"
       << "        float inv = 1.0f / dir.z;\n"
       << "        float t0 = (bmin.z - orig.z) * inv;\n"
       << "        float t1 = (bmax.z - orig.z) * inv;\n"
       << "        tmin = max(tmin, min(t0, t1));\n"
       << "        tmax = min(tmax, max(t0, t1));\n"
       << "        if (tmax < tmin) { out_tnear = 1e30f; return false; }\n"
       << "    }\n"
       << "    out_tnear = tmin;\n"
       << "    return true;\n"
       << "}\n\n"
       << "bool IntersectTri(float3 v0, float3 v1, float3 v2, float3 orig, float3 dir, float ray_tmin, float t_best,\n"
       << "                  uint ray_flags, uint inst_flags, out float out_t, out float out_u, out float out_v, out uint out_ff) {\n"
       << "    out_t = 1e30f; out_u = 0.0f; out_v = 0.0f; out_ff = 1u;\n"
       << "    float3 e1 = v1 - v0;\n"
       << "    float3 e2 = v2 - v0;\n"
       << "    float3 pvec = cross(dir, e2);\n"
       << "    float det = dot(e1, pvec);\n"
       << "    if (abs(det) < 1e-7f) return false;\n"
       << "    bool front_ccw = (inst_flags & 2u) != 0u;\n"
       << "    bool ff = front_ccw ? (det < 0.0f) : (det > 0.0f);\n"
       << "    bool cull_disable = (inst_flags & 1u) != 0u;\n"
       << "    if (!cull_disable) {\n"
       << "        if (((ray_flags & 0x10u) != 0u) && !ff) return false;\n"
       << "        if (((ray_flags & 0x20u) != 0u) && ff) return false;\n"
       << "    }\n"
       << "    float inv_det = 1.0f / det;\n"
       << "    float3 tvec = orig - v0;\n"
       << "    float u = dot(tvec, pvec) * inv_det;\n"
       << "    if (u < 0.0f || u > 1.0f) return false;\n"
       << "    float3 qvec = cross(tvec, e1);\n"
       << "    float v = dot(dir, qvec) * inv_det;\n"
       << "    if (v < 0.0f || (u + v) > 1.0f) return false;\n"
       << "    float t = dot(e2, qvec) * inv_det;\n"
       << "    if (t < ray_tmin || t >= t_best) return false;\n"
       << "    out_t = t; out_u = u; out_v = v; out_ff = ff ? 1u : 0u;\n"
       << "    return true;\n"
       << "}\n\n"
       << "uint FindWindowByVa(uint va_lo, uint va_hi) {\n"
       << "    [loop] for (uint i = 0u; i < WindowCount; ++i) {\n"
       << "        if (WindowTable[i].base_va_lo == va_lo && WindowTable[i].base_va_hi == va_hi) {\n"
       << "            return i;\n"
       << "        }\n"
       << "    }\n"
       << "    return 0xFFFFFFFFu;\n"
       << "}\n\n"
       << "void DecodeBoxChild(uint base_dw, uint ptr, uint c, out uint child_ptr, out float3 bmin, out float3 bmax) {\n"
       << "    uint ntype = ptr & 7u;\n"
       << "    uint node_dw = base_dw + ((ptr & 0xFFFFFFF8u) << 1u);\n"
       << "    child_ptr = BvhData[node_dw + c];\n"
       << "    if (ntype == 5u) {\n"
       << "        uint bo = node_dw + 4u + c * 6u;\n"
       << "        bmin = float3(asfloat(BvhData[bo + 0u]), asfloat(BvhData[bo + 1u]), asfloat(BvhData[bo + 2u]));\n"
       << "        bmax = float3(asfloat(BvhData[bo + 3u]), asfloat(BvhData[bo + 4u]), asfloat(BvhData[bo + 5u]));\n"
       << "    } else {\n"
       << "        uint bo = node_dw + 4u + c * 3u;\n"
       << "        uint w0 = BvhData[bo + 0u];\n"
       << "        uint w1 = BvhData[bo + 1u];\n"
       << "        uint w2 = BvhData[bo + 2u];\n"
       << "        bmin = float3(f16tof32(w0 & 0xFFFFu), f16tof32(w0 >> 16u), f16tof32(w1 & 0xFFFFu));\n"
       << "        bmax = float3(f16tof32(w1 >> 16u), f16tof32(w2 & 0xFFFFu), f16tof32(w2 >> 16u));\n"
       << "    }\n"
       << "}\n\n"
       << "[numthreads(" << tg_x << ", 1, 1)]\n"
       << "void main(uint3 tid : SV_DispatchThreadID) {\n"
       << "    if (tid.x >= RayCount || TlasWindowIndex >= WindowCount) {\n"
       << "        return;\n"
       << "    }\n\n"
       << "    RayInput rin = RayInputs[tid.x];\n"
       << "    uint ray_flags = rin.ray_flags | DefaultRayFlags;\n"
       << "    uint ray_mask = rin.instance_mask & 0xFFu;\n"
       << "    bool accept_first = (ray_flags & 0x04u) != 0u;\n\n"
       << "    RayHitOutput hout;\n"
       << "    hout.committed_status = 0u;\n"
       << "    hout.t = rin.t_max;\n"
       << "    hout.bary_u = 0.0f;\n"
       << "    hout.bary_v = 0.0f;\n"
       << "    hout.primitive_index = 0xFFFFFFFFu;\n"
       << "    hout.instance_index = 0xFFFFFFFFu;\n"
       << "    hout.instance_id = 0xFFFFFFFFu;\n"
       << "    hout.geometry_index = 0u;\n"
       << "    hout.sbt_offset = 0u;\n"
       << "    hout.front_face = 1u;\n"
       << "    hout.pad0 = 0u;\n"
       << "    hout.pad1 = 0u;\n\n"
       << "    GpuBvhWindowDesc root_win = WindowTable[TlasWindowIndex];\n"
       << "    uint root_base = root_win.dword_offset;\n"
       << "    float3 root_min = float3(asfloat(BvhData[root_base + 14u]), asfloat(BvhData[root_base + 15u]), asfloat(BvhData[root_base + 16u]));\n"
       << "    float3 root_max = float3(asfloat(BvhData[root_base + 17u]), asfloat(BvhData[root_base + 18u]), asfloat(BvhData[root_base + 19u]));\n"
       << "    float root_near = 0.0f;\n"
       << "    if (!IntersectAabbSlab(root_min, root_max, rin.origin, rin.direction, rin.t_min, hout.t, root_near)) {\n"
       << "        RayOutputs[tid.x] = hout;\n"
       << "        return;\n"
       << "    }\n\n"
       << "    uint top_stack[" << stack_depth << "];\n"
       << "    uint blas_stack[" << stack_depth << "];\n"
       << "    int top_sp = 0;\n"
       << "    top_stack[top_sp++] = root_win.root_node_ptr;\n"
       << "    bool done = false;\n\n"
       << "    [loop] while (top_sp > 0 && !done) {\n"
       << "        uint ptr = top_stack[--top_sp];\n"
       << "        uint ntype = ptr & 7u;\n"
       << "        uint node_dw = root_base + ((ptr & 0xFFFFFFF8u) << 1u);\n\n"
       << "        if (ntype == 4u || ntype == 5u) {\n"
       << "            uint hit_ptrs[4];\n"
       << "            float hit_ts[4];\n"
       << "            int hc = 0;\n"
       << "            [unroll] for (uint c = 0u; c < 4u; ++c) {\n"
       << "                uint cptr = 0xFFFFFFFFu;\n"
       << "                float3 bmin = float3(0.0f, 0.0f, 0.0f);\n"
       << "                float3 bmax = float3(0.0f, 0.0f, 0.0f);\n"
       << "                DecodeBoxChild(root_base, ptr, c, cptr, bmin, bmax);\n"
       << "                if (cptr != 0xFFFFFFFFu) {\n"
       << "                    float tn = 0.0f;\n"
       << "                    if (IntersectAabbSlab(bmin, bmax, rin.origin, rin.direction, rin.t_min, hout.t, tn)) {\n"
       << "                        hit_ptrs[hc] = cptr;\n"
       << "                        hit_ts[hc] = tn;\n"
       << "                        hc++;\n"
       << "                    }\n"
       << "                }\n"
       << "            }\n"
       << "            // Sort descending by t_near so nearest child is popped first.\n"
       << "            for (int i = 0; i < hc; ++i) {\n"
       << "                for (int j = i + 1; j < hc; ++j) {\n"
       << "                    if (hit_ts[j] > hit_ts[i]) {\n"
       << "                        float ft = hit_ts[i]; hit_ts[i] = hit_ts[j]; hit_ts[j] = ft;\n"
       << "                        uint fp = hit_ptrs[i]; hit_ptrs[i] = hit_ptrs[j]; hit_ptrs[j] = fp;\n"
       << "                    }\n"
       << "                }\n"
       << "            }\n"
       << "            for (int k = 0; k < hc; ++k) {\n"
       << "                if (top_sp < " << stack_depth << ") { top_stack[top_sp++] = hit_ptrs[k]; }\n"
       << "            }\n"
       << "        } else if (ntype <= 3u) {\n"
       << "            // Direct BLAS triangle leaf in root window\n"
       << "            float3 v0 = float3(asfloat(BvhData[node_dw + 0u]), asfloat(BvhData[node_dw + 1u]), asfloat(BvhData[node_dw + 2u]));\n"
       << "            float3 v1 = float3(asfloat(BvhData[node_dw + 3u]), asfloat(BvhData[node_dw + 4u]), asfloat(BvhData[node_dw + 5u]));\n"
       << "            float3 v2 = float3(asfloat(BvhData[node_dw + 6u]), asfloat(BvhData[node_dw + 7u]), asfloat(BvhData[node_dw + 8u]));\n"
       << "            uint prim_idx = BvhData[node_dw + 13u];\n"
       << "            uint geom_packed = BvhData[node_dw + 14u];\n"
       << "            float t = 0.0f; float u = 0.0f; float v = 0.0f; uint ff = 1u;\n"
       << "            if (IntersectTri(v0, v1, v2, rin.origin, rin.direction, rin.t_min, hout.t, ray_flags, 0u, t, u, v, ff)) {\n"
       << "                hout.committed_status = 1u; hout.t = t; hout.bary_u = u; hout.bary_v = v;\n"
       << "                hout.primitive_index = prim_idx; hout.instance_index = 0u; hout.instance_id = 0u;\n"
       << "                hout.geometry_index = geom_packed & 0x00FFFFFFu; hout.sbt_offset = 0u; hout.front_face = ff;\n"
       << "                if (accept_first) { done = true; }\n"
       << "            }\n"
       << "        } else if (ntype == 6u) {\n"
       << "            // Instance leaf: resolve 64-bit blas_base_va across multi-window table\n"
       << "            uint va_lo = BvhData[node_dw + 0u];\n"
       << "            uint va_hi = BvhData[node_dw + 1u];\n"
       << "            uint blas_root = BvhData[node_dw + 2u];\n"
       << "            uint id_mask = BvhData[node_dw + 3u];\n"
       << "            uint inst_id = id_mask & 0x00FFFFFFu;\n"
       << "            uint inst_mask = (id_mask >> 24u) & 0xFFu;\n"
       << "            uint sbt_flags = BvhData[node_dw + 4u];\n"
       << "            uint sbt_off = sbt_flags & 0x00FFFFFFu;\n"
       << "            uint inst_flags = (sbt_flags >> 24u) & 0xFFu;\n"
       << "            uint inst_idx = BvhData[node_dw + 5u];\n"
       << "            if ((inst_mask & ray_mask) != 0u && blas_root != 0xFFFFFFFFu) {\n"
       << "                uint bw_idx = FindWindowByVa(va_lo, va_hi);\n"
       << "                if (bw_idx != 0xFFFFFFFFu) {\n"
       << "                    uint blas_base = WindowTable[bw_idx].dword_offset;\n"
       << "                    float4 r0 = float4(asfloat(BvhData[node_dw + 6u]), asfloat(BvhData[node_dw + 7u]), asfloat(BvhData[node_dw + 8u]), asfloat(BvhData[node_dw + 9u]));\n"
       << "                    float4 r1 = float4(asfloat(BvhData[node_dw + 10u]), asfloat(BvhData[node_dw + 11u]), asfloat(BvhData[node_dw + 12u]), asfloat(BvhData[node_dw + 13u]));\n"
       << "                    float4 r2 = float4(asfloat(BvhData[node_dw + 14u]), asfloat(BvhData[node_dw + 15u]), asfloat(BvhData[node_dw + 16u]), asfloat(BvhData[node_dw + 17u]));\n"
       << "                    float3 obj_orig = float3(dot(r0.xyz, rin.origin) + r0.w, dot(r1.xyz, rin.origin) + r1.w, dot(r2.xyz, rin.origin) + r2.w);\n"
       << "                    float3 obj_dir  = float3(dot(r0.xyz, rin.direction), dot(r1.xyz, rin.direction), dot(r2.xyz, rin.direction));\n"
       << "                    int blas_sp = 0;\n"
       << "                    blas_stack[blas_sp++] = blas_root;\n"
       << "                    [loop] while (blas_sp > 0 && !done) {\n"
       << "                        uint bptr = blas_stack[--blas_sp];\n"
       << "                        uint btype = bptr & 7u;\n"
       << "                        uint bdw = blas_base + ((bptr & 0xFFFFFFF8u) << 1u);\n"
       << "                        if (btype == 4u || btype == 5u) {\n"
       << "                            uint bhp[4];\n"
       << "                            float bht[4];\n"
       << "                            int bhc = 0;\n"
       << "                            [unroll] for (uint c = 0u; c < 4u; ++c) {\n"
       << "                                uint cptr = 0xFFFFFFFFu;\n"
       << "                                float3 bmin = float3(0.0f, 0.0f, 0.0f);\n"
       << "                                float3 bmax = float3(0.0f, 0.0f, 0.0f);\n"
       << "                                DecodeBoxChild(blas_base, bptr, c, cptr, bmin, bmax);\n"
       << "                                if (cptr != 0xFFFFFFFFu) {\n"
       << "                                    float tn = 0.0f;\n"
       << "                                    if (IntersectAabbSlab(bmin, bmax, obj_orig, obj_dir, rin.t_min, hout.t, tn)) {\n"
       << "                                        bhp[bhc] = cptr;\n"
       << "                                        bht[bhc] = tn;\n"
       << "                                        bhc++;\n"
       << "                                    }\n"
       << "                                }\n"
       << "                            }\n"
       << "                            for (int i = 0; i < bhc; ++i) {\n"
       << "                                for (int j = i + 1; j < bhc; ++j) {\n"
       << "                                    if (bht[j] > bht[i]) {\n"
       << "                                        float ft = bht[i]; bht[i] = bht[j]; bht[j] = ft;\n"
       << "                                        uint fp = bhp[i]; bhp[i] = bhp[j]; bhp[j] = fp;\n"
       << "                                    }\n"
       << "                                }\n"
       << "                            }\n"
       << "                            for (int k = 0; k < bhc; ++k) {\n"
       << "                                if (blas_sp < " << stack_depth << ") { blas_stack[blas_sp++] = bhp[k]; }\n"
       << "                            }\n"
       << "                        } else if (btype <= 3u) {\n"
       << "                            uint tri_id = BvhData[bdw + 12u];\n"
       << "                            uint tri_count = min(2u, tri_id & 3u);\n"
       << "                            uint geom_packed = BvhData[bdw + 14u];\n"
       << "                            uint geom_idx = geom_packed & 0x00FFFFFFu;\n"
       << "                            uint geom_flags = (geom_packed >> 24u) & 0xFFu;\n"
       << "                            bool is_op = (geom_flags & 1u) != 0u;\n"
       << "                            bool eff_op = (((ray_flags & 1u) != 0u) || ((inst_flags & 4u) != 0u)) ? true :\n"
       << "                                          ((((ray_flags & 2u) != 0u) || ((inst_flags & 8u) != 0u)) ? false : is_op);\n"
       << "                            bool culled = (((ray_flags & 0x40u) != 0u) && eff_op) || (((ray_flags & 0x80u) != 0u) && !eff_op);\n"
       << "                            if (!culled) {\n"
       << "                                float3 v0 = float3(asfloat(BvhData[bdw + 0u]), asfloat(BvhData[bdw + 1u]), asfloat(BvhData[bdw + 2u]));\n"
       << "                                float3 v1 = float3(asfloat(BvhData[bdw + 3u]), asfloat(BvhData[bdw + 4u]), asfloat(BvhData[bdw + 5u]));\n"
       << "                                float3 v2 = float3(asfloat(BvhData[bdw + 6u]), asfloat(BvhData[bdw + 7u]), asfloat(BvhData[bdw + 8u]));\n"
       << "                                float t = 0.0f; float u = 0.0f; float v = 0.0f; uint ff = 1u;\n"
       << "                                if (tri_count >= 1u && IntersectTri(v0, v1, v2, obj_orig, obj_dir, rin.t_min, hout.t, ray_flags, inst_flags, t, u, v, ff)) {\n"
       << "                                    hout.committed_status = 1u; hout.t = t; hout.bary_u = u; hout.bary_v = v;\n"
       << "                                    hout.primitive_index = BvhData[bdw + 13u];\n"
       << "                                    hout.instance_index = inst_idx; hout.instance_id = inst_id;\n"
       << "                                    hout.geometry_index = geom_idx; hout.sbt_offset = sbt_off; hout.front_face = ff;\n"
       << "                                    if (accept_first) { done = true; }\n"
       << "                                }\n"
       << "                                if (tri_count >= 2u && !done) {\n"
       << "                                    float3 v3 = float3(asfloat(BvhData[bdw + 9u]), asfloat(BvhData[bdw + 10u]), asfloat(BvhData[bdw + 11u]));\n"
       << "                                    if (IntersectTri(v1, v2, v3, obj_orig, obj_dir, rin.t_min, hout.t, ray_flags, inst_flags, t, u, v, ff)) {\n"
       << "                                        hout.committed_status = 1u; hout.t = t; hout.bary_u = u; hout.bary_v = v;\n"
       << "                                        hout.primitive_index = BvhData[bdw + 15u];\n"
       << "                                        hout.instance_index = inst_idx; hout.instance_id = inst_id;\n"
       << "                                        hout.geometry_index = geom_idx; hout.sbt_offset = sbt_off; hout.front_face = ff;\n"
       << "                                        if (accept_first) { done = true; }\n"
       << "                                    }\n"
       << "                                }\n"
       << "                            }\n"
       << "                        }\n"
       << "                    }\n"
       << "                }\n"
       << "            }\n"
       << "        }\n"
       << "    }\n\n"
       << "    RayOutputs[tid.x] = hout;\n"
       << "}\n";

    out_hlsl = ss.str();
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// 3. Real Vulkan SPV_KHR_ray_query SPIR-V 1.4 Binary Emission
//    Validated against Khronos spirv-val --target-env vulkan1.2
// ---------------------------------------------------------------------------
Status EmitRdna2RayQuerySpirvKhr(
    const Rdna2BvhIntersectDesc& desc,
    std::vector<uint32_t>& out_spirv) {
    out_spirv.clear();
    const uint32_t tg_x = std::clamp<uint32_t>(desc.threadgroup_x, 1u, 1024u);

    uint32_t next_id = 1u;
    auto alloc_id = [&]() -> uint32_t { return next_id++; };

    // Reserve key IDs
    const uint32_t fn_main = alloc_id();
    const uint32_t t_void = alloc_id();
    const uint32_t t_fn_void = alloc_id();
    const uint32_t t_bool = alloc_id();
    const uint32_t t_u32 = alloc_id();
    const uint32_t t_f32 = alloc_id();
    const uint32_t t_v2f32 = alloc_id();
    const uint32_t t_v3f32 = alloc_id();
    const uint32_t t_v4f32 = alloc_id();
    const uint32_t t_v3u32 = alloc_id();
    const uint32_t t_v4u32 = alloc_id();

    const uint32_t t_as = alloc_id();
    const uint32_t t_rq = alloc_id();
    const uint32_t t_ptr_uc_as = alloc_id();
    const uint32_t t_ptr_fn_rq = alloc_id();
    const uint32_t t_ptr_in_v3u32 = alloc_id();
    const uint32_t t_ptr_in_u32 = alloc_id();

    // RayInput struct { vec4 origin_tmin; vec4 dir_tmax; uvec4 flags_mask; } (48B)
    const uint32_t t_ray_struct = alloc_id();
    const uint32_t t_rta_ray = alloc_id();
    const uint32_t t_ray_buf = alloc_id();
    const uint32_t t_ptr_sb_ray_buf = alloc_id();
    const uint32_t t_ptr_sb_v4f32 = alloc_id();
    const uint32_t t_ptr_sb_v4u32 = alloc_id();

    // RayHitOutput struct { u32 status, f32 t, f32 u, f32 v, u32 prim, inst_idx, inst_id, geom, sbt, ff, p0, p1 } (48B)
    const uint32_t t_hit_struct = alloc_id();
    const uint32_t t_rta_hit = alloc_id();
    const uint32_t t_hit_buf = alloc_id();
    const uint32_t t_ptr_sb_hit_buf = alloc_id();
    const uint32_t t_ptr_sb_u32 = alloc_id();
    const uint32_t t_ptr_sb_f32 = alloc_id();

    // Params UBO { u32 ray_count; u32 default_flags; u32 default_mask; u32 op_mode; }
    const uint32_t t_params_buf = alloc_id();
    const uint32_t t_ptr_uni_params = alloc_id();
    const uint32_t t_ptr_uni_u32 = alloc_id();

    // Global variables
    const uint32_t var_gid = alloc_id();
    const uint32_t var_tlas = alloc_id();
    const uint32_t var_rays = alloc_id();
    const uint32_t var_hits = alloc_id();
    const uint32_t var_params = alloc_id();

    // Constants
    const uint32_t c_u32_0 = alloc_id();
    const uint32_t c_u32_1 = alloc_id();
    const uint32_t c_u32_2 = alloc_id();
    const uint32_t c_u32_3 = alloc_id();
    const uint32_t c_u32_4 = alloc_id();
    const uint32_t c_u32_5 = alloc_id();
    const uint32_t c_u32_6 = alloc_id();
    const uint32_t c_u32_7 = alloc_id();
    const uint32_t c_u32_8 = alloc_id();
    const uint32_t c_u32_9 = alloc_id();
    const uint32_t c_u32_10 = alloc_id();
    const uint32_t c_u32_11 = alloc_id();
    const uint32_t c_u32_invalid = alloc_id();
    const uint32_t c_u32_desc_flags = alloc_id();
    const uint32_t c_u32_desc_mask = alloc_id();
    const uint32_t c_f32_0 = alloc_id();

    std::vector<uint32_t> sec_caps;
    std::vector<uint32_t> sec_exts;
    std::vector<uint32_t> sec_mem;
    std::vector<uint32_t> sec_entry;
    std::vector<uint32_t> sec_decs;
    std::vector<uint32_t> sec_types;
    std::vector<uint32_t> sec_fn;

    // 1. Capabilities
    EmitInst(sec_caps, SpvOpCapability, {kSpvCapShader});
    EmitInst(sec_caps, SpvOpCapability, {kSpvCapRayQueryKHR});

    // 2. Extensions
    {
        std::vector<uint32_t> ext_ops;
        AppendSpvString(ext_ops, "SPV_KHR_ray_query");
        EmitInstVec(sec_exts, SpvOpExtension, ext_ops);
    }

    // 3. Memory model
    EmitInst(sec_mem, SpvOpMemoryModel, {kSpvAddrLogical, kSpvMemGlsl450});

    // 4. Entry point & Execution mode (SPIR-V 1.4 requires ALL used global variables in interface)
    {
        std::vector<uint32_t> ep_ops = {kSpvExecModelGlCompute, fn_main};
        AppendSpvString(ep_ops, "main");
        ep_ops.push_back(var_gid);
        ep_ops.push_back(var_tlas);
        ep_ops.push_back(var_rays);
        ep_ops.push_back(var_hits);
        ep_ops.push_back(var_params);
        EmitInstVec(sec_entry, SpvOpEntryPoint, ep_ops);
        EmitInst(sec_entry, SpvOpExecutionMode, {fn_main, kSpvExecModeLocalSize, tg_x, 1u, 1u});
    }

    // 5. Decorations
    EmitInst(sec_decs, SpvOpDecorate, {var_gid, kSpvDecBuiltIn, kSpvBuiltInGlobalInvocationId});
    EmitInst(sec_decs, SpvOpDecorate, {var_tlas, kSpvDecDescriptorSet, 0u});
    EmitInst(sec_decs, SpvOpDecorate, {var_tlas, kSpvDecBinding, 0u});
    EmitInst(sec_decs, SpvOpDecorate, {var_rays, kSpvDecDescriptorSet, 0u});
    EmitInst(sec_decs, SpvOpDecorate, {var_rays, kSpvDecBinding, 1u});
    EmitInst(sec_decs, SpvOpDecorate, {var_rays, kSpvDecNonWritable});
    EmitInst(sec_decs, SpvOpDecorate, {var_hits, kSpvDecDescriptorSet, 0u});
    EmitInst(sec_decs, SpvOpDecorate, {var_hits, kSpvDecBinding, 2u});
    EmitInst(sec_decs, SpvOpDecorate, {var_params, kSpvDecDescriptorSet, 0u});
    EmitInst(sec_decs, SpvOpDecorate, {var_params, kSpvDecBinding, 3u});

    // RayInput struct & buffer layout
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_ray_struct, 0u, kSpvDecOffset, 0u});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_ray_struct, 1u, kSpvDecOffset, 16u});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_ray_struct, 2u, kSpvDecOffset, 32u});
    EmitInst(sec_decs, SpvOpDecorate, {t_rta_ray, kSpvDecArrayStride, 48u});
    EmitInst(sec_decs, SpvOpDecorate, {t_ray_buf, kSpvDecBlock});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_ray_buf, 0u, kSpvDecOffset, 0u});

    // RayHitOutput struct & buffer layout
    for (uint32_t m = 0u; m < 12u; ++m) {
        EmitInst(sec_decs, SpvOpMemberDecorate, {t_hit_struct, m, kSpvDecOffset, m * 4u});
    }
    EmitInst(sec_decs, SpvOpDecorate, {t_rta_hit, kSpvDecArrayStride, 48u});
    EmitInst(sec_decs, SpvOpDecorate, {t_hit_buf, kSpvDecBlock});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_hit_buf, 0u, kSpvDecOffset, 0u});

    // Params UBO layout
    EmitInst(sec_decs, SpvOpDecorate, {t_params_buf, kSpvDecBlock});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_params_buf, 0u, kSpvDecOffset, 0u});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_params_buf, 1u, kSpvDecOffset, 4u});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_params_buf, 2u, kSpvDecOffset, 8u});
    EmitInst(sec_decs, SpvOpMemberDecorate, {t_params_buf, 3u, kSpvDecOffset, 12u});

    // 6. Types, Constants, Global Variables
    EmitInst(sec_types, SpvOpTypeVoid, {t_void});
    EmitInst(sec_types, SpvOpTypeFunction, {t_fn_void, t_void});
    EmitInst(sec_types, SpvOpTypeBool, {t_bool});
    EmitInst(sec_types, SpvOpTypeInt, {t_u32, 32u, 0u});
    EmitInst(sec_types, SpvOpTypeFloat, {t_f32, 32u});
    EmitInst(sec_types, SpvOpTypeVector, {t_v2f32, t_f32, 2u});
    EmitInst(sec_types, SpvOpTypeVector, {t_v3f32, t_f32, 3u});
    EmitInst(sec_types, SpvOpTypeVector, {t_v4f32, t_f32, 4u});
    EmitInst(sec_types, SpvOpTypeVector, {t_v3u32, t_u32, 3u});
    EmitInst(sec_types, SpvOpTypeVector, {t_v4u32, t_u32, 4u});

    EmitInst(sec_types, SpvOpTypeAccelerationStructureKHR, {t_as});
    EmitInst(sec_types, SpvOpTypeRayQueryKHR, {t_rq});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_uc_as, kSpvScUniformConstant, t_as});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_fn_rq, kSpvScFunction, t_rq});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, kSpvScInput, t_v3u32});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, kSpvScInput, t_u32});

    EmitInst(sec_types, SpvOpTypeStruct, {t_ray_struct, t_v4f32, t_v4f32, t_v4u32});
    EmitInst(sec_types, SpvOpTypeRuntimeArray, {t_rta_ray, t_ray_struct});
    EmitInst(sec_types, SpvOpTypeStruct, {t_ray_buf, t_rta_ray});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_ray_buf, kSpvScStorageBuffer, t_ray_buf});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_v4f32, kSpvScStorageBuffer, t_v4f32});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_v4u32, kSpvScStorageBuffer, t_v4u32});

    EmitInst(sec_types, SpvOpTypeStruct,
             {t_hit_struct, t_u32, t_f32, t_f32, t_f32, t_u32, t_u32, t_u32, t_u32, t_u32, t_u32, t_u32, t_u32});
    EmitInst(sec_types, SpvOpTypeRuntimeArray, {t_rta_hit, t_hit_struct});
    EmitInst(sec_types, SpvOpTypeStruct, {t_hit_buf, t_rta_hit});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_hit_buf, kSpvScStorageBuffer, t_hit_buf});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_u32, kSpvScStorageBuffer, t_u32});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_sb_f32, kSpvScStorageBuffer, t_f32});

    EmitInst(sec_types, SpvOpTypeStruct, {t_params_buf, t_u32, t_u32, t_u32, t_u32});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_uni_params, kSpvScUniform, t_params_buf});
    EmitInst(sec_types, SpvOpTypePointer, {t_ptr_uni_u32, kSpvScUniform, t_u32});

    // Constants
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_0, 0u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_1, 1u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_2, 2u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_3, 3u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_4, 4u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_5, 5u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_6, 6u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_7, 7u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_8, 8u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_9, 9u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_10, 10u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_11, 11u});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_invalid, 0xFFFFFFFFu});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_desc_flags, desc.ray_flags});
    EmitInst(sec_types, SpvOpConstant, {t_u32, c_u32_desc_mask, desc.instance_mask & 0xFFu});
    EmitInst(sec_types, SpvOpConstant, {t_f32, c_f32_0, FloatToU32Bits(0.0f)});

    // Global variables
    EmitInst(sec_types, SpvOpVariable, {t_ptr_in_v3u32, var_gid, kSpvScInput});
    EmitInst(sec_types, SpvOpVariable, {t_ptr_uc_as, var_tlas, kSpvScUniformConstant});
    EmitInst(sec_types, SpvOpVariable, {t_ptr_sb_ray_buf, var_rays, kSpvScStorageBuffer});
    EmitInst(sec_types, SpvOpVariable, {t_ptr_sb_hit_buf, var_hits, kSpvScStorageBuffer});
    EmitInst(sec_types, SpvOpVariable, {t_ptr_uni_params, var_params, kSpvScUniform});

    // 7. Function main
    const uint32_t lbl_entry = alloc_id();
    const uint32_t lbl_in_bounds = alloc_id();
    const uint32_t lbl_loop_hdr = alloc_id();
    const uint32_t lbl_loop_cont = alloc_id();
    const uint32_t lbl_loop_merge = alloc_id();
    const uint32_t lbl_hit = alloc_id();
    const uint32_t lbl_miss = alloc_id();
    const uint32_t lbl_hit_merge = alloc_id();
    const uint32_t lbl_exit = alloc_id();

    const uint32_t var_rq = alloc_id();

    EmitInst(sec_fn, SpvOpFunction, {t_void, fn_main, 0u, t_fn_void});
    EmitInst(sec_fn, SpvOpLabel, {lbl_entry});
    // All Function-scope variables must appear at the start of the entry block
    EmitInst(sec_fn, SpvOpVariable, {t_ptr_fn_rq, var_rq, kSpvScFunction});

    const uint32_t ptr_gidx = alloc_id();
    const uint32_t gidx = alloc_id();
    const uint32_t ptr_rc = alloc_id();
    const uint32_t ray_count = alloc_id();
    const uint32_t oob = alloc_id();
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_in_u32, ptr_gidx, var_gid, c_u32_0});
    EmitInst(sec_fn, SpvOpLoad, {t_u32, gidx, ptr_gidx});
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_uni_u32, ptr_rc, var_params, c_u32_0});
    EmitInst(sec_fn, SpvOpLoad, {t_u32, ray_count, ptr_rc});
    EmitInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, oob, gidx, ray_count});
    EmitInst(sec_fn, SpvOpSelectionMerge, {lbl_exit, 0u});
    EmitInst(sec_fn, SpvOpBranchConditional, {oob, lbl_exit, lbl_in_bounds});

    // In-bounds block: load ray, initialize RayQuery, traverse
    EmitInst(sec_fn, SpvOpLabel, {lbl_in_bounds});
    const uint32_t ptr_r0 = alloc_id();
    const uint32_t ptr_r1 = alloc_id();
    const uint32_t ptr_r2 = alloc_id();
    const uint32_t r0 = alloc_id();
    const uint32_t r1 = alloc_id();
    const uint32_t r2 = alloc_id();
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_v4f32, ptr_r0, var_rays, c_u32_0, gidx, c_u32_0});
    EmitInst(sec_fn, SpvOpLoad, {t_v4f32, r0, ptr_r0});
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_v4f32, ptr_r1, var_rays, c_u32_0, gidx, c_u32_1});
    EmitInst(sec_fn, SpvOpLoad, {t_v4f32, r1, ptr_r1});
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_v4u32, ptr_r2, var_rays, c_u32_0, gidx, c_u32_2});
    EmitInst(sec_fn, SpvOpLoad, {t_v4u32, r2, ptr_r2});

    const uint32_t origin = alloc_id();
    const uint32_t t_min = alloc_id();
    const uint32_t dir = alloc_id();
    const uint32_t t_max = alloc_id();
    const uint32_t in_flags = alloc_id();
    const uint32_t in_mask = alloc_id();
    EmitInst(sec_fn, SpvOpVectorShuffle, {t_v3f32, origin, r0, r0, 0u, 1u, 2u});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_f32, t_min, r0, 3u});
    EmitInst(sec_fn, SpvOpVectorShuffle, {t_v3f32, dir, r1, r1, 0u, 1u, 2u});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_f32, t_max, r1, 3u});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_u32, in_flags, r2, 0u});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_u32, in_mask, r2, 1u});

    const uint32_t ptr_df = alloc_id();
    const uint32_t def_flags = alloc_id();
    const uint32_t ptr_dm = alloc_id();
    const uint32_t def_mask = alloc_id();
    const uint32_t flags0 = alloc_id();
    const uint32_t flags_all = alloc_id();
    const uint32_t mask0 = alloc_id();
    const uint32_t mask_all = alloc_id();
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_uni_u32, ptr_df, var_params, c_u32_1});
    EmitInst(sec_fn, SpvOpLoad, {t_u32, def_flags, ptr_df});
    EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_uni_u32, ptr_dm, var_params, c_u32_2});
    EmitInst(sec_fn, SpvOpLoad, {t_u32, def_mask, ptr_dm});
    EmitInst(sec_fn, SpvOpBitwiseOr, {t_u32, flags0, in_flags, def_flags});
    EmitInst(sec_fn, SpvOpBitwiseOr, {t_u32, flags_all, flags0, c_u32_desc_flags});
    EmitInst(sec_fn, SpvOpBitwiseAnd, {t_u32, mask0, in_mask, def_mask});
    EmitInst(sec_fn, SpvOpBitwiseAnd, {t_u32, mask_all, mask0, c_u32_desc_mask});

    const uint32_t as_val = alloc_id();
    EmitInst(sec_fn, SpvOpLoad, {t_as, as_val, var_tlas});
    EmitInst(sec_fn, SpvOpRayQueryInitializeKHR,
             {var_rq, as_val, flags_all, mask_all, origin, t_min, dir, t_max});
    EmitInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

    // Traversal loop: while (OpRayQueryProceedKHR(rq)) {}
    EmitInst(sec_fn, SpvOpLabel, {lbl_loop_hdr});
    const uint32_t proceed = alloc_id();
    EmitInst(sec_fn, SpvOpRayQueryProceedKHR, {t_bool, proceed, var_rq});
    EmitInst(sec_fn, SpvOpLoopMerge, {lbl_loop_merge, lbl_loop_cont, 0u});
    EmitInst(sec_fn, SpvOpBranchConditional, {proceed, lbl_loop_cont, lbl_loop_merge});

    EmitInst(sec_fn, SpvOpLabel, {lbl_loop_cont});
    EmitInst(sec_fn, SpvOpBranch, {lbl_loop_hdr});

    // Post-traversal: query committed intersection
    EmitInst(sec_fn, SpvOpLabel, {lbl_loop_merge});
    const uint32_t comm_type = alloc_id();
    const uint32_t is_tri_hit = alloc_id();
    // Intersection = c_u32_1 (RayQueryCommittedIntersectionKHR)
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionTypeKHR, {t_u32, comm_type, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpIEqual, {t_bool, is_tri_hit, comm_type, c_u32_1});
    EmitInst(sec_fn, SpvOpSelectionMerge, {lbl_hit_merge, 0u});
    EmitInst(sec_fn, SpvOpBranchConditional, {is_tri_hit, lbl_hit, lbl_miss});

    auto emit_store_hit_u32 = [&](uint32_t field_const_id, uint32_t val_id) {
        const uint32_t p = alloc_id();
        EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, p, var_hits, c_u32_0, gidx, field_const_id});
        EmitInst(sec_fn, SpvOpStore, {p, val_id});
    };
    auto emit_store_hit_f32 = [&](uint32_t field_const_id, uint32_t val_id) {
        const uint32_t p = alloc_id();
        EmitInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_f32, p, var_hits, c_u32_0, gidx, field_const_id});
        EmitInst(sec_fn, SpvOpStore, {p, val_id});
    };

    // Hit branch
    EmitInst(sec_fn, SpvOpLabel, {lbl_hit});
    const uint32_t hit_t = alloc_id();
    const uint32_t bary = alloc_id();
    const uint32_t bary_u = alloc_id();
    const uint32_t bary_v = alloc_id();
    const uint32_t prim_idx = alloc_id();
    const uint32_t inst_idx = alloc_id();
    const uint32_t inst_id = alloc_id();
    const uint32_t geom_idx = alloc_id();
    const uint32_t sbt_off = alloc_id();
    const uint32_t ff_bool = alloc_id();
    const uint32_t ff_u32 = alloc_id();

    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionTKHR, {t_f32, hit_t, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionBarycentricsKHR, {t_v2f32, bary, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_f32, bary_u, bary, 0u});
    EmitInst(sec_fn, SpvOpCompositeExtract, {t_f32, bary_v, bary, 1u});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionPrimitiveIndexKHR, {t_u32, prim_idx, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionInstanceIdKHR, {t_u32, inst_idx, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionInstanceCustomIndexKHR, {t_u32, inst_id, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionGeometryIndexKHR, {t_u32, geom_idx, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionInstanceSbtOffsetKHR, {t_u32, sbt_off, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpRayQueryGetIntersectionFrontFaceKHR, {t_bool, ff_bool, var_rq, c_u32_1});
    EmitInst(sec_fn, SpvOpSelect, {t_u32, ff_u32, ff_bool, c_u32_1, c_u32_0});

    emit_store_hit_u32(c_u32_0, c_u32_1);
    emit_store_hit_f32(c_u32_1, hit_t);
    emit_store_hit_f32(c_u32_2, bary_u);
    emit_store_hit_f32(c_u32_3, bary_v);
    emit_store_hit_u32(c_u32_4, prim_idx);
    emit_store_hit_u32(c_u32_5, inst_idx);
    emit_store_hit_u32(c_u32_6, inst_id);
    emit_store_hit_u32(c_u32_7, geom_idx);
    emit_store_hit_u32(c_u32_8, sbt_off);
    emit_store_hit_u32(c_u32_9, ff_u32);
    emit_store_hit_u32(c_u32_10, c_u32_0);
    emit_store_hit_u32(c_u32_11, c_u32_0);
    EmitInst(sec_fn, SpvOpBranch, {lbl_hit_merge});

    // Miss branch
    EmitInst(sec_fn, SpvOpLabel, {lbl_miss});
    emit_store_hit_u32(c_u32_0, c_u32_0);
    emit_store_hit_f32(c_u32_1, t_max);
    emit_store_hit_f32(c_u32_2, c_f32_0);
    emit_store_hit_f32(c_u32_3, c_f32_0);
    emit_store_hit_u32(c_u32_4, c_u32_invalid);
    emit_store_hit_u32(c_u32_5, c_u32_invalid);
    emit_store_hit_u32(c_u32_6, c_u32_invalid);
    emit_store_hit_u32(c_u32_7, c_u32_0);
    emit_store_hit_u32(c_u32_8, c_u32_0);
    emit_store_hit_u32(c_u32_9, c_u32_1);
    emit_store_hit_u32(c_u32_10, c_u32_0);
    emit_store_hit_u32(c_u32_11, c_u32_0);
    EmitInst(sec_fn, SpvOpBranch, {lbl_hit_merge});

    EmitInst(sec_fn, SpvOpLabel, {lbl_hit_merge});
    EmitInst(sec_fn, SpvOpBranch, {lbl_exit});

    EmitInst(sec_fn, SpvOpLabel, {lbl_exit});
    EmitInst(sec_fn, SpvOpReturn, {});
    EmitInst(sec_fn, SpvOpFunctionEnd, {});

    // Assemble SPIR-V 1.4 binary stream
    out_spirv.reserve(5u + sec_caps.size() + sec_exts.size() + sec_mem.size() +
                      sec_entry.size() + sec_decs.size() + sec_types.size() + sec_fn.size());
    out_spirv.push_back(kSpvMagic);
    out_spirv.push_back(kSpvVersion14);
    out_spirv.push_back(kSpvGeneratorOpenScarlett);
    out_spirv.push_back(next_id);
    out_spirv.push_back(0u);

    auto append = [&](const std::vector<uint32_t>& s) {
        out_spirv.insert(out_spirv.end(), s.begin(), s.end());
    };
    append(sec_caps);
    append(sec_exts);
    append(sec_mem);
    append(sec_entry);
    append(sec_decs);
    append(sec_types);
    append(sec_fn);

    return Status::Ok();
}

Status InspectRdna2RayQuerySpirv(
    const std::vector<uint32_t>& spirv,
    Rdna2RayQuerySpirvInspectInfo& out_info) {
    out_info = Rdna2RayQuerySpirvInspectInfo{};
    if (spirv.size() < 5u || spirv[0] != kSpvMagic) {
        return Status::Error(StatusCode::ParseError, "Invalid SPIR-V header or magic");
    }
    out_info.valid_magic = true;
    out_info.version = spirv[1];

    size_t pos = 5u;
    while (pos < spirv.size()) {
        const uint16_t wc = static_cast<uint16_t>((spirv[pos] >> 16u) & 0xFFFFu);
        const uint16_t op = static_cast<uint16_t>(spirv[pos] & 0xFFFFu);
        if (wc == 0u || pos + wc > spirv.size()) {
            return Status::Error(StatusCode::ParseError, "Truncated or zero-length SPIR-V instruction");
        }
        if (op == SpvOpCapability && wc == 2u && spirv[pos + 1u] == kSpvCapRayQueryKHR) {
            out_info.has_ray_query_cap = true;
        } else if (op == SpvOpExtension && wc >= 2u) {
            if (DecodeSpvString(spirv, pos + 1u, pos + wc) == "SPV_KHR_ray_query") {
                out_info.has_ray_query_ext = true;
            }
        } else if (op == SpvOpTypeAccelerationStructureKHR) {
            out_info.has_accel_struct_type = true;
        } else if (op == SpvOpTypeRayQueryKHR) {
            out_info.has_ray_query_type = true;
        } else if (op == SpvOpRayQueryInitializeKHR) {
            out_info.has_initialize = true;
        } else if (op == SpvOpRayQueryProceedKHR) {
            out_info.has_proceed = true;
        } else if (op == SpvOpRayQueryGetIntersectionTypeKHR) {
            out_info.has_get_intersection_type = true;
        } else if (op == SpvOpRayQueryGetIntersectionTKHR) {
            out_info.has_get_intersection_t = true;
        } else if (op == SpvOpRayQueryGetIntersectionBarycentricsKHR) {
            out_info.has_get_intersection_barycentrics = true;
        } else if (op == SpvOpRayQueryGetIntersectionPrimitiveIndexKHR) {
            out_info.has_get_intersection_prim_index = true;
        } else if (op == SpvOpRayQueryGetIntersectionInstanceIdKHR) {
            out_info.has_get_intersection_instance_id = true;
        }
        pos += wc;
    }
    return Status::Ok();
}

Status TranslateRdna2BvhIntersectToRayQuery(
    const Rdna2BvhIntersectDesc& desc,
    Rdna2RayQueryTranslationResult& out_result) {
    out_result = Rdna2RayQueryTranslationResult{};
    out_result.source_op = desc.op;

    if (desc.emit_dxr11_hlsl) {
        Status st = EmitRdna2RayQueryDxr11Hlsl(desc, out_result.dxr11_hlsl);
        if (!st.ok()) return st;
        out_result.dxr11_target = "cs_6_5";
    }
    if (desc.emit_compute_bvh4_fallback_hlsl) {
        Status st = EmitRdna2ComputeBvh4FallbackHlsl(desc, out_result.compute_bvh4_hlsl);
        if (!st.ok()) return st;
        out_result.compute_bvh4_target = "cs_6_5";
    }
    if (desc.emit_spirv_ray_query) {
        Status st = EmitRdna2RayQuerySpirvKhr(desc, out_result.spirv_ray_query_words);
        if (!st.ok()) return st;
    }

    CallTelemetry::Global().Record("rdna2_rayquery", "TranslateRdna2BvhIntersectToRayQuery", CallOutcome::Ok, 0);
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// 4. Title-List XboxBvhBuilder Conversion (Xbox BVH4 <-> PC Host BLAS/TLAS)
// ---------------------------------------------------------------------------
Status ConvertXboxBlasToHostAndBvh4(
    const XboxBvhBlasBuildInput& input,
    uint64_t xbox_base_va,
    XboxBvhConvertedBlas& out_blas) {
    out_blas = XboxBvhConvertedBlas{};
    if (xbox_base_va == 0ull || (xbox_base_va & (kDxrAsByteAlignment - 1u)) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Xbox BLAS base VA must be non-zero and 256-byte aligned");
    }
    if (input.geometries.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Xbox BLAS input requires at least one geometry");
    }

    std::vector<RtTriangle> tris;
    for (const auto& geom : input.geometries) {
        if (!geom.indices.empty()) {
            if ((geom.indices.size() % 3u) != 0u) {
                return Status::Error(StatusCode::InvalidArgument, "Geometry index count must be a multiple of 3");
            }
            for (size_t i = 0; i < geom.indices.size(); i += 3u) {
                const uint32_t i0 = geom.indices[i + 0u];
                const uint32_t i1 = geom.indices[i + 1u];
                const uint32_t i2 = geom.indices[i + 2u];
                if (i0 >= geom.vertices.size() || i1 >= geom.vertices.size() || i2 >= geom.vertices.size()) {
                    return Status::Error(StatusCode::InvalidArgument, "Geometry triangle index out of vertex bounds");
                }
                RtTriangle t{};
                t.v0 = geom.vertices[i0];
                t.v1 = geom.vertices[i1];
                t.v2 = geom.vertices[i2];
                t.color = geom.default_color;
                t.reflectivity = geom.default_reflectivity;
                if (geom.has_transform) {
                    t = TransformTriangle3x4(geom.transform, t);
                }
                tris.push_back(t);
            }
        } else {
            if (geom.vertices.size() < 3u || (geom.vertices.size() % 3u) != 0u) {
                return Status::Error(StatusCode::InvalidArgument,
                                     "Non-indexed geometry vertex count must be a non-zero multiple of 3");
            }
            for (size_t i = 0; i < geom.vertices.size(); i += 3u) {
                RtTriangle t{};
                t.v0 = geom.vertices[i + 0u];
                t.v1 = geom.vertices[i + 1u];
                t.v2 = geom.vertices[i + 2u];
                t.color = geom.default_color;
                t.reflectivity = geom.default_reflectivity;
                if (geom.has_transform) {
                    t = TransformTriangle3x4(geom.transform, t);
                }
                tris.push_back(t);
            }
        }
    }

    if (tris.empty()) {
        return Status::Error(StatusCode::RaytracingError, "Xbox BLAS conversion produced 0 triangles");
    }

    BvhAccelerationStructure sah_bvh;
    Status st = sah_bvh.Build(tris);
    if (!st.ok()) {
        return st;
    }

    Rdna2BvhEncodeOptions enc_opts{};
    enc_opts.use_box16 = input.use_box16;
    enc_opts.build_flags = input.build_flags;
    enc_opts.geometry_index = input.geometries[0].geometry_index;
    enc_opts.geometry_flags = input.geometries[0].is_opaque ? 1u : 0u;

    std::vector<uint8_t> bvh4_buf;
    Rdna2BvhHeader hdr{};
    st = EncodeBvh2ToRdna2Bvh4(sah_bvh, enc_opts, bvh4_buf, &hdr);
    if (!st.ok()) {
        return st;
    }

    st = ValidateRdna2Bvh4(bvh4_buf.data(), bvh4_buf.size());
    if (!st.ok()) {
        return st;
    }

    out_blas.xbox_gpu_va = xbox_base_va;
    out_blas.host_triangles = std::move(tris);
    out_blas.xbox_bvh4_buffer = std::move(bvh4_buf);
    out_blas.bvh4_header = hdr;
    out_blas.host_build_flags = input.build_flags;
    out_blas.geometry_count = static_cast<uint32_t>(input.geometries.size());
    out_blas.host_prebuild.result_data_max_size_bytes =
        AlignUpU64(out_blas.xbox_bvh4_buffer.size(), kDxrAsByteAlignment);
    out_blas.host_prebuild.scratch_data_size_bytes =
        AlignUpU64(std::max<uint64_t>(256ull, out_blas.host_triangles.size() * 160ull), kDxrAsByteAlignment);
    out_blas.host_prebuild.update_scratch_data_size_bytes =
        ((input.build_flags & kDxrBuildAllowUpdate) != 0u)
            ? AlignUpU64(std::max<uint64_t>(256ull, out_blas.host_triangles.size() * 64ull), kDxrAsByteAlignment)
            : 0ull;
    out_blas.host_prebuild.from_hardware_driver = false;
    return Status::Ok();
}

Status ConvertXboxTlasToHostAndBvh4(
    const std::vector<XboxBvhTlasInstanceInput>& instances,
    const std::vector<XboxBvhConvertedBlas>& blas_list,
    uint64_t tlas_base_va,
    uint32_t build_flags,
    bool use_box16,
    XboxBvhConvertedTlas& out_tlas) {
    out_tlas = XboxBvhConvertedTlas{};
    if (tlas_base_va == 0ull || (tlas_base_va & (kDxrAsByteAlignment - 1u)) != 0ull) {
        return Status::Error(StatusCode::InvalidArgument,
                             "Xbox TLAS base VA must be non-zero and 256-byte aligned");
    }
    if (instances.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Xbox TLAS input requires at least one instance");
    }

    std::vector<DxrInstanceDesc> host_insts;
    std::vector<Rdna2BvhTlasInstanceInput> bvh4_insts;
    host_insts.reserve(instances.size());
    bvh4_insts.reserve(instances.size());

    for (const auto& in : instances) {
        const XboxBvhConvertedBlas* matched = nullptr;
        for (const auto& b : blas_list) {
            if (b.xbox_gpu_va == in.blas_xbox_va) {
                matched = &b;
                break;
            }
        }
        if (!matched) {
            return Status::Error(StatusCode::NotFound,
                                 "TLAS instance references unknown Xbox BLAS GPU VA");
        }

        DxrInstanceDesc hi{};
        hi.blas_id = in.host_blas_id;
        std::memcpy(hi.transform, in.object_to_world, sizeof(hi.transform));
        hi.instance_id = in.instance_id & 0x00FFFFFFu;
        hi.instance_mask = in.instance_mask & 0xFFu;
        hi.instance_contribution_to_hit_group_index = in.hit_group_contribution & 0x00FFFFFFu;
        hi.flags = in.instance_flags & 0xFFu;
        host_insts.push_back(hi);

        Rdna2BvhTlasInstanceInput bi{};
        bi.blas_base_va = matched->xbox_gpu_va;
        bi.blas_root_ptr = matched->bvh4_header.root_node_ptr;
        bi.blas_local_aabb = matched->bvh4_header.root_aabb;
        std::memcpy(bi.object_to_world, in.object_to_world, sizeof(bi.object_to_world));
        bi.instance_id = hi.instance_id;
        bi.instance_mask = hi.instance_mask;
        bi.sbt_offset = hi.instance_contribution_to_hit_group_index;
        bi.instance_flags = hi.flags;
        bvh4_insts.push_back(bi);
    }

    Rdna2BvhEncodeOptions enc_opts{};
    enc_opts.use_box16 = use_box16;
    enc_opts.build_flags = build_flags;

    std::vector<uint8_t> tlas_buf;
    Rdna2BvhHeader thdr{};
    Status st = BuildRdna2Bvh4Tlas(bvh4_insts, {}, enc_opts, tlas_buf, &thdr);
    if (!st.ok()) {
        return st;
    }

    st = ValidateRdna2Bvh4(tlas_buf.data(), tlas_buf.size());
    if (!st.ok()) {
        return st;
    }

    out_tlas.xbox_gpu_va = tlas_base_va;
    out_tlas.host_instances = std::move(host_insts);
    out_tlas.xbox_bvh4_buffer = std::move(tlas_buf);
    out_tlas.bvh4_header = thdr;
    out_tlas.host_build_flags = build_flags;
    out_tlas.host_prebuild.result_data_max_size_bytes =
        AlignUpU64(out_tlas.xbox_bvh4_buffer.size(), kDxrAsByteAlignment);
    out_tlas.host_prebuild.scratch_data_size_bytes =
        AlignUpU64(std::max<uint64_t>(256ull, instances.size() * 128ull), kDxrAsByteAlignment);
    out_tlas.host_prebuild.update_scratch_data_size_bytes =
        ((build_flags & kDxrBuildAllowUpdate) != 0u)
            ? AlignUpU64(std::max<uint64_t>(256ull, instances.size() * 64ull), kDxrAsByteAlignment)
            : 0ull;
    out_tlas.host_prebuild.from_hardware_driver = false;
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// 5. Live GPU RayQuery Executor (Automatic DXR 1.1 / Compute BVH4 on D3D12)
// ---------------------------------------------------------------------------
struct Rdna2RayQueryExecutor::Impl {
    mutable std::mutex mutex;
    bool initialized = false;
    bool force_compute_fallback = false;
    Rdna2RayQueryBackend selected_backend = Rdna2RayQueryBackend::CpuReference;
    DxrHardwareEngine dxr_engine;
    DxrCapabilities caps{};
    bool used_sm65_dxil = false;

#if defined(_WIN32)
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* cmd_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0ull;
    ID3D12RootSignature* root_sig = nullptr;
    ID3D12PipelineState* compute_pso = nullptr;

    Status WaitForGpu() {
        if (!queue || !fence || !fence_event) {
            return Status::Error(StatusCode::DeviceError, "RayQueryExecutor command queue/fence unavailable");
        }
        const uint64_t target = ++fence_value;
        HRESULT hr = queue->Signal(fence, target);
        if (FAILED(hr)) {
            return Status::Error(StatusCode::DeviceError, "RayQueryExecutor fence Signal failed");
        }
        if (fence->GetCompletedValue() < target) {
            hr = fence->SetEventOnCompletion(target, fence_event);
            if (FAILED(hr)) {
                return Status::Error(StatusCode::DeviceError, "RayQueryExecutor fence SetEventOnCompletion failed");
            }
            if (WaitForSingleObject(fence_event, 15000) != WAIT_OBJECT_0) {
                return Status::Error(StatusCode::DeviceError, "RayQueryExecutor GPU fence wait timed out");
            }
        }
        return Status::Ok();
    }

    Status SetupComputePipeline() {
        if (!device) {
            return Status::Error(StatusCode::DeviceError, "No ID3D12Device for compute BVH4 pipeline");
        }

        // Command queue + allocator + command list + fence
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandQueue failed");
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            !allocator) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandAllocator failed");
        }
        if (FAILED(device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             IID_PPV_ARGS(&cmd_list))) ||
            !cmd_list) {
            return Status::Error(StatusCode::DeviceError, "CreateCommandList failed");
        }
        cmd_list->Close();

        if (FAILED(device->CreateFence(0ull, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence) {
            return Status::Error(StatusCode::DeviceError, "CreateFence failed");
        }
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            return Status::Error(StatusCode::DeviceError, "CreateEventA failed");
        }

        // Root signature: b0 (4 root constants), t0 (WindowTable), t1 (BvhData), t2 (RayInputs), u0 (RayOutputs)
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0u;
        params[0].Constants.RegisterSpace = 0u;
        params[0].Constants.Num32BitValues = 4u;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0u;
        params[1].Descriptor.RegisterSpace = 0u;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 1u;
        params[2].Descriptor.RegisterSpace = 0u;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor.ShaderRegister = 2u;
        params[3].Descriptor.RegisterSpace = 0u;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 0u;
        params[4].Descriptor.RegisterSpace = 0u;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 5u;
        rs_desc.pParameters = params;
        rs_desc.NumStaticSamplers = 0u;
        rs_desc.pStaticSamplers = nullptr;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ID3DBlob* rs_blob = nullptr;
        ID3DBlob* rs_err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
        SafeReleaseCom(rs_err);
        if (FAILED(hr) || !rs_blob) {
            SafeReleaseCom(rs_blob);
            return Status::Error(StatusCode::DeviceError, "D3D12SerializeRootSignature failed");
        }
        hr = device->CreateRootSignature(0u, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_sig));
        SafeReleaseCom(rs_blob);
        if (FAILED(hr) || !root_sig) {
            return Status::Error(StatusCode::DeviceError, "CreateRootSignature failed");
        }

        // Emit compute BVH4 traversal HLSL
        Rdna2BvhIntersectDesc bvh_desc{};
        bvh_desc.threadgroup_x = 64u;
        bvh_desc.max_stack_depth = 64u;
        std::string hlsl;
        Status st = EmitRdna2ComputeBvh4FallbackHlsl(bvh_desc, hlsl);
        if (!st.ok()) {
            return st;
        }

        // Attempt SM 6.5 DXIL via portable dxc.exe first; fall back seamlessly to D3DCompile cs_5_0
        // if dxc.exe is unavailable or the host driver/WARP rejects unsigned DXIL.
        used_sm65_dxil = false;
        const std::string dxc_path = ResolvePortableDxcPath();
        if (!dxc_path.empty()) {
            std::vector<uint8_t> dxil;
            std::string diag;
            if (CompileHlslWithDxc(hlsl, "cs_6_5", "main", dxil, diag, dxc_path).ok() && !dxil.empty()) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
                pso_desc.pRootSignature = root_sig;
                pso_desc.CS.pShaderBytecode = dxil.data();
                pso_desc.CS.BytecodeLength = dxil.size();
                if (SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&compute_pso))) &&
                    compute_pso != nullptr) {
                    used_sm65_dxil = true;
                }
            }
        }

        if (!compute_pso) {
            ID3DBlob* cs_blob = nullptr;
            ID3DBlob* err_blob = nullptr;
            hr = D3DCompile(hlsl.data(), hlsl.size(), "rdna2_bvh4_fallback.hlsl", nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0u, &cs_blob, &err_blob);
            if (FAILED(hr) || !cs_blob) {
                std::string msg = "D3DCompile(cs_5_0) failed for BVH4 fallback";
                if (err_blob && err_blob->GetBufferPointer()) {
                    msg += ": ";
                    msg.append(static_cast<const char*>(err_blob->GetBufferPointer()),
                               err_blob->GetBufferSize());
                }
                SafeReleaseCom(err_blob);
                SafeReleaseCom(cs_blob);
                return Status::Error(StatusCode::ShaderError, msg);
            }
            SafeReleaseCom(err_blob);

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
            pso_desc.pRootSignature = root_sig;
            pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
            pso_desc.CS.BytecodeLength = cs_blob->GetBufferSize();
            hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&compute_pso));
            SafeReleaseCom(cs_blob);
            if (FAILED(hr) || !compute_pso) {
                return Status::Error(StatusCode::DeviceError, "CreateComputePipelineState failed");
            }
        }

        return Status::Ok();
    }
#endif

    void Cleanup() {
#if defined(_WIN32)
        SafeReleaseCom(compute_pso);
        SafeReleaseCom(root_sig);
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        SafeReleaseCom(fence);
        SafeReleaseCom(cmd_list);
        SafeReleaseCom(allocator);
        SafeReleaseCom(queue);
        SafeReleaseCom(device);
        fence_value = 0ull;
#endif
        dxr_engine.Shutdown();
        caps = DxrCapabilities{};
        selected_backend = Rdna2RayQueryBackend::CpuReference;
        used_sm65_dxil = false;
        initialized = false;
    }

    ~Impl() { Cleanup(); }
};

Rdna2RayQueryExecutor::Rdna2RayQueryExecutor() : impl_(std::make_unique<Impl>()) {}
Rdna2RayQueryExecutor::~Rdna2RayQueryExecutor() = default;

bool Rdna2RayQueryExecutor::IsInitialized() const noexcept {
    return impl_ && impl_->initialized;
}

Rdna2RayQueryBackend Rdna2RayQueryExecutor::SelectedBackend() const noexcept {
    return impl_ ? impl_->selected_backend : Rdna2RayQueryBackend::CpuReference;
}

const DxrCapabilities& Rdna2RayQueryExecutor::Capabilities() const noexcept {
    static const DxrCapabilities kEmpty{};
    return impl_ ? impl_->caps : kEmpty;
}

Status Rdna2RayQueryExecutor::Initialize(bool prefer_warp, bool force_compute_fallback) {
    Shutdown();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->force_compute_fallback = force_compute_fallback;

    DxrInitOptions dxr_opts{};
    dxr_opts.prefer_warp = prefer_warp;
    dxr_opts.force_software_path = force_compute_fallback;
    Status st = impl_->dxr_engine.Initialize(dxr_opts);
    if (!st.ok()) {
        return st;
    }
    impl_->caps = impl_->dxr_engine.Capabilities();
    impl_->selected_backend = SelectRayQueryBackend(impl_->caps, force_compute_fallback);

#if defined(_WIN32)
    IDXGIFactory6* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0u, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        impl_->Cleanup();
        return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory2 failed in RayQueryExecutor");
    }

    ID3D12Device* dev = nullptr;
    if (!prefer_warp) {
        for (UINT i = 0u;; ++i) {
            IDXGIAdapter1* cand = nullptr;
            if (factory->EnumAdapterByGpuPreference(
                    i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&cand)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!cand) continue;
            DXGI_ADAPTER_DESC1 ad{};
            if (SUCCEEDED(cand->GetDesc1(&ad)) &&
                (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0u &&
                !(ad.VendorId == 0x1414u && ad.DeviceId == 0x008Cu)) {
                if (SUCCEEDED(D3D12CreateDevice(cand, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))) ||
                    SUCCEEDED(D3D12CreateDevice(cand, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
                    cand->Release();
                    break;
                }
            }
            cand->Release();
        }
    }
    if (!dev) {
        IDXGIAdapter1* warp = nullptr;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
            if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
                D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
            }
            warp->Release();
        }
    }
    factory->Release();

    if (!dev) {
        impl_->Cleanup();
        return Status::Error(StatusCode::DeviceError, "Failed to create ID3D12Device for RayQueryExecutor");
    }
    impl_->device = dev;

    st = impl_->SetupComputePipeline();
    if (!st.ok()) {
        impl_->Cleanup();
        return st;
    }
#endif

    impl_->initialized = true;
    CallTelemetry::Global().Record("rdna2_rayquery", "ExecutorInitialize", CallOutcome::Ok, 0);
    return Status::Ok();
}

Status Rdna2RayQueryExecutor::Initialize(ID3D12Device* device, bool force_compute_fallback) {
    Shutdown();
#if defined(_WIN32)
    if (!device) {
        return Status::Error(StatusCode::InvalidArgument, "ID3D12Device must not be null");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->force_compute_fallback = force_compute_fallback;

    Status st = impl_->dxr_engine.Initialize(device, force_compute_fallback);
    if (!st.ok()) {
        return st;
    }
    impl_->caps = impl_->dxr_engine.Capabilities();
    impl_->selected_backend = SelectRayQueryBackend(impl_->caps, force_compute_fallback);

    impl_->device = device;
    impl_->device->AddRef();
    st = impl_->SetupComputePipeline();
    if (!st.ok()) {
        impl_->Cleanup();
        return st;
    }
    impl_->initialized = true;
    return Status::Ok();
#else
    (void)device;
    (void)force_compute_fallback;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->selected_backend = Rdna2RayQueryBackend::CpuReference;
    impl_->initialized = true;
    return Status::Ok();
#endif
}

void Rdna2RayQueryExecutor::Shutdown() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Cleanup();
}

Status Rdna2RayQueryExecutor::DispatchRayQueries(
    const Rdna2Bvh64MultiWindow& bvh64,
    uint64_t tlas_base_va,
    const std::vector<Rdna2RayQueryInput>& rays,
    std::vector<Rdna2RayQueryHit>& out_hits,
    Rdna2RayQueryDispatchReport* out_report) {
    out_hits.clear();
    if (!IsInitialized()) {
        return Status::Error(StatusCode::DeviceError, "Rdna2RayQueryExecutor is not initialized");
    }
    if (rays.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "DispatchRayQueries requires at least 1 ray");
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    Rdna2RayQueryDispatchReport rep{};
    rep.selected_backend = impl_->selected_backend;
    rep.hardware_dxr_tier = impl_->caps.hardware_tier;
    rep.adapter_is_warp = impl_->caps.adapter_is_warp;
    rep.adapter_description = impl_->caps.adapter_description;
    rep.ray_count = static_cast<uint32_t>(rays.size());
    rep.used_sm65_dxil = impl_->used_sm65_dxil;

#if defined(_WIN32)
    if (impl_->device && impl_->compute_pso && impl_->root_sig && impl_->cmd_list && impl_->queue) {
        std::vector<Rdna2GpuBvhWindowDesc> win_table;
        std::vector<uint32_t> bvh_dwords;
        uint32_t tlas_win_idx = 0u;
        Status st = bvh64.PackForGpuCompute(tlas_base_va, win_table, bvh_dwords, tlas_win_idx);
        if (!st.ok()) {
            return st;
        }

        const uint64_t win_bytes = static_cast<uint64_t>(win_table.size()) * sizeof(Rdna2GpuBvhWindowDesc);
        const uint64_t bvh_bytes = static_cast<uint64_t>(bvh_dwords.size()) * sizeof(uint32_t);
        const uint64_t ray_bytes = static_cast<uint64_t>(rays.size()) * sizeof(Rdna2RayQueryInput);
        const uint64_t out_bytes = static_cast<uint64_t>(rays.size()) * sizeof(Rdna2RayQueryHit);

        ID3D12Resource* win_buf = nullptr;
        ID3D12Resource* bvh_buf = nullptr;
        ID3D12Resource* ray_buf = nullptr;
        ID3D12Resource* out_uav = nullptr;
        ID3D12Resource* out_rb = nullptr;

        auto cleanup_bufs = [&]() {
            SafeReleaseCom(out_rb);
            SafeReleaseCom(out_uav);
            SafeReleaseCom(ray_buf);
            SafeReleaseCom(bvh_buf);
            SafeReleaseCom(win_buf);
        };

        auto upload_bytes = [&](const void* src, uint64_t sz, ID3D12Resource** dst) -> Status {
            Status cst = CreateD3D12Buffer(impl_->device, D3D12_HEAP_TYPE_UPLOAD, sz,
                                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, dst);
            if (!cst.ok()) return cst;
            void* mapped = nullptr;
            D3D12_RANGE no_read{0, 0};
            if (FAILED((*dst)->Map(0u, &no_read, &mapped)) || !mapped) {
                return Status::Error(StatusCode::DeviceError, "Upload buffer Map failed");
            }
            std::memcpy(mapped, src, static_cast<size_t>(sz));
            (*dst)->Unmap(0u, nullptr);
            return Status::Ok();
        };

        st = upload_bytes(win_table.data(), win_bytes, &win_buf);
        if (st.ok()) st = upload_bytes(bvh_dwords.data(), bvh_bytes, &bvh_buf);
        if (st.ok()) st = upload_bytes(rays.data(), ray_bytes, &ray_buf);
        if (st.ok()) {
            st = CreateD3D12Buffer(impl_->device, D3D12_HEAP_TYPE_DEFAULT, out_bytes,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &out_uav);
        }
        if (st.ok()) {
            st = CreateD3D12Buffer(impl_->device, D3D12_HEAP_TYPE_READBACK, out_bytes,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &out_rb);
        }

        if (st.ok()) {
            HRESULT hr = impl_->allocator->Reset();
            if (SUCCEEDED(hr)) {
                hr = impl_->cmd_list->Reset(impl_->allocator, impl_->compute_pso);
            }
            if (FAILED(hr)) {
                st = Status::Error(StatusCode::DeviceError, "CommandList Reset failed in RayQueryExecutor");
            } else {
                impl_->cmd_list->SetComputeRootSignature(impl_->root_sig);
                const uint32_t root_consts[4] = {
                    static_cast<uint32_t>(rays.size()),
                    tlas_win_idx,
                    static_cast<uint32_t>(win_table.size()),
                    0u,
                };
                impl_->cmd_list->SetComputeRoot32BitConstants(0u, 4u, root_consts, 0u);
                impl_->cmd_list->SetComputeRootShaderResourceView(1u, win_buf->GetGPUVirtualAddress());
                impl_->cmd_list->SetComputeRootShaderResourceView(2u, bvh_buf->GetGPUVirtualAddress());
                impl_->cmd_list->SetComputeRootShaderResourceView(3u, ray_buf->GetGPUVirtualAddress());
                impl_->cmd_list->SetComputeRootUnorderedAccessView(4u, out_uav->GetGPUVirtualAddress());

                const uint32_t groups_x = (static_cast<uint32_t>(rays.size()) + 63u) / 64u;
                impl_->cmd_list->Dispatch(groups_x, 1u, 1u);

                D3D12_RESOURCE_BARRIER barriers[2]{};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barriers[0].UAV.pResource = out_uav;
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = out_uav;
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                impl_->cmd_list->ResourceBarrier(2u, barriers);

                impl_->cmd_list->CopyResource(out_rb, out_uav);

                if (FAILED(impl_->cmd_list->Close())) {
                    st = Status::Error(StatusCode::DeviceError, "CommandList Close failed in RayQueryExecutor");
                } else {
                    ID3D12CommandList* lists[] = {impl_->cmd_list};
                    impl_->queue->ExecuteCommandLists(1u, lists);
                    st = impl_->WaitForGpu();
                }
            }
        }

        if (st.ok()) {
            void* mapped = nullptr;
            D3D12_RANGE read_range{0, static_cast<SIZE_T>(out_bytes)};
            if (FAILED(out_rb->Map(0u, &read_range, &mapped)) || !mapped) {
                st = Status::Error(StatusCode::DeviceError, "Readback buffer Map failed in RayQueryExecutor");
            } else {
                out_hits.resize(rays.size());
                std::memcpy(out_hits.data(), mapped, static_cast<size_t>(out_bytes));
                D3D12_RANGE written_none{0, 0};
                out_rb->Unmap(0u, &written_none);
                rep.executed_on_gpu = true;
            }
        }

        cleanup_bufs();
        if (!st.ok()) {
            return st;
        }
    } else
#endif
    {
        out_hits.resize(rays.size());
        for (size_t i = 0; i < rays.size(); ++i) {
            Status st = bvh64.TraceRay64(tlas_base_va, rays[i], out_hits[i]);
            if (!st.ok()) {
                return st;
            }
        }
        rep.executed_on_gpu = false;
    }

    for (const auto& h : out_hits) {
        if (h.committed_status != kRdna2CommittedNothing) {
            ++rep.hit_count;
        }
    }

    if (out_report) {
        *out_report = rep;
    }
    CallTelemetry::Global().Record("rdna2_rayquery", "DispatchRayQueries", CallOutcome::Ok, 0);
    return Status::Ok();
}

}  // namespace openscarlett
