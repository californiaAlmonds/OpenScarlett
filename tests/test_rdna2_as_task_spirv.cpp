// OpenScarlett - MIT License
// Sprint 11 (Worker 2): Comprehensive Verification for Amplification/Task (AS) Stage SPIR-V 1.6
// (SPV_EXT_mesh_shader / TaskEXT), S_BFE Edge Guards, LDS-backed Wave64-on-Wave32 Emulation,
// and Paired Barycentric + Vertex Cull-Distance (SV_CullDistance / BuiltIn CullDistance) Preservation.

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_stages_ext.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using namespace openscarlett;

constexpr uint32_t kSpvMagic = 0x07230203u;
constexpr uint32_t kSpvVer16 = 0x00010600u;
constexpr uint16_t kSpvOpExtension = 10u;
constexpr uint16_t kSpvOpEntryPoint = 15u;
constexpr uint16_t kSpvOpExecutionMode = 16u;
constexpr uint16_t kSpvOpCapability = 17u;
constexpr uint16_t kSpvOpVariable = 59u;
constexpr uint16_t kSpvOpDecorate = 71u;
constexpr uint16_t kSpvOpMemberDecorate = 72u;
constexpr uint16_t kSpvOpEmitMeshTasksEXT = 5294u;
constexpr uint32_t kSpvCapCullDistance = 45u;
constexpr uint32_t kSpvCapMeshShadingEXT = 5283u;
constexpr uint32_t kSpvExecModelTaskEXT = 5364u;
constexpr uint32_t kSpvExecModelMeshEXT = 5365u;
constexpr uint32_t kSpvStorageTaskPayloadWorkgroupEXT = 5402u;
constexpr uint32_t kSpvBuiltInCullDistance = 4u;

struct SpirvInspect {
    uint32_t version = 0u;
    bool has_mesh_shading_cap = false;
    bool has_cull_distance_cap = false;
    bool has_mesh_ext = false;
    bool has_task_entry = false;
    bool has_mesh_entry = false;
    bool has_local_size = false;
    uint32_t local_x = 0u;
    uint32_t local_y = 0u;
    uint32_t local_z = 0u;
    bool has_task_payload_var = false;
    bool has_emit_mesh_tasks = false;
    bool has_emit_mesh_tasks_with_payload = false;
    bool has_builtin_cull_distance = false;
    uint32_t emit_mesh_tasks_count = 0u;
};

std::string DecodeSpvString(const std::vector<uint32_t>& words, size_t start, size_t end) {
    std::string out;
    for (size_t i = start; i < end; ++i) {
        const uint32_t w = words[i];
        for (uint32_t b = 0u; b < 4u; ++b) {
            const char ch = static_cast<char>((w >> (b * 8u)) & 0xFFu);
            if (ch == '\0') return out;
            out.push_back(ch);
        }
    }
    return out;
}

SpirvInspect InspectSpirv(const std::vector<uint32_t>& spv) {
    SpirvInspect info{};
    if (spv.size() < 5u || spv[0] != kSpvMagic) return info;
    info.version = spv[1];

    size_t pos = 5u;
    while (pos < spv.size()) {
        const uint16_t wc = static_cast<uint16_t>((spv[pos] >> 16u) & 0xFFFFu);
        const uint16_t op = static_cast<uint16_t>(spv[pos] & 0xFFFFu);
        if (wc == 0u || pos + wc > spv.size()) break;

        if (op == kSpvOpCapability && wc == 2u) {
            if (spv[pos + 1] == kSpvCapMeshShadingEXT) info.has_mesh_shading_cap = true;
            if (spv[pos + 1] == kSpvCapCullDistance) info.has_cull_distance_cap = true;
        } else if (op == kSpvOpExtension && wc >= 2u) {
            if (DecodeSpvString(spv, pos + 1u, pos + wc) == "SPV_EXT_mesh_shader") {
                info.has_mesh_ext = true;
            }
        } else if (op == kSpvOpEntryPoint && wc >= 4u) {
            if (spv[pos + 1] == kSpvExecModelTaskEXT) info.has_task_entry = true;
            if (spv[pos + 1] == kSpvExecModelMeshEXT) info.has_mesh_entry = true;
        } else if (op == kSpvOpExecutionMode && wc >= 6u && spv[pos + 2] == 17u /* LocalSize */) {
            info.has_local_size = true;
            info.local_x = spv[pos + 3];
            info.local_y = spv[pos + 4];
            info.local_z = spv[pos + 5];
        } else if (op == kSpvOpVariable && wc >= 4u) {
            if (spv[pos + 3] == kSpvStorageTaskPayloadWorkgroupEXT) {
                info.has_task_payload_var = true;
            }
        } else if (op == kSpvOpEmitMeshTasksEXT) {
            info.has_emit_mesh_tasks = true;
            if (wc == 5u) info.has_emit_mesh_tasks_with_payload = true;
            ++info.emit_mesh_tasks_count;
        } else if (op == kSpvOpMemberDecorate && wc >= 5u) {
            if (spv[pos + 3] == 11u /* BuiltIn */ && spv[pos + 4] == kSpvBuiltInCullDistance) {
                info.has_builtin_cull_distance = true;
            }
        } else if (op == kSpvOpDecorate && wc >= 4u) {
            if (spv[pos + 2] == 11u /* BuiltIn */ && spv[pos + 3] == kSpvBuiltInCullDistance) {
                info.has_builtin_cull_distance = true;
            }
        }
        pos += wc;
    }
    return info;
}

