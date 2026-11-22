// OpenScarlett - MIT License
// Sprint 12 (Worker 3): Live Verification Suite for RDNA2 RayQuery Bridge,
// DXR 1.1 HLSL SM 6.5 (dxc.exe), Vulkan SPV_KHR_ray_query SPIR-V 1.4 (spirv-val),
// Multi-Window 64-bit BVH4 Traversal on Live GPU (HW + D3D12 WARP),
// Title-List XboxBvhBuilder Conversion, and Live CreateStateObject / DispatchRays SBT Remapping.

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

#include "openscarlett/gpu/bvh4_decoder.h"
#include "openscarlett/gpu/dxr_hardware.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_rayquery_bridge.h"
#include "openscarlett/gpu/rt_bvh.h"
#include "openscarlett/shims/secondary_shims_abi.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using namespace openscarlett;

std::string FindSpirvValPortable() {
    std::error_code ec;
#if defined(_WIN32)
    char env_buf[1024]{};
    const DWORD env_len = GetEnvironmentVariableA(
        "OPENSCARLETT_SPIRV_VAL", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len > 0u && env_len < sizeof(env_buf) && std::filesystem::exists(env_buf, ec)) {
        return std::string(env_buf, env_len);
    }
    char vk_sdk[1024]{};
    const DWORD vk_len = GetEnvironmentVariableA(
        "VULKAN_SDK", vk_sdk, static_cast<DWORD>(sizeof(vk_sdk)));
    if (vk_len > 0u && vk_len < sizeof(vk_sdk)) {
        const auto cand = std::filesystem::path(std::string(vk_sdk, vk_len)) / "Bin" / "spirv-val.exe";
        if (std::filesystem::exists(cand, ec)) {
            return cand.string();
        }
    }
#endif
    // Walk up from source dir and current dir looking for repo-local .tools/spirv-val.exe
    const std::array<std::filesystem::path, 2> roots = {
        std::filesystem::path(OPENSCARLETT_SOURCE_DIR),
        std::filesystem::current_path(ec),
    };
    for (auto cur : roots) {
        for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
            const auto cand = cur / ".tools" / "spirv-val.exe";
            if (std::filesystem::exists(cand, ec)) {
                return cand.string();
            }
            const auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
    }
#if defined(_WIN32)
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(
        nullptr, "spirv-val.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0u && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf, path_len);
    }
#endif
    return {};
}

