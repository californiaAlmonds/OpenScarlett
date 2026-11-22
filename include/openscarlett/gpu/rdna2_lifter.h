#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_ir.h"

namespace openscarlett {

inline constexpr const char* kDefaultSpirvValPath = "E:\\OpenScarlett\\.tools\\spirv-val.exe";
inline constexpr const char* kDefaultDxcPath =
    "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxc.exe";
inline constexpr const char* kLifterTempDir = "E:\\OpenScarlett\\.tmp\\lifter";

struct Rdna2LiftOptions {
    bool use_sm65 = true;
    bool emit_spirv = true;
    bool emit_mesh_spirv = false;  // Emit SPIR-V 1.4 SPV_EXT_mesh_shader (with Block+bool CullPrimitiveEXT) for Mesh stage
    bool emit_hlsl = true;
    bool validate_spirv_external = false;
    bool compile_dxil_with_dxc = false;
    bool resign_dxil = true;       // Verify/re-sign DXIL container MD5 digest after compilation/retargeting
    bool mesh_has_payload = true;
    StageContractTable stage_contract{};  // Optional FSSM pixel-input & vertex-fetch subroutine table mapping
    std::string target_profile_override;  // Optional explicit DXC profile (e.g. "cs_6_2", "ps_6_2")
    std::string spirv_val_path = kDefaultSpirvValPath;
    std::string spirv_target_env = "vulkan1.2";
    std::string dxc_path;  // Empty -> portable lookup (exe dir -> OPENSCARLETT_DXC_PATH -> PATH -> WinSDK)
};

// Portable dxc.exe resolution: exe dir -> OPENSCARLETT_DXC_PATH -> requested_path -> PATH -> Windows Kits.
std::string ResolvePortableDxcPath(const std::string& requested_path = {});

struct Rdna2LiftResult {
    Xs2ShaderContainer container;
    std::vector<Rdna2Instruction> instructions;
    Rdna2IrModule ir;
    std::vector<uint32_t> spirv_words;
    std::string hlsl_source;
    std::string hlsl_target;
    std::vector<uint8_t> pc_dxil;
    uint64_t pc_dxil_hash = 0;
    uint32_t inserted_lockstep_lds_barriers = 0;
    std::string spirv_val_diagnostics;
    std::string dxc_diagnostics;
};

// Emits a strictly valid SPIR-V 1.3 / 1.4 / 1.6 binary module (generator ID 0x4F533200)
// from an RDNA2 SSA IR module (Compute, Vertex, Pixel, MeshEXT, and TaskEXT Amplification).
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv);

// Structurally validates a SPIR-V 1.3 / 1.4 / 1.6 binary word stream (header, word counts, ID bounds,
// required sections, entry point, function balance, and OpReturn / OpEmitMeshTasksEXT terminators).
Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv);

// Validates a SPIR-V binary using Khronos spirv-val.exe (temp files strictly under E:\OpenScarlett\.tmp\).
Status ValidateSpirvExternal(
    const std::vector<uint32_t>& spirv,
    std::string& out_diagnostics,
    const std::string& spirv_val_path = kDefaultSpirvValPath,
    const std::string& target_env = "vulkan1.2");

// Emits clean, compilable HLSL (SM 6.5: cs_6_5/vs_6_5/ps_6_5/ms_6_5/as_6_5, or SM 5.0 fallback).
Status EmitHlsl(
    const Rdna2IrModule& ir,
    std::string& out_hlsl,
    std::string& out_target,
    bool use_sm65 = true);

// Compiles HLSL source to real DXIL bytecode using Microsoft dxc.exe (temp files strictly under %TEMP%\openscarlett_lifter).
Status CompileHlslWithDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path = {});

// Full RDNA2 XS2\0 or Xbox-wrapped DXBC lifting pipeline: Parse -> Disasm -> SSA IR -> Optimize -> SPIR-V + HLSL (+ optional spirv-val/DXC).
Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options = {});

// ---------------------------------------------------------------------------
// LDS-backed 64-lane Wave64-on-Wave32 Emulation Helper Emission
// (ballot64, mbcnt64_lo/hi, readlane64, cross-half lane exchange via LDS + barrier)
// ---------------------------------------------------------------------------
struct Wave64LdsEmulationConfig {
    uint32_t threadgroup_size = 64u;       // Rounded up to multiple of 64 (64, 128, 256, ...)
    uint32_t max_guest_waves = 1u;         // Minimum guest Wave64 slots allocated in LDS
    bool emit_ballot64 = true;
    bool emit_mbcnt64 = true;
    bool emit_readlane64 = true;
    bool emit_cross_half_exchange = true;
};

// Emits reusable HLSL Wave64-on-Wave32 LDS emulation helper functions (ballot64, mbcnt64_lo/hi,
// readlane64, cross_half_exchange64) backed by groupshared memory + GroupMemoryBarrierWithGroupSync().
std::string EmitWave64On32LdsHlslLibrary(const Wave64LdsEmulationConfig& config = {});

// Emits a strictly valid SPIR-V 1.3 compute module implementing multi-wave isolated LDS-backed
// Wave64-on-Wave32 emulation (ballot64, mbcnt64_lo/hi, readlane64, cross-half lane exchange).
Status EmitWave64On32LdsSpirv(
    const Wave64LdsEmulationConfig& config,
    std::vector<uint32_t>& out_spirv);

// ---------------------------------------------------------------------------
// Paired Barycentric & Vertex Cull-Distance Stage Lowering (VS / GS / MS)
// Preserves paired (I, J) / float3 barycentrics and SV_CullDistance / BuiltIn CullDistance.
// ---------------------------------------------------------------------------
struct Rdna2BarycentricCullStageDesc {
    ShaderStage stage = ShaderStage::Vertex;  // Vertex, Geometry, or Mesh
    uint32_t cull_distance_count = 4u;        // 1..8 floats (clamped to [1, 8] when enabled)
    bool preserve_paired_barycentrics = true;
    bool preserve_cull_distance = true;
    uint32_t max_vertices = 3u;               // GS maxvertexcount or MS OutputVertices
    uint32_t max_primitives = 1u;             // MS OutputPrimitivesEXT
    uint32_t threadgroup_x = 32u;             // MS LocalSize.x
};

struct Rdna2BarycentricCullLiftResult {
    ShaderStage stage = ShaderStage::Unknown;
    std::string hlsl_source;
    std::string hlsl_target;
    std::vector<uint32_t> spirv_words;
    uint32_t preserved_cull_distances = 0u;
    bool has_paired_barycentrics = false;
};

// Lowers VS, GS, or MS stage preserving paired barycentric coordinates and vertex cull-distance
// outputs (SV_CullDistance in SM 6.5 HLSL; OpCapability CullDistance + BuiltIn CullDistance in SPIR-V).
Status LowerRdna2StageWithBarycentricAndCullDistance(
    const Xs2ShaderContainer& xs2,
    const Rdna2BarycentricCullStageDesc& desc,
    Rdna2BarycentricCullLiftResult& out_result);

}  // namespace openscarlett