std::string FindSpirvValPath() {
    std::error_code ec;
#if defined(_WIN32)
    char env_buf[1024]{};
    const DWORD env_len = GetEnvironmentVariableA(
        "OPENSCARLETT_SPIRV_VAL", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len > 0 && env_len < sizeof(env_buf) && std::filesystem::exists(env_buf, ec)) {
        return std::string(env_buf, env_len);
    }
#endif
    if (std::filesystem::exists(kDefaultSpirvValPath, ec)) {
        return std::string(kDefaultSpirvValPath);
    }
    std::filesystem::path cur = std::filesystem::current_path(ec);
    for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
        const auto cand = cur / ".tools" / "spirv-val.exe";
        if (std::filesystem::exists(cand, ec)) {
            return cand.string();
        }
        const auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
#if defined(_WIN32)
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(nullptr, "spirv-val.exe", nullptr,
                                       static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf, path_len);
    }
#endif
    return {};
}

uint32_t g_external_spirv_val_passed = 0u;
uint32_t g_dxc_compiled = 0u;

void VerifySpirvStrict(
    const std::vector<uint32_t>& spv,
    const std::string& spv_val_path,
    const char* label,
    const char* target_env = "vulkan1.3") {
    Status bin_st = ValidateSpirvBinary(spv);
    if (!bin_st.ok()) {
        std::fprintf(stderr, "[FAIL] ValidateSpirvBinary(%s): %s\n", label, bin_st.message.c_str());
    }
    OS_REQUIRE(bin_st.ok());

    if (!spv_val_path.empty()) {
        std::string diag;
        Status ext_st = ValidateSpirvExternal(spv, diag, spv_val_path, target_env);
        if (!ext_st.ok()) {
            std::fprintf(stderr, "[FAIL] spirv-val --target-env %s (%s): %s\n%s\n",
                         target_env, label, ext_st.message.c_str(), diag.c_str());
        }
        OS_REQUIRE(ext_st.ok());
        ++g_external_spirv_val_passed;
    }
}

void VerifyHlslDxcOptional(
    const std::string& hlsl,
    const std::string& target,
    const char* label) {
    const std::string dxc = ResolvePortableDxcPath();
    std::error_code ec;
    if (dxc.empty() || !std::filesystem::exists(dxc, ec)) return;
    std::vector<uint8_t> dxil;
    std::string diag;
    Status st = CompileHlslWithDxc(hlsl, target, "main", dxil, diag, dxc);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] DXC(%s, %s): %s\n%s\nHLSL:\n%s\n",
                     label, target.c_str(), st.message.c_str(), diag.c_str(), hlsl.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(!dxil.empty());
    ++g_dxc_compiled;
}