// ---------------------------------------------------------------------------
// Test 1: DXR 1.1 HLSL SM 6.5 (dxc.exe) + Compute BVH4 HLSL + SPV_KHR_ray_query (spirv-val)
// ---------------------------------------------------------------------------
void Test1_ShaderTranslationAndValidation() {
    for (Rdna2BvhIntersectOp op : {Rdna2BvhIntersectOp::ImageBvhIntersectRay32,
                                   Rdna2BvhIntersectOp::ImageBvh64IntersectRay}) {
        Rdna2BvhIntersectDesc desc{};
        desc.op = op;
        desc.a16 = (op == Rdna2BvhIntersectOp::ImageBvhIntersectRay32);
        desc.r128 = true;
        desc.ray_flags = kRdna2RayFlagNone;
        desc.instance_mask = 0xFFu;
        desc.threadgroup_x = 64u;
        desc.max_stack_depth = 64u;

        Rdna2RayQueryTranslationResult res{};
        OS_REQUIRE(TranslateRdna2BvhIntersectToRayQuery(desc, res).ok());
        OS_REQUIRE(!res.dxr11_hlsl.empty());
        OS_REQUIRE(!res.compute_bvh4_hlsl.empty());
        OS_REQUIRE(!res.spirv_ray_query_words.empty());

        // Verify DXR 1.1 HLSL contains all required RayQuery<RAY_FLAG_NONE> constructs
        OS_REQUIRE(res.dxr11_hlsl.find("RaytracingAccelerationStructure") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("RayDesc") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("RayQuery<RAY_FLAG_NONE>") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("TraceRayInline") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("Proceed()") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("COMMITTED_TRIANGLE_HIT") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("CommittedRayT()") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("CommittedTriangleBarycentrics()") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("CommittedPrimitiveIndex()") != std::string::npos);
        OS_REQUIRE(res.dxr11_hlsl.find("CommittedInstanceIndex()") != std::string::npos);

        // Compile BOTH DXR 1.1 HLSL and Compute BVH4 fallback HLSL with dxc.exe -T cs_6_5
        const std::string dxc_path = ResolvePortableDxcPath();
        if (!dxc_path.empty()) {
            std::vector<uint8_t> dxil_rq;
            std::string diag_rq;
            Status st_rq = CompileHlslWithDxc(res.dxr11_hlsl, "cs_6_5", "main", dxil_rq, diag_rq, dxc_path);
            if (!st_rq.ok()) {
                std::fprintf(stderr, "[FAIL] DXC cs_6_5 DXR 1.1 RayQuery failed: %s\n%s\n",
                             st_rq.message.c_str(), diag_rq.c_str());
            }
            OS_REQUIRE(st_rq.ok());
            OS_REQUIRE(dxil_rq.size() > 64u);
            OS_REQUIRE(dxil_rq[0] == 'D' && dxil_rq[1] == 'X' && dxil_rq[2] == 'B' && dxil_rq[3] == 'C');

            std::vector<uint8_t> dxil_fb;
            std::string diag_fb;
            Status st_fb = CompileHlslWithDxc(res.compute_bvh4_hlsl, "cs_6_5", "main", dxil_fb, diag_fb, dxc_path);
            if (!st_fb.ok()) {
                std::fprintf(stderr, "[FAIL] DXC cs_6_5 Compute BVH4 fallback failed: %s\n%s\n",
                             st_fb.message.c_str(), diag_fb.c_str());
            }
            OS_REQUIRE(st_fb.ok());
            OS_REQUIRE(dxil_fb.size() > 64u);
            OS_REQUIRE(dxil_fb[0] == 'D' && dxil_fb[1] == 'X' && dxil_fb[2] == 'B' && dxil_fb[3] == 'C');
        }

        // Structural validation of emitted Vulkan SPV_KHR_ray_query SPIR-V 1.4 binary
        OS_REQUIRE(ValidateSpirvBinary(res.spirv_ray_query_words).ok());
        Rdna2RayQuerySpirvInspectInfo spv_info{};
        OS_REQUIRE(InspectRdna2RayQuerySpirv(res.spirv_ray_query_words, spv_info).ok());
        OS_REQUIRE(spv_info.valid_magic);
        OS_REQUIRE(spv_info.version >= 0x00010400u);
        OS_REQUIRE(spv_info.has_ray_query_ext);
        OS_REQUIRE(spv_info.has_ray_query_cap);
        OS_REQUIRE(spv_info.has_accel_struct_type);
        OS_REQUIRE(spv_info.has_ray_query_type);
        OS_REQUIRE(spv_info.has_initialize);
        OS_REQUIRE(spv_info.has_proceed);
        OS_REQUIRE(spv_info.has_get_intersection_type);
        OS_REQUIRE(spv_info.has_get_intersection_t);
        OS_REQUIRE(spv_info.has_get_intersection_barycentrics);
        OS_REQUIRE(spv_info.has_get_intersection_prim_index);
        OS_REQUIRE(spv_info.has_get_intersection_instance_id);

        // External Khronos spirv-val --target-env vulkan1.2 validation when present
        const std::string spv_val = FindSpirvValPortable();
        if (!spv_val.empty()) {
            std::string diag;
            Status st = ValidateSpirvExternal(res.spirv_ray_query_words, diag, spv_val, "vulkan1.2");
            if (!st.ok()) {
                std::fprintf(stderr, "[FAIL] spirv-val --target-env vulkan1.2 failed: %s\n%s\n",
                             st.message.c_str(), diag.c_str());
            }
            OS_REQUIRE(st.ok());
        }
    }

    // Automatic backend selector unit verification
    DxrCapabilities tier11_caps{};
    tier11_caps.using_hardware_path = true;
    tier11_caps.supports_inline_rt = true;
    tier11_caps.hardware_tier = static_cast<uint32_t>(DxrTier::Tier1_1);
    OS_REQUIRE(SelectRayQueryBackend(tier11_caps, false) == Rdna2RayQueryBackend::HardwareDxr11);
    OS_REQUIRE(SelectRayQueryBackend(tier11_caps, true) == Rdna2RayQueryBackend::ComputeBvh4Fallback);

    DxrCapabilities tier0_caps{};
    tier0_caps.using_hardware_path = false;
    tier0_caps.supports_inline_rt = false;
    tier0_caps.hardware_tier = 0u;
    OS_REQUIRE(SelectRayQueryBackend(tier0_caps, false) == Rdna2RayQueryBackend::ComputeBvh4Fallback);

    std::printf("[PASS] Test1: DXR 1.1 SM 6.5 (dxc.exe) + Compute BVH4 HLSL + SPV_KHR_ray_query (spirv-val vulkan1.2)\n");
}

