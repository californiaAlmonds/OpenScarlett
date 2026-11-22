#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rt_bvh.h"

struct ID3D12Device;

namespace openscarlett {

// ---------------------------------------------------------------------------
// 1. RDNA2 BVH Ray-Intersection Ops & RayQuery Translation Constants
// ---------------------------------------------------------------------------
enum class Rdna2BvhIntersectOp : uint8_t {
    ImageBvhIntersectRay32 = 0,   // image_bvh_intersect_ray (32-bit node ptr)
    ImageBvh64IntersectRay = 1,   // image_bvh64_intersect_ray (64-bit node ptr)
};

enum class Rdna2RayQueryBackend : uint8_t {
    Auto = 0,
    HardwareDxr11 = 1,         // DXR Tier 1.1 inline RayQuery<...>
    ComputeBvh4Fallback = 2,   // Live GPU compute BVH4 traversal (non-DXR GPU / WARP)
    CpuReference = 3,          // Deterministic CPU BVH4 reference traversal
};

// Ray flags matching DXR 1.1 RAY_FLAG_* and Vulkan SpvRayFlagsMask bits.
enum Rdna2RayFlags : uint32_t {
    kRdna2RayFlagNone = 0x00u,
    kRdna2RayFlagForceOpaque = 0x01u,
    kRdna2RayFlagForceNonOpaque = 0x02u,
    kRdna2RayFlagAcceptFirstHitAndEndSearch = 0x04u,
    kRdna2RayFlagSkipClosestHitShader = 0x08u,
    kRdna2RayFlagCullBackFacingTriangles = 0x10u,
    kRdna2RayFlagCullFrontFacingTriangles = 0x20u,
    kRdna2RayFlagCullOpaque = 0x40u,
    kRdna2RayFlagCullNonOpaque = 0x80u,
};

enum Rdna2CommittedStatus : uint32_t {
    kRdna2CommittedNothing = 0u,
    kRdna2CommittedTriangleHit = 1u,
    kRdna2CommittedProceduralHit = 2u,
};

struct Rdna2BvhIntersectDesc {
    Rdna2BvhIntersectOp op = Rdna2BvhIntersectOp::ImageBvh64IntersectRay;
    bool a16 = false;                  // FP16 vs FP32 box/ray encoding
    bool r128 = true;                  // 128-bit vs 256-bit BVH resource descriptor
    uint32_t ray_flags = kRdna2RayFlagNone;
    uint32_t instance_mask = 0xFFu;
    uint32_t threadgroup_x = 64u;
    uint32_t max_stack_depth = 64u;
    bool emit_dxr11_hlsl = true;
    bool emit_compute_bvh4_fallback_hlsl = true;
    bool emit_spirv_ray_query = true;
};

struct Rdna2RayQueryTranslationResult {
    Rdna2BvhIntersectOp source_op = Rdna2BvhIntersectOp::ImageBvh64IntersectRay;
    std::string dxr11_hlsl;
    std::string dxr11_target = "cs_6_5";
    std::string compute_bvh4_hlsl;
    std::string compute_bvh4_target = "cs_6_5";
    std::vector<uint32_t> spirv_ray_query_words;
};

struct Rdna2RayQuerySpirvInspectInfo {
    bool valid_magic = false;
    uint32_t version = 0u;
    bool has_ray_query_ext = false;
    bool has_ray_query_cap = false;
    bool has_accel_struct_type = false;
    bool has_ray_query_type = false;
    bool has_initialize = false;
    bool has_proceed = false;
    bool has_get_intersection_type = false;
    bool has_get_intersection_t = false;
    bool has_get_intersection_barycentrics = false;
    bool has_get_intersection_prim_index = false;
    bool has_get_intersection_instance_id = false;
};

// Translates an RDNA2 image_bvh_intersect_ray / image_bvh64_intersect_ray descriptor into:
//   - Real DXR 1.1 HLSL SM 6.5 (RayQuery<RAY_FLAG_NONE> / TraceRayInline / Proceed / Committed*)
//   - Real GPU Compute BVH4 fallback HLSL (cs_6_5 & cs_5_0 compatible)
//   - Real Vulkan SPV_KHR_ray_query SPIR-V 1.4 binary (spirv-val --target-env vulkan1.2 clean)
Status TranslateRdna2BvhIntersectToRayQuery(
    const Rdna2BvhIntersectDesc& desc,
    Rdna2RayQueryTranslationResult& out_result);

Status EmitRdna2RayQueryDxr11Hlsl(
    const Rdna2BvhIntersectDesc& desc,
    std::string& out_hlsl);

Status EmitRdna2ComputeBvh4FallbackHlsl(
    const Rdna2BvhIntersectDesc& desc,
    std::string& out_hlsl);

Status EmitRdna2RayQuerySpirvKhr(
    const Rdna2BvhIntersectDesc& desc,
    std::vector<uint32_t>& out_spirv);

Status InspectRdna2RayQuerySpirv(
    const std::vector<uint32_t>& spirv,
    Rdna2RayQuerySpirvInspectInfo& out_info);

// Automatic hardware-vs-software backend selector driven by DxrHardwareEngine Tier 1.1 probe.
Rdna2RayQueryBackend SelectRayQueryBackend(
    const DxrCapabilities& caps,
    bool force_compute_fallback = false) noexcept;

Rdna2RayQueryBackend SelectRayQueryBackend(
    const DxrHardwareEngine& engine,
    bool force_compute_fallback = false) noexcept;

// ---------------------------------------------------------------------------
// 2. Enhanced Ray-Primitive Intersection Helpers (implemented in rt_bvh.cpp)
// ---------------------------------------------------------------------------
bool IntersectRayAabbSlabs(
    const RtAabb& box,
    const RtRay& ray,
    float t_best,
    float& out_t_near) noexcept;

bool IntersectRayTriangleEx(
    const RtTriangle& tri,
    const RtRay& ray,
    float t_best,
    uint32_t ray_flags,
    uint32_t instance_flags,
    float& out_t,
    float& out_u,
    float& out_v,
    Vec3& out_normal,
    bool& out_front_face) noexcept;

// ---------------------------------------------------------------------------
// 3. 64-bit Multi-Window RDNA2 BVH4 Traversal (implemented in bvh4_decoder.cpp)
// ---------------------------------------------------------------------------
// ABI-matched GPU/CPU ray input (48 bytes = 12 dwords).
struct Rdna2RayQueryInput {
    Vec3 origin{};
    float t_min = 0.001f;
    Vec3 direction{0.0f, 0.0f, 1.0f};
    float t_max = 1e30f;
    uint32_t ray_flags = kRdna2RayFlagNone;
    uint32_t instance_mask = 0xFFu;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};
static_assert(sizeof(Rdna2RayQueryInput) == 48u, "Rdna2RayQueryInput must be 48 bytes");

// ABI-matched GPU/CPU ray query hit result (48 bytes = 12 dwords).
struct Rdna2RayQueryHit {
    uint32_t committed_status = kRdna2CommittedNothing;
    float t = 1e30f;
    float bary_u = 0.0f;
    float bary_v = 0.0f;
    uint32_t primitive_index = 0xFFFFFFFFu;
    uint32_t instance_index = 0xFFFFFFFFu;
    uint32_t instance_id = 0xFFFFFFFFu;
    uint32_t geometry_index = 0u;
    uint32_t sbt_offset = 0u;
    uint32_t front_face = 1u;
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};
static_assert(sizeof(Rdna2RayQueryHit) == 48u, "Rdna2RayQueryHit must be 48 bytes");

// GPU SRV descriptor for one disjoint 64-bit BVH4 window (32 bytes = 8 dwords).
struct Rdna2GpuBvhWindowDesc {
    uint32_t base_va_lo = 0u;
    uint32_t base_va_hi = 0u;
    uint32_t dword_offset = 0u;
    uint32_t dword_count = 0u;
    uint32_t root_node_ptr = kRdna2BvhInvalidNodePtr;
    uint32_t as_type = 0u;  // 0 = TLAS, 1 = BLAS
    uint32_t pad0 = 0u;
    uint32_t pad1 = 0u;
};
static_assert(sizeof(Rdna2GpuBvhWindowDesc) == 32u, "Rdna2GpuBvhWindowDesc must be 32 bytes");

struct Rdna2Bvh64Window {
    uint64_t base_gpu_va = 0ull;
    Rdna2BvhHeader header{};
    std::vector<uint8_t> buffer;
};

// Direct emulation result of a single RDNA2 image_bvh_intersect_ray / image_bvh64_intersect_ray op.
struct Rdna2Bvh64SingleStepResult {
    Rdna2BvhNodeType node_type = Rdna2BvhNodeType::Box32;
    bool is_triangle = false;
    bool is_box = false;
    bool is_instance = false;
    bool is_procedural = false;
    bool triangle_hit = false;
    bool front_face = true;
    float hit_t = 1e30f;
    float bary_u = 0.0f;
    float bary_v = 0.0f;
    uint32_t primitive_index = 0xFFFFFFFFu;
    uint32_t geometry_index = 0u;
    std::array<uint32_t, 4> sorted_children = {
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
        kRdna2BvhInvalidNodePtr,
    };
    uint32_t hit_child_count = 0u;
    uint64_t next_blas_base_va = 0ull;
    uint32_t next_blas_root_ptr = kRdna2BvhInvalidNodePtr;
    uint32_t instance_index = 0xFFFFFFFFu;
    uint32_t instance_id = 0xFFFFFFFFu;
    uint32_t instance_flags = 0u;
    uint32_t sbt_offset = 0u;
    RtRay transformed_ray{};
};

// Manages multiple disjoint 64-bit Xbox GPU VA BVH4 windows (TLAS + multiple BLAS buffers)
// and supports both single-step image_bvh64_intersect_ray execution and full TLAS->BLAS traversal.
class Rdna2Bvh64MultiWindow {
public:
    Status RegisterWindow(uint64_t base_gpu_va, const uint8_t* data, size_t size);
    Status RegisterWindow(uint64_t base_gpu_va, std::vector<uint8_t> buffer);
    bool UnregisterWindow(uint64_t base_gpu_va);
    void Clear();