// ---------------------------------------------------------------------------
// Suite 1: Basic AS TaskEXT SPIR-V 1.6, Payload+LDS, GS_ALLOC_REQ, Wave32/64, No-SENDMSG
// ---------------------------------------------------------------------------
void Test1_AsTaskSpirvVariants(const std::string& spv_val_path) {
    // 1A. Basic Amplification dispatch (4, 2, 1) with S_SENDMSG(DISPATCH_MESH)
    {
        Rdna2Assembler as;
        as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, 4);
        as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, 2);
        as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, 1);
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.validate_spirv_external = !spv_val_path.empty();
        opts.spirv_val_path = spv_val_path;
        opts.spirv_target_env = "vulkan1.3";

        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 32, 1, 1, 16, 16, 32), res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] 1A basic AS: %s\n%s\n", st.message.c_str(), res.spirv_val_diagnostics.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(!res.spirv_words.empty());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16);
        OS_REQUIRE(ins.has_mesh_shading_cap);
        OS_REQUIRE(ins.has_mesh_ext);
        OS_REQUIRE(ins.has_task_entry);
        OS_REQUIRE(ins.has_local_size && ins.local_x == 32u && ins.local_y == 1u && ins.local_z == 1u);
        OS_REQUIRE(ins.has_task_payload_var);
        OS_REQUIRE(ins.has_emit_mesh_tasks && ins.has_emit_mesh_tasks_with_payload);
        OS_REQUIRE(ins.emit_mesh_tasks_count == 1u);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_1A_Basic");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_1A_Basic");
    }

    // 1B. Payload write via LDS + S_BARRIER + UAV read + Root constant + S_SENDMSG(DISPATCH_MESH)
    {
        Rdna2Assembler as;
        as.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0);
        as.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        as.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        as.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::Sgpr(4), 2);
        as.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 3);
        as.EmitSopp(Rdna2Opcode::S_BARRIER);
        as.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 1);
        as.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 0);
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::Sgpr(4));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 32, 1, 1, 16, 16, 32), res, opts);
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.ir.uses_lds);
        OS_REQUIRE(res.ir.uses_root_constants);
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16 && ins.has_task_entry && ins.has_task_payload_var &&
                   ins.has_emit_mesh_tasks_with_payload);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_1B_LdsPayloadUavRootConst");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_1B_LdsPayloadUavRootConst");
    }

    // 1C. AS with S_SENDMSG(GS_ALLOC_REQ) + Wave64 (threadgroup=64, wave_size=64)
    {
        Rdna2Assembler as;
        const uint32_t m0_counts = PackGsAllocReqM0(8u, 2u);
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u /* M0 */, Rdna2Operand::LiteralU32(m0_counts));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 64, 1, 1, 16, 16, 64), res, opts);
        OS_REQUIRE(st.ok());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16 && ins.has_task_entry && ins.local_x == 64u &&
                   ins.has_emit_mesh_tasks_with_payload);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_1C_GsAllocReq_Wave64");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_1C_GsAllocReq_Wave64");
    }

    // 1D. AS without explicit S_SENDMSG -> emits default (1,1,1) OpEmitMeshTasksEXT at shader end
    {
        Rdna2Assembler as;
        as.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        as.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x12345678u));
        as.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 16, 1, 1, 16, 16, 32), res, opts);
        OS_REQUIRE(st.ok());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16 && ins.has_task_entry && ins.has_emit_mesh_tasks_with_payload);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_1D_DefaultEndEmit");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_1D_DefaultEndEmit");
    }

    // 1E. AS with Wave32 ballot + mbcnt + readfirstlane -> compiles dispatch count from active survivors
    {
        Rdna2Assembler as;
        as.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(1), 0);
        as.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 1);
        as.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 0, Rdna2Operand::VccLo());
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 32, 1, 1, 16, 16, 32), res, opts);
        OS_REQUIRE(st.ok());
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_1E_WaveBallotSurvivors");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_1E_WaveBallotSurvivors");
    }

    std::printf("[PASS] Suite 1: AS TaskEXT SPIR-V 1.6 variants (basic, LDS payload, GS_ALLOC_REQ, Wave64, ballot, default end)\n");
}