// ---------------------------------------------------------------------------
// Test 2: 64-bit Multi-Window BVH4 Traversal on LIVE GPU (Host GPU + D3D12 WARP)
//         Verified against exact analytic CPU reference (hit/miss, t, bary u/v, prim, inst)!
// ---------------------------------------------------------------------------
void Test2_MultiWindow64BitBvhLiveGpu() {
    // Build 2 disjoint BLAS meshes across separate 64-bit Xbox GPU VA windows:
    //   BLAS 0 (Box32 @ 0x00000011_00000000): 2-triangle quad on Z=0
    //   BLAS 1 (Box16 @ 0x00000022_80000000): 12-triangle unit cube
    XboxBvhBlasBuildInput blas0_in{};
    blas0_in.build_flags = kDxrBuildPreferFastTrace;
    blas0_in.use_box16 = false;
    {
        XboxBvhGeometryDesc g{};
        g.vertices = {
            {-1.0f, -1.0f, 0.0f},
            { 1.0f, -1.0f, 0.0f},
            { 1.0f,  1.0f, 0.0f},
            {-1.0f,  1.0f, 0.0f},
        };
        g.indices = {0u, 1u, 2u, 0u, 2u, 3u};
        g.geometry_index = 0u;
        g.is_opaque = true;
        blas0_in.geometries.push_back(std::move(g));
    }

    XboxBvhBlasBuildInput blas1_in{};
    blas1_in.build_flags = kDxrBuildMinimizeMemory;
    blas1_in.use_box16 = true;
    {
        const auto cube = BuildUnitCubeMesh({0.2f, 0.8f, 0.9f}, 0.25f);
        XboxBvhGeometryDesc g{};
        g.vertices.reserve(cube.size() * 3u);
        for (const auto& tri : cube) {
            g.vertices.push_back(tri.v0);
            g.vertices.push_back(tri.v1);
            g.vertices.push_back(tri.v2);
        }
        g.geometry_index = 2u;
        g.is_opaque = true;
        blas1_in.geometries.push_back(std::move(g));
    }

    constexpr uint64_t kBlas0Va = 0x0000001100000000ull;
    constexpr uint64_t kBlas1Va = 0x0000002280000000ull;
    constexpr uint64_t kTlasVa  = 0x0000003340000000ull;

    XboxBvhConvertedBlas blas0{}, blas1{};
    OS_REQUIRE(ConvertXboxBlasToHostAndBvh4(blas0_in, kBlas0Va, blas0).ok());
    OS_REQUIRE(ConvertXboxBlasToHostAndBvh4(blas1_in, kBlas1Va, blas1).ok());
    OS_REQUIRE(blas0.host_triangles.size() == 2u);
    OS_REQUIRE(blas1.host_triangles.size() == 12u);

    // Build TLAS with 3 instances (including translated/rotated instances + 1 masked-out instance)
    std::vector<XboxBvhTlasInstanceInput> tlas_insts(3);
    // Instance 0: Quad translated to Z = +3.0 (instance_id = 100, mask = 0x01, sbt_offset = 5)
    tlas_insts[0].blas_xbox_va = kBlas0Va;
    tlas_insts[0].host_blas_id = 1ull;
    MakeTranslation3x4(0.0f, 0.0f, 3.0f, tlas_insts[0].object_to_world);
    tlas_insts[0].instance_id = 100u;
    tlas_insts[0].instance_mask = 0x01u;
    tlas_insts[0].hit_group_contribution = 5u;

    // Instance 1: Box16 cube scaled & translated to (2.5, 0.0, 4.0) (instance_id = 200, mask = 0x02, sbt_offset = 9)
    tlas_insts[1].blas_xbox_va = kBlas1Va;
    tlas_insts[1].host_blas_id = 2ull;
    MakeScaleTranslation3x4(1.0f, 1.0f, 1.0f, 2.5f, 0.0f, 4.0f, tlas_insts[1].object_to_world);
    tlas_insts[1].instance_id = 200u;
    tlas_insts[1].instance_mask = 0x02u;
    tlas_insts[1].hit_group_contribution = 9u;

    // Instance 2: Masked-out blocker right in front of camera at Z = 1.0 (mask = 0x04)
    tlas_insts[2].blas_xbox_va = kBlas0Va;
    tlas_insts[2].host_blas_id = 1ull;
    MakeTranslation3x4(0.0f, 0.0f, 1.0f, tlas_insts[2].object_to_world);
    tlas_insts[2].instance_id = 300u;
    tlas_insts[2].instance_mask = 0x04u;
    tlas_insts[2].hit_group_contribution = 12u;

    XboxBvhConvertedTlas tlas{};
    OS_REQUIRE(ConvertXboxTlasToHostAndBvh4(
        tlas_insts, {blas0, blas1}, kTlasVa, kDxrBuildPreferFastTrace, false, tlas).ok());

    Rdna2Bvh64MultiWindow bvh64;
    OS_REQUIRE(bvh64.RegisterWindow(kBlas0Va, blas0.xbox_bvh4_buffer).ok());
    OS_REQUIRE(bvh64.RegisterWindow(kBlas1Va, blas1.xbox_bvh4_buffer).ok());
    OS_REQUIRE(bvh64.RegisterWindow(kTlasVa, tlas.xbox_bvh4_buffer).ok());
    OS_REQUIRE(bvh64.WindowCount() == 3u);

    // Direct single-step image_bvh64_intersect_ray verification on TLAS root
    {
        Rdna2Bvh64SingleStepResult step{};
        RtRay probe{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.001f, 100.0f};
        OS_REQUIRE(bvh64.StepBvh64IntersectRay(
            kTlasVa, tlas.bvh4_header.root_node_ptr, probe, 100.0f, 0u, 0xFFu, 0u, step).ok());
        OS_REQUIRE(step.is_box && step.hit_child_count >= 2u);
    }

    // Craft analytic test rays covering:
    //  Ray 0: hits Instance 0 quad tri 0 at (0.25, -0.25, 3.0) -> exact t=3.0, bary u=0.625, v=0.375
    //         (mask 0x03 skips Instance 2 blocker at Z=1.0!)
    //  Ray 1: hits Instance 2 blocker quad at Z=1.0 when mask=0xFF -> exact t=1.0, instance_id=300
    //  Ray 2: hits Instance 1 Box16 cube front face at (2.5, 0.0, 3.5) -> exact t=3.5, instance_id=200
    //  Ray 3: misses everything -> committed_status == 0
    //  Rays 4..67: dense 8x8 ray grid sweeping across both instances & empty sky
    std::vector<Rdna2RayQueryInput> rays;
    rays.reserve(68u);

    // Ray 0: origin (0.25, -0.25, 0.0), dir (0, 0, 1), mask=0x03 -> hits quad at Z=3.0
    // Tri 0 vertices: v0=(-1,-1,0), v1=(1,-1,0), v2=(1,1,0).
    // Point P=(0.25, -0.25) = v0 + u*(v1-v0) + v*(v2-v0) = (-1,-1) + u*(2,0) + v*(2,2)
    // => 2*v = 0.75 => v = 0.375; 2*u + 2*v = 1.25 => 2*u = 0.5 => u = 0.25!
    rays.push_back({{0.25f, -0.25f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0x03u, 0u, 0u});
    // Ray 1: same ray with mask=0xFF -> hits Instance 2 at Z=1.0
    rays.push_back({{0.25f, -0.25f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});
    // Ray 2: hits interior of Box16 cube -Z face at (2.65, -0.15, 3.5) -> exact t=3.5 (avoids shared diagonal)
    rays.push_back({{2.65f, -0.15f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0x03u, 0u, 0u});
    // Ray 3: complete miss
    rays.push_back({{-10.0f, 5.0f, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0xFFu, 0u, 0u});

    for (uint32_t y = 0u; y < 8u; ++y) {
        for (uint32_t x = 0u; x < 8u; ++x) {
            const float ox = -1.5f + 5.0f * (static_cast<float>(x) + 0.5f) / 8.0f + 0.013f;
            const float oy = -1.2f + 2.4f * (static_cast<float>(y) + 0.5f) / 8.0f - 0.029f;
            rays.push_back({{ox, oy, 0.0f}, 0.001f, {0.0f, 0.0f, 1.0f}, 100.0f, kRdna2RayFlagNone, 0x03u, 0u, 0u});
        }
    }

    // Compute exact analytic CPU reference hits
    std::vector<Rdna2RayQueryHit> cpu_hits(rays.size());
    for (size_t i = 0; i < rays.size(); ++i) {
        OS_REQUIRE(bvh64.TraceRay64(kTlasVa, rays[i], cpu_hits[i]).ok());
    }

    // Verify analytic closed-form expectations on Rays 0..3
    OS_REQUIRE(cpu_hits[0].committed_status == kRdna2CommittedTriangleHit);
    OS_REQUIRE(std::fabs(cpu_hits[0].t - 3.0f) < 1e-5f);
    OS_REQUIRE(std::fabs(cpu_hits[0].bary_u - 0.25f) < 1e-5f);
    OS_REQUIRE(std::fabs(cpu_hits[0].bary_v - 0.375f) < 1e-5f);
    OS_REQUIRE(cpu_hits[0].primitive_index == 0u);
    OS_REQUIRE(cpu_hits[0].instance_index == 0u);
    OS_REQUIRE(cpu_hits[0].instance_id == 100u);
    OS_REQUIRE(cpu_hits[0].sbt_offset == 5u);

    OS_REQUIRE(cpu_hits[1].committed_status == kRdna2CommittedTriangleHit);
    OS_REQUIRE(std::fabs(cpu_hits[1].t - 1.0f) < 1e-5f);
    OS_REQUIRE(cpu_hits[1].instance_index == 2u);
    OS_REQUIRE(cpu_hits[1].instance_id == 300u);
    OS_REQUIRE(cpu_hits[1].sbt_offset == 12u);

    OS_REQUIRE(cpu_hits[2].committed_status == kRdna2CommittedTriangleHit);
    OS_REQUIRE(std::fabs(cpu_hits[2].t - 3.5f) < 1e-5f);
    OS_REQUIRE(cpu_hits[2].instance_index == 1u);
    OS_REQUIRE(cpu_hits[2].instance_id == 200u);
    OS_REQUIRE(cpu_hits[2].geometry_index == 2u);
    OS_REQUIRE(cpu_hits[2].sbt_offset == 9u);

    OS_REQUIRE(cpu_hits[3].committed_status == kRdna2CommittedNothing);

    auto run_and_verify_gpu = [&](bool prefer_warp, const char* label) {
        Rdna2RayQueryExecutor exec;
        OS_REQUIRE(exec.Initialize(prefer_warp, false).ok());
        OS_REQUIRE(exec.IsInitialized());

        std::vector<Rdna2RayQueryHit> gpu_hits;
        Rdna2RayQueryDispatchReport rep{};
        OS_REQUIRE(exec.DispatchRayQueries(bvh64, kTlasVa, rays, gpu_hits, &rep).ok());
        OS_REQUIRE(rep.executed_on_gpu);
        OS_REQUIRE(gpu_hits.size() == cpu_hits.size());
        OS_REQUIRE(rep.hit_count > 10u);

        for (size_t i = 0; i < rays.size(); ++i) {
            OS_REQUIRE(gpu_hits[i].committed_status == cpu_hits[i].committed_status);
            if (cpu_hits[i].committed_status == kRdna2CommittedTriangleHit) {
                OS_REQUIRE(std::fabs(gpu_hits[i].t - cpu_hits[i].t) < 1e-4f);
                OS_REQUIRE(std::fabs(gpu_hits[i].bary_u - cpu_hits[i].bary_u) < 1e-4f);
                OS_REQUIRE(std::fabs(gpu_hits[i].bary_v - cpu_hits[i].bary_v) < 1e-4f);
                OS_REQUIRE(gpu_hits[i].primitive_index == cpu_hits[i].primitive_index);
                OS_REQUIRE(gpu_hits[i].instance_index == cpu_hits[i].instance_index);
                OS_REQUIRE(gpu_hits[i].instance_id == cpu_hits[i].instance_id);
                OS_REQUIRE(gpu_hits[i].geometry_index == cpu_hits[i].geometry_index);
                OS_REQUIRE(gpu_hits[i].sbt_offset == cpu_hits[i].sbt_offset);
                OS_REQUIRE(gpu_hits[i].front_face == cpu_hits[i].front_face);
            }
        }

        std::printf("  [%s] Adapter='%s' Tier=%u WARP=%s SM65=%s hits=%u/%u exact CPU==GPU match!\n",
                    label,
                    rep.adapter_description.c_str(),
                    rep.hardware_dxr_tier,
                    rep.adapter_is_warp ? "yes" : "no",
                    rep.used_sm65_dxil ? "yes" : "no",
                    rep.hit_count,
                    rep.ray_count);
        exec.Shutdown();
    };

    // Execute LIVE on Host Hardware GPU (GTX 1650 Ti) AND D3D12 WARP!
    run_and_verify_gpu(false, "HW-GPU");
    run_and_verify_gpu(true, "WARP");
    std::printf("[PASS] Test2: Multi-window 64-bit BVH4 live GPU execution (HW GPU + WARP == analytic CPU)\n");
}

// ---------------------------------------------------------------------------
// Test 3: XboxBvhBuilder DLL C ABI + Title-List Conversion + 64-bit BVH4 TraceRay
// ---------------------------------------------------------------------------
void Test3_XboxBvhBuilderConversion() {
    XboxBvhCreateDesc cdesc{};
    cdesc.preferWarp = 1u;
    XboxBvhBuilderHandle builder = nullptr;
    OS_REQUIRE(XboxBvhCreateBuilder(&cdesc, &builder) == OS_SHIM_S_OK && builder != nullptr);

    const auto scene = BuildDefaultRtTestScene();
    std::vector<XboxBvhTriangle> xtris(scene.size());
    for (size_t i = 0; i < scene.size(); ++i) {
        xtris[i].v0[0] = scene[i].v0.x; xtris[i].v0[1] = scene[i].v0.y; xtris[i].v0[2] = scene[i].v0.z;
        xtris[i].v1[0] = scene[i].v1.x; xtris[i].v1[1] = scene[i].v1.y; xtris[i].v1[2] = scene[i].v1.z;
        xtris[i].v2[0] = scene[i].v2.x; xtris[i].v2[1] = scene[i].v2.y; xtris[i].v2[2] = scene[i].v2.z;
        xtris[i].color[0] = scene[i].color.x; xtris[i].color[1] = scene[i].color.y; xtris[i].color[2] = scene[i].color.z;
        xtris[i].reflectivity = scene[i].reflectivity;
    }

    uint64_t blas_id = 0ull;
    OS_REQUIRE(XboxBvhBuildBlas(builder, xtris.data(), static_cast<uint32_t>(xtris.size()),
                                XBOX_BVH_BUILD_PREFER_FAST_TRACE | XBOX_BVH_BUILD_ALLOW_UPDATE,
                                &blas_id) == OS_SHIM_S_OK);
    OS_REQUIRE(blas_id != 0ull);

    XboxBvhInstanceDesc inst{};
    inst.blasId = blas_id;
    MakeIdentity3x4(inst.transform);
    inst.instanceId = 77u;
    inst.instanceMask = 0xFFu;
    inst.hitGroupContribution = 0u;
    inst.flags = 0u;

    uint64_t tlas_id = 0ull;
    OS_REQUIRE(XboxBvhBuildTlas(builder, &inst, 1u, XBOX_BVH_BUILD_PREFER_FAST_TRACE, &tlas_id) == OS_SHIM_S_OK);
    OS_REQUIRE(tlas_id != 0ull);

    XboxBvhRay ray{};
    ray.origin[0] = 0.0f; ray.origin[1] = 1.0f; ray.origin[2] = -3.5f;
    ray.direction[0] = 0.0f; ray.direction[1] = -0.1f; ray.direction[2] = 0.994987f;
    ray.tMin = 0.001f;
    ray.tMax = 100.0f;

    XboxBvhHit hit_blas{};
    XboxBvhHit hit_tlas{};
    OS_REQUIRE(XboxBvhTraceRay(builder, blas_id, &ray, &hit_blas) == OS_SHIM_S_OK);
    OS_REQUIRE(XboxBvhTraceRay(builder, tlas_id, &ray, &hit_tlas) == OS_SHIM_S_OK);
    OS_REQUIRE(hit_blas.hit == 1u && hit_tlas.hit == 1u);
    OS_REQUIRE(std::fabs(hit_blas.t - hit_tlas.t) < 1e-4f);
    OS_REQUIRE(std::fabs(hit_blas.u - hit_tlas.u) < 1e-4f);
    OS_REQUIRE(std::fabs(hit_blas.v - hit_tlas.v) < 1e-4f);

    XboxBvhDestroyBuilder(builder);
    std::printf("[PASS] Test3: XboxBvhBuilder DLL BLAS/TLAS BVH4 conversion & TraceRay parity (t=%.4f)\n",
                hit_tlas.t);
}

// ---------------------------------------------------------------------------
// Test 4: Live CreateStateObject / DispatchRays SBT Record Remapping (32B & 64B Xbox IDs)
// ---------------------------------------------------------------------------
void Test4_CreateStateObjectAndSbtRemapping() {
    DxrHardwareEngine engine;
    DxrInitOptions opts{};
    opts.prefer_warp = false;  // Live host GPU
    OS_REQUIRE(engine.Initialize(opts).ok());

    const auto scene = BuildDefaultRtTestScene();
    uint64_t blas_id = 0ull;
    OS_REQUIRE(engine.BuildBlas(scene, kDxrBuildPreferFastTrace, blas_id).ok());
    DxrInstanceDesc inst{};
    inst.blas_id = blas_id;
    MakeIdentity3x4(inst.transform);
    uint64_t tlas_id = 0ull;
    OS_REQUIRE(engine.BuildTlas({inst}, kDxrBuildPreferFastTrace, tlas_id).ok());

    DxrStateObjectSbtRemapper remapper;

    for (uint32_t xbox_id_size : {32u, 64u}) {
        DxrStateObjectDesc so_desc{};
        so_desc.xbox_identifier_size_bytes = xbox_id_size;
        so_desc.max_payload_size_bytes = 48u;
        so_desc.max_attribute_size_bytes = 8u;
        so_desc.max_recursion_depth = 2u;
        so_desc.exports = {
            {"XboxRayGen", DxrStateObjectExportDesc::Kind::RayGen, "", "", "", {}},
            {"XboxMissSky", DxrStateObjectExportDesc::Kind::Miss, "", "", "", {}},
            {"XboxMissShadow", DxrStateObjectExportDesc::Kind::Miss, "", "", "", {}},
            {"XboxHitDiffuse", DxrStateObjectExportDesc::Kind::HitGroup, "ChsDiffuse", "", "", {}},
            {"XboxHitGlossy", DxrStateObjectExportDesc::Kind::HitGroup, "ChsGlossy", "AhsMask", "", {}},
            {"XboxCallableBrdf", DxrStateObjectExportDesc::Kind::Callable, "", "", "", {}},
        };

        uint64_t so_id = 0ull;
        OS_REQUIRE(remapper.CreateStateObject(so_desc, so_id).ok());
        OS_REQUIRE(so_id != 0ull);

        auto make_xbox_rec = [&](const std::string& exp_name, const std::vector<uint8_t>& local_args) {
            std::vector<uint8_t> xid;
            OS_REQUIRE(remapper.GetXboxShaderIdentifier(so_id, exp_name, xid).ok());
            OS_REQUIRE(xid.size() == xbox_id_size);
            XboxSbtRecordInput rec{};
            rec.raw_record_bytes = xid;
            rec.raw_record_bytes.insert(rec.raw_record_bytes.end(), local_args.begin(), local_args.end());
            return rec;
        };

        const std::vector<uint8_t> rg_args = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
        const std::vector<uint8_t> hit0_args = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
        const std::vector<uint8_t> hit1_args(36u, 0x5Cu);

        XboxDispatchRaysInput xdisp{};
        xdisp.raygen_record = make_xbox_rec("XboxRayGen", rg_args);
        xdisp.miss_records = {
            make_xbox_rec("XboxMissSky", {}),
            make_xbox_rec("XboxMissShadow", {0x01, 0x02, 0x03, 0x04}),
        };
        xdisp.hit_group_records = {
            make_xbox_rec("XboxHitDiffuse", hit0_args),
            make_xbox_rec("XboxHitGlossy", hit1_args),
        };
        xdisp.callable_records = {
            make_xbox_rec("XboxCallableBrdf", {0x7F, 0x7E}),
        };
        xdisp.width = 64u;
        xdisp.height = 48u;
        xdisp.depth = 1u;

        DxrRemappedSbtResult remapped{};
        OS_REQUIRE(remapper.RemapDispatchRaysSbt(so_id, engine, xdisp, remapped).ok());
        OS_REQUIRE(remapped.remapped_record_count == 6u);
        OS_REQUIRE(remapped.xbox_identifier_size_bytes == xbox_id_size);
        OS_REQUIRE(remapped.host_sbt.uploaded_to_gpu);
        OS_REQUIRE((remapped.host_sbt.base_gpu_va % kDxrShaderTableAlignment) == 0u);
        OS_REQUIRE((remapped.host_sbt.miss_record_stride % kDxrShaderRecordAlignment) == 0u);
        OS_REQUIRE((remapped.host_sbt.hit_group_record_stride % kDxrShaderRecordAlignment) == 0u);

        // Verify host 32-byte shader identifier + exact local root arguments landed in host SBT bytes
        std::array<uint8_t, kDxrShaderIdentifierSize> expected_rg_host_id{};
        OS_REQUIRE(remapper.GetHostShaderIdentifier(so_id, "XboxRayGen", expected_rg_host_id).ok());
        OS_REQUIRE(std::memcmp(remapped.host_sbt.buffer.data() + remapped.host_sbt.raygen_offset,
                               expected_rg_host_id.data(), kDxrShaderIdentifierSize) == 0);
        OS_REQUIRE(std::memcmp(
                       remapped.host_sbt.buffer.data() + remapped.host_sbt.raygen_offset + kDxrShaderIdentifierSize,
                       rg_args.data(), rg_args.size()) == 0);

        // Verify software dispatch descriptors preserve export names, indices, and root arguments
        OS_REQUIRE(remapped.sw_raygen.export_name == "XboxRayGen");
        OS_REQUIRE(remapped.sw_raygen.local_root_arguments == rg_args);
        OS_REQUIRE(remapped.sw_hit_groups.size() == 2u);
        OS_REQUIRE(remapped.sw_hit_groups[0].export_name == "XboxHitDiffuse");
        OS_REQUIRE(remapped.sw_hit_groups[0].local_root_arguments == hit0_args);
        OS_REQUIRE(remapped.sw_hit_groups[1].export_name == "XboxHitGlossy");
        OS_REQUIRE(remapped.sw_hit_groups[1].local_root_arguments == hit1_args);

        // Execute real DispatchRays using the remapped host SBT!
        RtDispatchDesc rtd{};
        rtd.width = 64u;
        rtd.height = 48u;
        rtd.depth = 1u;
        rtd.sbt = &remapped.host_sbt;
        FrameBufferRgba8 frame{};
        RtStats stats{};
        uint64_t hash = 0ull;
        OS_REQUIRE(engine.DispatchRays(tlas_id, rtd, frame, &stats, &hash).ok());
        OS_REQUIRE(hash != 0ull && stats.primary_hits > 0u);

        // Negative check: unknown Xbox identifier must fail cleanly
        XboxDispatchRaysInput bad_xdisp = xdisp;
        bad_xdisp.raygen_record.raw_record_bytes[0] ^= 0xFFu;
        DxrRemappedSbtResult dummy{};
        OS_REQUIRE(remapper.RemapDispatchRaysSbt(so_id, engine, bad_xdisp, dummy).code == StatusCode::NotFound);

        OS_REQUIRE(remapper.DestroyStateObject(so_id).ok());
    }

    engine.Shutdown();
    std::printf("[PASS] Test4: CreateStateObject / DispatchRays SBT remapping (32B & 64B Xbox IDs) + live GPU upload\n");
}

}  // namespace

int main() {
    std::printf("=== OpenScarlett Sprint 12 Live RDNA2 RayQuery, Multi-Window BVH64 & SBT Suite ===\n");
    Test1_ShaderTranslationAndValidation();
    Test2_MultiWindow64BitBvhLiveGpu();
    Test3_XboxBvhBuilderConversion();
    Test4_CreateStateObjectAndSbtRemapping();
    std::printf("ALL SPRINT 12 RAYQUERY / BVH64 / SBT TESTS PASSED!\n");
    return 0;
}