    size_t WindowCount() const noexcept { return windows_.size(); }
    const Rdna2Bvh64Window* FindWindow(uint64_t base_gpu_va) const noexcept;

    Status StepBvh64IntersectRay(
        uint64_t window_base_va,
        uint32_t node_ptr,
        const RtRay& ray,
        float t_best,
        uint32_t ray_flags,
        uint32_t instance_mask,
        uint32_t active_instance_flags,
        Rdna2Bvh64SingleStepResult& out_step) const;

    Status TraceRay64(
        uint64_t tlas_base_va,
        const Rdna2RayQueryInput& ray,
        Rdna2RayQueryHit& out_hit,
        RtStats* out_stats = nullptr) const;

    // Packs all registered 64-bit BVH4 windows into GPU-ready descriptor + dword buffers.
    Status PackForGpuCompute(
        uint64_t tlas_base_va,
        std::vector<Rdna2GpuBvhWindowDesc>& out_window_table,
        std::vector<uint32_t>& out_bvh_dwords,
        uint32_t& out_tlas_window_index) const;

private:
    std::vector<Rdna2Bvh64Window> windows_;
};

// ---------------------------------------------------------------------------
// 4. Live GPU RayQuery Executor (DXR 1.1 HW or Live Compute BVH4 on D3D12)
// ---------------------------------------------------------------------------
struct Rdna2RayQueryDispatchReport {
    Rdna2RayQueryBackend selected_backend = Rdna2RayQueryBackend::ComputeBvh4Fallback;
    uint32_t hardware_dxr_tier = 0u;
    bool adapter_is_warp = false;
    bool executed_on_gpu = false;
    bool used_sm65_dxil = false;
    std::string adapter_description;
    uint32_t ray_count = 0u;
    uint32_t hit_count = 0u;
};

class Rdna2RayQueryExecutor {
public:
    Rdna2RayQueryExecutor();
    ~Rdna2RayQueryExecutor();