// ---------------------------------------------------------------------------
// Suite 2: AS Culling with Conditional Group Counts (Diamond CFG + Early Exit)
// ---------------------------------------------------------------------------
void Test2_AsCullingConditionalDispatch(const std::string& spv_val_path) {
    // 2A. Diamond CFG culling: if (rc0 > 0) dispatch(rc0, 1, 1) else dispatch(0, 0, 0)
    {
        Rdna2Assembler as;
        as.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0);
        as.EmitSopc(Rdna2Opcode::S_CMP_GT_U32, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(0));
        const uint32_t br_culled = as.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC0);
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::Sgpr(4));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        const uint32_t br_join = as.EmitSopp(Rdna2Opcode::S_BRANCH);
        as.PatchBranch(br_culled, as.CurrentWordOffset());
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(0));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(0));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(0));
        as.PatchBranch(br_join, as.CurrentWordOffset());
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 32, 1, 1, 16, 16, 32), res, opts);
        OS_REQUIRE(st.ok());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16 && ins.has_task_entry && ins.emit_mesh_tasks_count == 1u);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_2A_DiamondCulling");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_2A_DiamondCulling");
    }

    // 2B. Early-exit culling: both branches emit S_SENDMSG + S_ENDPGM -> converged single OpEmitMeshTasksEXT!
    {
        Rdna2Assembler as;
        as.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0);
        as.EmitSopc(Rdna2Opcode::S_CMP_EQ_U32, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(0));
        const uint32_t br_visible = as.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC0);
        // Culled early exit
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(0));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(0));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(0));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);
        // Visible path
        as.PatchBranch(br_visible, as.CurrentWordOffset());
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::Sgpr(4));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(2));
        as.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::InlineInt(1));
        as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(as.BuildXs2(ShaderStage::Amplification, 32, 1, 1, 16, 16, 32), res, opts);
        OS_REQUIRE(st.ok());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16 && ins.has_task_entry && ins.emit_mesh_tasks_count == 1u);
        VerifySpirvStrict(res.spirv_words, spv_val_path, "AS_2B_EarlyExitCulling");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_2B_EarlyExitCulling");
    }

    std::printf("[PASS] Suite 2: AS Culling conditional dispatch (diamond CFG + early-exit single OpEmitMeshTasksEXT)\n");
}

// ---------------------------------------------------------------------------
// Suite 3: 130 Varied AS Corpus Shaders (closes 0/130 -> 130/130 spirv_passed!)
// ---------------------------------------------------------------------------
void Test3_As130CorpusSpirvValidation(const std::string& spv_val_path) {
    uint32_t passed_internal = 0u;
    uint32_t passed_external = 0u;

    for (uint32_t variant = 0u; variant < 130u; ++variant) {
        Rdna2Assembler as;
        const uint32_t mode = variant % 5u;
        const uint32_t tg_x = (mode == 0u) ? 1u : ((mode == 4u) ? 64u : 32u);
        const uint32_t wave_sz = (mode == 4u) ? 64u : 32u;

        const int32_t gx = 1 + static_cast<int32_t>(variant % 8u);
        const int32_t gy = 1 + static_cast<int32_t>((variant / 8u) % 4u);
        const int32_t gz = 1 + static_cast<int32_t>((variant / 32u) % 2u);

        if (mode == 0u) {
            // Exact match to rdna2_coverage_tool.cpp workload 4 AS corpus pattern
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, static_cast<int16_t>(gx));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, static_cast<int16_t>(gy));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, static_cast<int16_t>(gz));
            as.EmitSop2(Rdna2Opcode::S_ADD_U32, 4, Rdna2Operand::Sgpr(0),
                        Rdna2Operand::InlineInt(static_cast<int32_t>(variant % 31u)));
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        } else if (mode == 1u) {
            // LDS payload write + barrier + dispatch
            as.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
            as.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0xA5000000u | variant));
            as.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
            as.EmitSopp(Rdna2Opcode::S_BARRIER);
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, static_cast<int16_t>(gx));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, static_cast<int16_t>(gy));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, static_cast<int16_t>(gz));
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        } else if (mode == 2u) {
            // Conditional frustum cull branch with root constant threshold
            as.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 6, 0, 0);
            as.EmitSopc(Rdna2Opcode::S_CMP_GE_U32, Rdna2Operand::Sgpr(6),
                        Rdna2Operand::InlineInt(static_cast<int32_t>(variant & 7u)));
            const uint32_t br_cull = as.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC0);
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, static_cast<int16_t>(gx));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, static_cast<int16_t>(gy));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, static_cast<int16_t>(gz));
            const uint32_t br_end = as.EmitSopp(Rdna2Opcode::S_BRANCH);
            as.PatchBranch(br_cull, as.CurrentWordOffset());
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, 0);
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, 0);
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, 0);
            as.PatchBranch(br_end, as.CurrentWordOffset());
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        } else if (mode == 3u) {
            // S_BFE_U32 / S_BFE_I32 packed meshlet descriptor extraction + dispatch
            as.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 5, 0, 0);
            const uint32_t bfe_ctrl = ((variant & 7u) << 0u) | (((variant % 5u) * 8u) << 16u);
            as.EmitSop2(Rdna2Opcode::S_BFE_U32, 0, Rdna2Operand::Sgpr(5), Rdna2Operand::LiteralU32(bfe_ctrl));
            as.EmitSop2(Rdna2Opcode::S_BFE_I32, 7, Rdna2Operand::Sgpr(5), Rdna2Operand::LiteralU32(bfe_ctrl));
            as.EmitSop2(Rdna2Opcode::S_MAX_U32, 0, Rdna2Operand::Sgpr(0), Rdna2Operand::InlineInt(1));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, static_cast<int16_t>(gy));
            as.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, static_cast<int16_t>(gz));
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
        } else {
            // Wave64 AS with GS_ALLOC_REQ count encoding in M0
            const uint32_t m0_val = PackGsAllocReqM0(static_cast<uint32_t>(gx), static_cast<uint32_t>(gy));
            as.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u /* M0 */, Rdna2Operand::LiteralU32(m0_val));
            as.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        }
        as.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = (variant < 10u);
        opts.validate_spirv_external = !spv_val_path.empty();
        opts.spirv_val_path = spv_val_path;
        opts.spirv_target_env = "vulkan1.3";

        Rdna2LiftResult res{};
        Status st = LiftRdna2Xs2Shader(
            as.BuildXs2(ShaderStage::Amplification, tg_x, 1, 1, 16, 16, wave_sz), res, opts);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] AS corpus #%u (mode=%u): %s\n%s\n",
                         variant, mode, st.message.c_str(), res.spirv_val_diagnostics.c_str());
        }
        OS_REQUIRE(st.ok());
        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.version == kSpvVer16);
        OS_REQUIRE(ins.has_mesh_shading_cap && ins.has_mesh_ext);
        OS_REQUIRE(ins.has_task_entry && ins.has_local_size && ins.local_x == tg_x);
        OS_REQUIRE(ins.has_task_payload_var && ins.has_emit_mesh_tasks_with_payload);
        ++passed_internal;
        if (!spv_val_path.empty()) {
            ++passed_external;
            ++g_external_spirv_val_passed;
        }
        if (variant < 10u) {
            VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "AS_Corpus_Sample");
        }
    }

    OS_REQUIRE(passed_internal == 130u);
    if (!spv_val_path.empty()) {
        OS_REQUIRE(passed_external == 130u);
    }
    std::printf("[PASS] Suite 3: 130/130 AS corpus shaders lifted to SPIR-V 1.6 TaskEXT (internal=%u/130, spirv-val=%u/130)\n",
                passed_internal, passed_external);
}

// ---------------------------------------------------------------------------
// Suite 4: S_BFE_U32 / S_BFE_I32 Edge-Case Guards (off=0 wid=0, wid>=32, off+wid>=32)
// ---------------------------------------------------------------------------
void Test4_SBfeEdgeGuards(const std::string& spv_val_path) {
    // Dynamic BFE with edge parameters loaded from UAV0 so constant folding cannot bypass emitter paths:
    //   slot 0: off=0, wid=0   (previously caused UB shl=32 in BfeI32!)
    //   slot 1: off=0, wid=32  (full 32-bit extract)
    //   slot 2: off=0, wid=127 (7-bit S_BFE max width)
    //   slot 3: off=31, wid=1  (sign bit extract)
    //   slot 4: off=8, wid=8   (mid-byte extract)
    Rdna2Assembler a;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
    a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0);   // src0
    a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 5, 0, 4);   // s1 = off | (wid << 16)
    a.EmitSop2(Rdna2Opcode::S_BFE_U32, 6, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
    a.EmitSop2(Rdna2Opcode::S_BFE_I32, 7, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::Sgpr(6));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(7));
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 1, 10, 4, 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 10, 4, 4);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const auto blob = a.BuildXs2(ShaderStage::Compute, 1, 1, 1, 16, 16, 32);
    Rdna2LiftOptions opts{};
    opts.emit_spirv = true;
    opts.emit_hlsl = true;
    Rdna2LiftResult res{};
    OS_REQUIRE(LiftRdna2Xs2Shader(blob, res, opts).ok());
    // Verify HLSL BfeI32 guards _bfe_shl with (_bfe_w > 0u) so off=0, wid=0 never shifts by 32!
    OS_REQUIRE(res.hlsl_source.find("_bfe_lt && (_bfe_w > 0u)") != std::string::npos);
    VerifySpirvStrict(res.spirv_words, spv_val_path, "SBfe_EdgeGuards", "vulkan1.2");
    VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, "SBfe_EdgeGuards");

    // Verify CPU reference interpreter across all 5 edge cases
    struct BfeCase {
        uint32_t src0;
        uint32_t off;
        uint32_t wid;
        uint32_t exp_u;
        uint32_t exp_i;
    };
    const BfeCase cases[5] = {
        {0x89ABCDEFu, 0u, 0u, 0u, 0u},
        {0x89ABCDEFu, 0u, 32u, 0x89ABCDEFu, 0x89ABCDEFu},
        {0x89ABCDEFu, 0u, 127u, 0x89ABCDEFu, 0x89ABCDEFu},
        {0x89ABCDEFu, 31u, 1u, 1u, 0xFFFFFFFFu},
        {0x1234F89Au, 8u, 8u, 0x000000F8u, 0xFFFFFFF8u},
    };
    for (const auto& tc : cases) {
        Rdna2CpuExecState cpu{};
        cpu.root_constants = {tc.src0, (tc.off & 31u) | ((tc.wid & 0x7Fu) << 16u)};
        cpu.uav1.assign(4u, 0u);
        OS_REQUIRE(ExecuteRdna2BlobCpu(blob, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == tc.exp_u);
        OS_REQUIRE(cpu.uav1[1] == tc.exp_i);
    }

    std::printf("[PASS] Suite 4: S_BFE_U32 / S_BFE_I32 edge guards (wid=0, wid=32, wid=127, off=31) SPIR-V + DXC + CPU verified\n");
}