    Rdna2RayQueryExecutor(const Rdna2RayQueryExecutor&) = delete;
    Rdna2RayQueryExecutor& operator=(const Rdna2RayQueryExecutor&) = delete;

    Status Initialize(bool prefer_warp = false, bool force_compute_fallback = false);
    Status Initialize(ID3D12Device* device, bool force_compute_fallback = false);
    void Shutdown();
    bool IsInitialized() const noexcept;

    Rdna2RayQueryBackend SelectedBackend() const noexcept;
    const DxrCapabilities& Capabilities() const noexcept;

    Status DispatchRayQueries(
        const Rdna2Bvh64MultiWindow& bvh64,
        uint64_t tlas_base_va,
        const std::vector<Rdna2RayQueryInput>& rays,
        std::vector<Rdna2RayQueryHit>& out_hits,
        Rdna2RayQueryDispatchReport* out_report = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// 5. Title-List XboxBvhBuilder Conversion (Xbox BVH4 <-> PC Host BLAS/TLAS)
// ---------------------------------------------------------------------------
struct XboxBvhGeometryDesc {
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;  // Optional; if empty, vertices are treated as non-indexed triangles
    float transform[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    bool has_transform = false;
    bool is_opaque = true;
    uint32_t geometry_index = 0u;
    Vec3 default_color{0.8f, 0.8f, 0.8f};
    float default_reflectivity = 0.0f;
};

struct XboxBvhBlasBuildInput {
    std::vector<XboxBvhGeometryDesc> geometries;
    uint32_t build_flags = kDxrBuildPreferFastTrace;
    bool use_box16 = false;
};

struct XboxBvhTlasInstanceInput {
    uint64_t blas_xbox_va = 0ull;
    uint64_t host_blas_id = 0ull;
    float object_to_world[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    uint32_t instance_id = 0u;
    uint32_t instance_mask = 0xFFu;
    uint32_t hit_group_contribution = 0u;
    uint32_t instance_flags = kDxrInstanceNone;
};

struct XboxBvhConvertedBlas {
    uint64_t xbox_gpu_va = 0ull;
    std::vector<RtTriangle> host_triangles;
    std::vector<uint8_t> xbox_bvh4_buffer;
    Rdna2BvhHeader bvh4_header{};
    DxrPrebuildInfo host_prebuild{};
    uint32_t host_build_flags = 0u;
    uint32_t geometry_count = 0u;
};

struct XboxBvhConvertedTlas {
    uint64_t xbox_gpu_va = 0ull;
    std::vector<DxrInstanceDesc> host_instances;
    std::vector<uint8_t> xbox_bvh4_buffer;
    Rdna2BvhHeader bvh4_header{};
    DxrPrebuildInfo host_prebuild{};
    uint32_t host_build_flags = 0u;
};

Status ConvertXboxBlasToHostAndBvh4(
    const XboxBvhBlasBuildInput& input,
    uint64_t xbox_base_va,
    XboxBvhConvertedBlas& out_blas);

Status ConvertXboxTlasToHostAndBvh4(
    const std::vector<XboxBvhTlasInstanceInput>& instances,
    const std::vector<XboxBvhConvertedBlas>& blas_list,
    uint64_t tlas_base_va,
    uint32_t build_flags,
    bool use_box16,
    XboxBvhConvertedTlas& out_tlas);

// ---------------------------------------------------------------------------
// 6. Live CreateStateObject / DispatchRays SBT Record Remapping
//    (Xbox 32/64-byte shader identifiers + local root args -> Host PC SBT)
// ---------------------------------------------------------------------------
struct DxrStateObjectExportDesc {
    enum class Kind : uint8_t {
        RayGen = 0,
        Miss = 1,
        HitGroup = 2,
        Callable = 3,
    };

    std::string export_name;
    Kind kind = Kind::RayGen;
    std::string closest_hit_shader;
    std::string any_hit_shader;
    std::string intersection_shader;
    // Optional explicit 32-byte or 64-byte Xbox shader identifier. If empty, derived deterministically.
    std::vector<uint8_t> xbox_shader_identifier;
};

struct DxrStateObjectDesc {
    std::vector<DxrStateObjectExportDesc> exports;
    uint32_t max_payload_size_bytes = 32u;
    uint32_t max_attribute_size_bytes = 8u;
    uint32_t max_recursion_depth = 2u;
    uint32_t xbox_identifier_size_bytes = 32u;  // 32 or 64 bytes
};

struct XboxSbtRecordInput {
    // Starts with 32B or 64B Xbox shader identifier followed by optional local root arguments.
    std::vector<uint8_t> raw_record_bytes;
};

struct XboxDispatchRaysInput {
    XboxSbtRecordInput raygen_record;
    std::vector<XboxSbtRecordInput> miss_records;
    std::vector<XboxSbtRecordInput> hit_group_records;
    std::vector<XboxSbtRecordInput> callable_records;
    uint32_t width = 1u;
    uint32_t height = 1u;
    uint32_t depth = 1u;
    uint64_t optional_host_base_va = 0ull;
};

struct SoftwareDispatchRecord {
    std::string export_name;
    DxrStateObjectExportDesc::Kind kind = DxrStateObjectExportDesc::Kind::RayGen;
    uint32_t shader_index = 0u;
    std::vector<uint8_t> local_root_arguments;
};

struct DxrRemappedSbtResult {
    DxrShaderBindingTable host_sbt{};
    SoftwareDispatchRecord sw_raygen{};
    std::vector<SoftwareDispatchRecord> sw_miss;
    std::vector<SoftwareDispatchRecord> sw_hit_groups;
    std::vector<SoftwareDispatchRecord> sw_callables;
    uint32_t xbox_identifier_size_bytes = 32u;
    uint32_t remapped_record_count = 0u;
};

class DxrStateObjectSbtRemapper {
public:
    DxrStateObjectSbtRemapper();
    ~DxrStateObjectSbtRemapper();

    Status CreateStateObject(
        const DxrStateObjectDesc& desc,
        uint64_t& out_state_object_id);

    Status GetXboxShaderIdentifier(
        uint64_t state_object_id,
        const std::string& export_name,
        std::vector<uint8_t>& out_xbox_id) const;

    Status GetHostShaderIdentifier(
        uint64_t state_object_id,
        const std::string& export_name,
        std::array<uint8_t, kDxrShaderIdentifierSize>& out_host_id) const;

    Status RemapDispatchRaysSbt(
        uint64_t state_object_id,
        DxrHardwareEngine& engine,
        const XboxDispatchRaysInput& xbox_input,
        DxrRemappedSbtResult& out_remapped) const;

    Status DestroyStateObject(uint64_t state_object_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace openscarlett