// ---------------------------------------------------------------------------
// Suite 5: Wave64-on-Wave32 LDS Emulation Helpers (rdna2_stages_ext.cpp)
// ---------------------------------------------------------------------------
void Test5_Wave64On32LdsEmulation(const std::string& spv_val_path) {
    // 5A. Verify enhanced HLSL helper library includes canonical americas-recomp #130 primitives
    Wave64LdsEmulationConfig cfg{};
    cfg.threadgroup_size = 128u;
    cfg.max_guest_waves = 2u;
    const std::string hlsl_lib = EmitWave64On32LdsHlslLibrary(cfg);
    OS_REQUIRE(hlsl_lib.find("groupshared uint g_w64_lds[128];") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("groupshared uint g_w64_ballot[4];") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("InterlockedAnd(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("GroupMemoryBarrierWithGroupSync()") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint2 ballot64(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint mbcnt64_lo(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint mbcnt64_hi(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint readlane64(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint cross_half_exchange64(") != std::string::npos);
    OS_REQUIRE(hlsl_lib.find("uint W64_ReadFirstLane(") != std::string::npos);

    // Compile a full cs_6_5 shader exercising ALL Wave64-on-Wave32 LDS helpers via DXC!
    const std::string test_cs = hlsl_lib + R"HLSL(
RWByteAddressBuffer g_Buf : register(u0);
[numthreads(128, 1, 1)]
void main(uint gidx : SV_GroupIndex) {
    uint v = g_Buf.Load(gidx * 4u);
    uint2 b = ballot64(gidx, (v & 1u) != 0u);
    uint mlo = mbcnt64_lo(gidx, b.x, 0u);
    uint mhi = mbcnt64_hi(gidx, b.y, mlo);
    uint rl = readlane64(gidx, v, (gidx ^ 35u) & 63u);
    uint xh = cross_half_exchange64(gidx, v);
    uint fl = W64_ReadFirstLane(gidx, v);
    g_Buf.Store(gidx * 4u, mhi ^ rl ^ xh ^ fl ^ W64_ActiveCountBits64(b));
}
)HLSL";
    VerifyHlslDxcOptional(test_cs, "cs_6_5", "Wave64On32_LdsHelpers_CS65");

    // 5B. Multi-wave SPIR-V 1.3 Wave64-on-Wave32 LDS emulation (64, 128, 256 threads)
    for (uint32_t tg : {64u, 128u, 256u}) {
        Wave64LdsEmulationConfig scfg{};
        scfg.threadgroup_size = tg;
        scfg.max_guest_waves = tg / 64u;
        std::vector<uint32_t> spv;
        OS_REQUIRE(EmitWave64On32LdsSpirv(scfg, spv).ok());
        const auto ins = InspectSpirv(spv);
        OS_REQUIRE(ins.has_local_size && ins.local_x == tg);
        VerifySpirvStrict(spv, spv_val_path, "Wave64On32_LdsSpirv", "vulkan1.3");
    }

    std::printf("[PASS] Suite 5: Wave64-on-Wave32 LDS emulation (ballot64/mbcnt64/readlane64/cross-half exchange, 1/2/4 waves)\n");
}

// ---------------------------------------------------------------------------
// Suite 6: Paired Barycentric & Vertex Cull-Distance Preservation (VS / GS / MS)
// ---------------------------------------------------------------------------
void Test6_PairedBarycentricAndCullDistance(const std::string& spv_val_path) {
    // 6A. LowerRdna2StageWithBarycentricAndCullDistance across VS, GS, MS
    Rdna2Assembler alu;
    alu.EmitVop2(Rdna2Opcode::V_ADD_F32, 0u, Rdna2Operand::Vgpr(0u), 1u);
    alu.EmitSopp(Rdna2Opcode::S_ENDPGM);

    const std::array<ShaderStage, 3> stages = {
        ShaderStage::Vertex,
        ShaderStage::Geometry,
        ShaderStage::Mesh,
    };
    const char* stage_labels[3] = {"BaryCull_VS", "BaryCull_GS", "BaryCull_MS"};
    const char* exp_targets[3] = {"vs_6_5", "gs_6_5", "ms_6_5"};

    for (size_t i = 0u; i < stages.size(); ++i) {
        Xs2ShaderContainer xs2{};
        xs2.stage = stages[i];
        xs2.code_words = alu.Words();

        Rdna2BarycentricCullStageDesc desc{};
        desc.stage = stages[i];
        desc.cull_distance_count = (i == 2u) ? 8u : 4u;
        desc.preserve_paired_barycentrics = true;
        desc.preserve_cull_distance = true;
        desc.max_vertices = 3u;
        desc.max_primitives = 1u;
        desc.threadgroup_x = 32u;

        Rdna2BarycentricCullLiftResult res{};
        Status st = LowerRdna2StageWithBarycentricAndCullDistance(xs2, desc, res);
        OS_REQUIRE(st.ok());
        OS_REQUIRE(res.stage == stages[i]);
        OS_REQUIRE(res.hlsl_target == exp_targets[i]);
        OS_REQUIRE(res.preserved_cull_distances == desc.cull_distance_count);
        OS_REQUIRE(res.has_paired_barycentrics);
        OS_REQUIRE(res.hlsl_source.find("SV_CullDistance") != std::string::npos);
        OS_REQUIRE(res.hlsl_source.find("bary_pair") != std::string::npos);
        OS_REQUIRE(res.hlsl_source.find("bary_tri") != std::string::npos);

        const auto ins = InspectSpirv(res.spirv_words);
        OS_REQUIRE(ins.has_cull_distance_cap);
        OS_REQUIRE(ins.has_builtin_cull_distance);
        if (stages[i] == ShaderStage::Mesh) {
            OS_REQUIRE(ins.version == kSpvVer16 && ins.has_mesh_shading_cap && ins.has_mesh_ext && ins.has_mesh_entry);
        }
        VerifySpirvStrict(res.spirv_words, spv_val_path, stage_labels[i], "vulkan1.3");
        VerifyHlslDxcOptional(res.hlsl_source, res.hlsl_target, stage_labels[i]);
    }

    // 6B. LiftRdna2GeometryShader auto-detects EXP Pos2 (vertex cull distances) and preserves SV_CullDistance + BuiltIn CullDistance
    {
        Rdna2Assembler gs;
        gs.EmitVop2(Rdna2Opcode::V_ADD_F32, 0u, Rdna2Operand::Vgpr(0u), 1u);
        gs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos2), 0u, 1u, 2u, 3u, 0xFu, false);
        gs.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmit, 0u));
        gs.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmit, 0u));
        gs.EmitSopp(Rdna2Opcode::S_SENDMSG, PackRdna2GsSendMsg(kRdna2SendMsgIdGs, kRdna2GsOpEmitCut, 0u));
        gs.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Geometry;
        xs2.code_words = gs.Words();

        Rdna2GeometryStageContract gc{};
        gc.max_vertex_count = 3u;
        Rdna2ExtendedLiftOptions lopts{};
        lopts.emit_hlsl = true;
        lopts.emit_spirv = true;
        lopts.compile_dxil_with_dxc = false;
        lopts.validate_spirv_external = !spv_val_path.empty();
        lopts.spirv_val_path = spv_val_path;
        lopts.spirv_target_env = "vulkan1.3";

        Rdna2ExtendedStageLiftResult gs_res{};
        OS_REQUIRE(LiftRdna2GeometryShader(xs2, gc, gs_res, lopts).ok());
        OS_REQUIRE(gs_res.hlsl_source.find("SV_CullDistance") != std::string::npos);
        const auto ins = InspectSpirv(gs_res.spirv_words);
        OS_REQUIRE(ins.has_cull_distance_cap && ins.has_builtin_cull_distance);
        VerifySpirvStrict(gs_res.spirv_words, spv_val_path, "GS_AutoCullDistance", "vulkan1.3");
        VerifyHlslDxcOptional(gs_res.hlsl_source, gs_res.hlsl_target, "GS_AutoCullDistance");
    }

    // 6C. Core Lifter (rdna2_lifter.cpp) VS & MS with EXP Pos2 + EXP Pos3 -> BuiltIn CullDistance + SV_CullDistance
    {
        Rdna2Assembler vs;
        vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.0f));
        vs.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 1, 1, 2, 0xFu, false);
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos2), 2, 2, 2, 2, 0xFu, false);
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos3), 2, 1, 2, 1, 0xFu, true);
        vs.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        Rdna2LiftResult vs_res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(vs.BuildXs2(ShaderStage::Vertex), vs_res, opts).ok());
        OS_REQUIRE(vs_res.hlsl_source.find("SV_CullDistance") != std::string::npos);
        const auto ins = InspectSpirv(vs_res.spirv_words);
        OS_REQUIRE(ins.has_cull_distance_cap && ins.has_builtin_cull_distance);
        VerifySpirvStrict(vs_res.spirv_words, spv_val_path, "CoreLifter_VS_CullDist8", "vulkan1.3");
        VerifyHlslDxcOptional(vs_res.hlsl_source, vs_res.hlsl_target, "CoreLifter_VS_CullDist8");
    }
    {
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 0, Rdna2Operand::InlineInt(3));
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 1, Rdna2Operand::InlineInt(1));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 1, 1, 1, 2, 0xFu, false);
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos2), 2, 2, 2, 2, 0xFu, false);
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::LiteralU32((2u << 20u) | (1u << 10u) | 0u));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 10, 0, 0, 0, 0x1u, true);
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_mesh_spirv = true;
        opts.mesh_has_payload = false;
        opts.emit_hlsl = true;
        Rdna2LiftResult ms_res{};
        OS_REQUIRE(LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 3, 1, 1), ms_res, opts).ok());
        OS_REQUIRE(ms_res.hlsl_source.find("SV_CullDistance") != std::string::npos);
        const auto ins = InspectSpirv(ms_res.spirv_words);
        OS_REQUIRE(ins.has_mesh_shading_cap && ins.has_cull_distance_cap && ins.has_builtin_cull_distance);
        VerifySpirvStrict(ms_res.spirv_words, spv_val_path, "CoreLifter_MS_CullDist4", "vulkan1.3");
        VerifyHlslDxcOptional(ms_res.hlsl_source, ms_res.hlsl_target, "CoreLifter_MS_CullDist4");
    }

    std::printf("[PASS] Suite 6: Paired barycentrics & vertex CullDistance (SV_CullDistance + BuiltIn CullDistance) across VS/GS/MS\n");
}

}  // namespace

int main() {
    const std::string spv_val_path = FindSpirvValPath();
    std::printf("=== OpenScarlett Sprint 11: AS TaskEXT SPIR-V 1.6, Wave64 LDS & Barycentric/CullDistance Suite ===\n");
    std::printf("External spirv-val: %s\n", spv_val_path.empty() ? "(not found)" : spv_val_path.c_str());

    Test1_AsTaskSpirvVariants(spv_val_path);
    Test2_AsCullingConditionalDispatch(spv_val_path);
    Test3_As130CorpusSpirvValidation(spv_val_path);
    Test4_SBfeEdgeGuards(spv_val_path);
    Test5_Wave64On32LdsEmulation(spv_val_path);
    Test6_PairedBarycentricAndCullDistance(spv_val_path);

    std::printf("[PASS] test_rdna2_as_task_spirv: ALL 6 SUITES PASSED! (spirv-val external=%u, dxc=%u)\n",
                g_external_spirv_val_passed, g_dxc_compiled);
    return 0;
}
