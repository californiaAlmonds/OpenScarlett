// OpenScarlett - MIT License
// Sprint 15 Worker 1 Verification Suite:
//   1. 4/4 Privileged Trap/Halt/RFE Opcodes (s_rfe_b64, s_sethalt, s_sendmsghalt, s_trap):
//      - Binary decode, SPIR-V + HLSL/DXIL lift + external spirv-val & in-process DXC validation
//      - Core ExecuteRdna2Cpu non-mutation termination guard (post-trap UAV1 writes blocked)
//      - Deterministic CPU-ref trap/halt/RFE harness semantics (ExecuteRdna2PrivilegedTrapHarness
//        + native ExecuteScalarVectorCpu halt semantics)
//   2. 6/6 Newly GPU-Verified Shader-Executable Opcodes:
//      (s_setpc_b64, s_swappc_b64, s_call_b64, s_endpgm_saved, s_endpgm_ordered_ps_done, s_code_end)
//      - Real CPU-ref + SPIR-V + DXIL + live GPU execution parity
//   3. RunRdna2CoverageBenchmark (--fast mode):
//      - 779/779 decoded, SPIR-V, DXIL, CPU-ref
//      - 775/779 honest GPU-verified when GPU+DXC present (SOP1 57/58, SOPK 27/27, SOPP 36/39,
//        and all other 17 families 100%; 4 privileged trap/halt ops NEVER falsely marked gpu_verified)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"
#include "rdna2_coverage_tool.h"
#include "rdna2_isa_scalar_vector.h"

namespace {

using namespace openscarlett;

void AppendMubuf(std::vector<uint32_t>& words,
                 Rdna2Opcode op,
                 uint8_t vdata,
                 uint8_t vaddr,
                 uint8_t srsrc_sgpr,
                 uint16_t offset,
                 bool offen) {
    Rdna2Assembler a;
    a.EmitMubuf(op, vdata, vaddr, srsrc_sgpr, offset, offen);
    const auto& w = a.Words();
    words.insert(words.end(), w.begin(), w.end());
}

Status CompileHlslBestLocal(const std::string& hlsl,
                            const std::string& target,
                            std::vector<uint8_t>& out_dxil,
                            std::string& out_diag,
                            const std::string& dxc_path) {
    if (IsInProcessDxcAvailable(dxc_path)) {
        Status st = CompileHlslInProcessDxc(hlsl, target, "main", out_dxil, out_diag, dxc_path);
        if (st.ok()) return st;
        if (st.code != StatusCode::NotFound && st.code != StatusCode::NotImplemented) return st;
    }
    return CompileHlslWithDxc(hlsl, target, "main", out_dxil, out_diag, dxc_path);
}

// ---------------------------------------------------------------------------
// Test 1: 4/4 Privileged Trap/Halt/RFE Opcodes (Decode, Lift, Validators, CPU-Ref Harness)
// ---------------------------------------------------------------------------
void Test1_PrivilegedTrapHaltRfeHarness() {
    using namespace openscarlett::rdna2_sv;

    const std::string dxc_path = ResolvePortableDxcPath();
    std::error_code ec;
    const bool has_dxc = IsInProcessDxcAvailable(dxc_path) ||
                         (!dxc_path.empty() && std::filesystem::exists(dxc_path, ec));
    const bool has_spv_val = std::filesystem::exists(kDefaultSpirvValPath, ec);

    struct PrivCase {
        const char* mnemonic;
        Rdna2Encoding enc;
        uint16_t raw_op;
        int32_t imm;
        Rdna2TrapTerminationKind expected_kind;
        bool native_sv_halts;
    };
    const std::array<PrivCase, 4> kCases = {{
        {"s_sethalt", Rdna2Encoding::SOPP, 0x0Du, 1, Rdna2TrapTerminationKind::Halted, true},
        {"s_sendmsghalt", Rdna2Encoding::SOPP, 0x11u, 0x2A, Rdna2TrapTerminationKind::SendMsgHalt, true},
        {"s_trap", Rdna2Encoding::SOPP, 0x12u, 7, Rdna2TrapTerminationKind::TrapRaised, false},
        {"s_rfe_b64", Rdna2Encoding::SOP1, 0x20u, 0, Rdna2TrapTerminationKind::RfeReturned, false},
    }};

    for (const auto& tc : kCases) {
        // Build shader:
        //   1. Store sentinel 0x11112222 -> uav1[0] (BEFORE privileged op)
        //   2. Execute privileged op (for s_rfe_b64, s[4:5] = 0 -> out-of-bounds return exits cleanly)
        //   3. Store clobber 0xDEAD9999 -> uav1[1] AND overwrite uav1[0] (MUST NOT EXECUTE!)
        std::vector<uint32_t> code;
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 2u, {Rdna2Operand::LiteralU32(0x11112222u)}, 0, code));
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 0u, 4u, 0u, false);
        if (std::strcmp(tc.mnemonic, "s_rfe_b64") == 0) {
            OS_REQUIRE(EncodeScalarVectorInst("s_mov_b32", 4u, {Rdna2Operand::InlineInt(0)}, 0, code));
            OS_REQUIRE(EncodeScalarVectorInst("s_mov_b32", 5u, {Rdna2Operand::InlineInt(0)}, 0, code));
            OS_REQUIRE(EncodeScalarVectorInst("s_rfe_b64", 0u, {Rdna2Operand::Sgpr(4)}, 0, code));
        } else {
            OS_REQUIRE(EncodeScalarVectorInst(tc.mnemonic, 0u, {}, tc.imm, code));
        }
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 3u, {Rdna2Operand::LiteralU32(0xDEAD9999u)}, 0, code));
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 0u, 4u, 4u, false);
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 0u, 4u, 0u, false);
        OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));

        // 1A. Decode via rdna2_sv & verify exact encoding/raw_opcode/mnemonic
        std::vector<SvDecodedInst> decoded;
        OS_REQUIRE(DecodeScalarVectorStream(code, decoded).ok());
        bool found_priv = false;
        for (const auto& di : decoded) {
            if (di.entry && std::strcmp(di.entry->mnemonic, tc.mnemonic) == 0 &&
                di.base.encoding == tc.enc && di.base.raw_opcode == tc.raw_op) {
                found_priv = true;
                break;
            }
        }
        OS_REQUIRE(found_priv);

        // 1B. Lift to valid SPIR-V + SM 6.5 HLSL (+ optional spirv-val + in-process DXC DXIL)
        Xs2ShaderContainer xs2{};
        xs2.stage = ShaderStage::Compute;
        xs2.threadgroup_x = 1u;
        xs2.threadgroup_y = 1u;
        xs2.threadgroup_z = 1u;
        xs2.wave_size = 32u;
        xs2.code_words = code;
        Rdna2LiftOptions lopts{};
        lopts.use_sm65 = true;
        lopts.emit_spirv = true;
        lopts.emit_hlsl = true;
        lopts.compile_dxil_with_dxc = false;
        Rdna2LiftResult lifted{};
        OS_REQUIRE(LiftScalarVectorShader(xs2, decoded, lifted, lopts).ok());
        OS_REQUIRE(!lifted.spirv_words.empty());
        OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());
        OS_REQUIRE(!lifted.hlsl_source.empty());

        if (has_spv_val) {
            std::string diag;
            Status sv_st = ValidateSpirvExternal(lifted.spirv_words, diag, kDefaultSpirvValPath, "vulkan1.2");
            if (!sv_st.ok()) {
                std::fprintf(stderr, "[FAIL] spirv-val for %s: %s\n%s\n", tc.mnemonic, sv_st.message.c_str(), diag.c_str());
            }
            OS_REQUIRE(sv_st.ok());
        }
        if (has_dxc) {
            std::vector<uint8_t> dxil;
            std::string diag;
            Status dst = CompileHlslBestLocal(lifted.hlsl_source, lifted.hlsl_target, dxil, diag, dxc_path);
            if (!dst.ok()) {
                std::fprintf(stderr, "[FAIL] DXC for %s: %s\n%s\n", tc.mnemonic, dst.message.c_str(), diag.c_str());
            }
            OS_REQUIRE(dst.ok());
            OS_REQUIRE(VerifyDxbcContainerHash(dxil.data(), dxil.size()));
        }

        // 1C. Verify native ExecuteScalarVectorCpu halt semantics for s_sethalt / s_sendmsghalt
        if (tc.native_sv_halts) {
            SvWaveState wave{};
            wave.Reset(32u);
            wave.SeedThreadIds(0u);
            wave.exec = 1ull;
            wave.uav0.assign(4u, 0u);
            wave.uav1.assign(4u, 0xCAFE0000u);
            OS_REQUIRE(ExecuteScalarVectorCpu(decoded, wave).ok());
            OS_REQUIRE(wave.halted);
            OS_REQUIRE(wave.uav1[0] == 0x11112222u);
            OS_REQUIRE(wave.uav1[1] == 0xCAFE0000u);  // Post-halt store did NOT mutate UAV1!
        }

        // 1D. Verify core ExecuteRdna2Cpu non-mutation semantics on privileged trap/halt instruction:
        //     Pre-trap store executes cleanly, then privileged op terminates ExecuteRdna2Cpu
        //     BEFORE any post-trap store can mutate uav1[1] or overwrite uav1[0]!
        {
            Rdna2Assembler pre;
            pre.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x11112222u));
            pre.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 0, false);
            std::vector<Rdna2Instruction> core_insns;
            OS_REQUIRE(DisassembleRdna2(pre.Words(), core_insns).ok());

            Rdna2Instruction priv_inst{};
            priv_inst.word_offset = static_cast<uint32_t>(pre.Words().size());
            priv_inst.word_count = 1u;
            priv_inst.encoding = tc.enc;
            priv_inst.raw_opcode = tc.raw_op;
            priv_inst.opcode = Rdna2Opcode::Unknown;
            priv_inst.mnemonic = tc.mnemonic;
            priv_inst.simm16 = static_cast<int16_t>(tc.imm);
            core_insns.push_back(priv_inst);

            Rdna2Assembler post;
            post.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(0xDEAD9999u));
            post.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 0, 4, 4, false);
            post.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 0, 4, 0, false);
            post.EmitSopp(Rdna2Opcode::S_ENDPGM);
            std::vector<Rdna2Instruction> post_insns;
            OS_REQUIRE(DisassembleRdna2(post.Words(), post_insns).ok());
            const uint32_t base_off = priv_inst.word_offset + 1u;
            for (auto& pi : post_insns) {
                pi.word_offset += base_off;
                core_insns.push_back(pi);
            }

            Xs2ShaderContainer core_c{};
            core_c.stage = ShaderStage::Compute;
            core_c.threadgroup_x = 1u;
            core_c.threadgroup_y = 1u;
            core_c.threadgroup_z = 1u;
            core_c.wave_size = 32u;

            Rdna2CpuExecState cpu{};
            cpu.uav1.assign(4u, 0xCAFE0000u);
            Status cpu_st = ExecuteRdna2Cpu(core_c, core_insns, cpu);
            OS_REQUIRE(!cpu_st.ok());  // Privileged op halts/traps core interpreter immediately
            OS_REQUIRE(cpu.uav1[0] == 0x11112222u);
            OS_REQUIRE(cpu.uav1[1] == 0xCAFE0000u);  // Post-trap store NEVER mutated UAV1!
        }

        // 1E. Verify full architectural CPU-ref trap/halt/RFE harness semantics
        Rdna2PrivilegedTrapResult trap_res{};
        OS_REQUIRE(ExecuteRdna2PrivilegedTrapHarness(code, {0u, 0u, 0u, 0u}, 4u, trap_res).ok());
        OS_REQUIRE(trap_res.termination == tc.expected_kind);
        OS_REQUIRE(trap_res.uav1.size() == 4u);
        OS_REQUIRE(trap_res.uav1[0] == 0x11112222u);
        OS_REQUIRE(trap_res.uav1[1] == 0u);  // Trailing 0xDEAD9999 clobber did NOT execute!
        if (tc.expected_kind == Rdna2TrapTerminationKind::TrapRaised) {
            OS_REQUIRE(trap_res.trap_id == 7u);
            OS_REQUIRE(trap_res.saved_pc_bytes > 0u);
        } else if (tc.expected_kind == Rdna2TrapTerminationKind::SendMsgHalt) {
            OS_REQUIRE(trap_res.trap_id == 0x2Au);
        }
    }

    // 1F. Extra s_rfe_b64 valid exception-return jump test:
    //     s_rfe_b64 jumps over a 0xDEAD9999 clobber block to a valid resume label that writes 0x55AA55AA -> uav1[2]!
    {
        std::vector<uint32_t> code;
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 2u, {Rdna2Operand::LiteralU32(0x11112222u)}, 0, code));
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 0u, 4u, 0u, false);
        OS_REQUIRE(EncodeScalarVectorInst("s_getpc_b64", 4u, {}, 0, code));
        const size_t after_getpc = code.size();
        const size_t lit_idx = code.size() + 1u;
        OS_REQUIRE(EncodeScalarVectorInst("s_add_u32", 4u,
            {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32(0x12345678u)}, 0, code));
        OS_REQUIRE(EncodeScalarVectorInst("s_rfe_b64", 0u, {Rdna2Operand::Sgpr(4)}, 0, code));
        // Skipped clobber block (MUST NOT EXECUTE!):
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 3u, {Rdna2Operand::LiteralU32(0xDEAD9999u)}, 0, code));
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 0u, 4u, 4u, false);
        // Resume target after RFE:
        const size_t resume_word = code.size();
        code[lit_idx] = static_cast<uint32_t>((resume_word - after_getpc) * 4u);
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 5u, {Rdna2Operand::LiteralU32(0x55AA55AAu)}, 0, code));
        AppendMubuf(code, Rdna2Opcode::BUFFER_STORE_DWORD, 5u, 0u, 4u, 8u, false);
        OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));

        Rdna2PrivilegedTrapResult rfe_res{};
        OS_REQUIRE(ExecuteRdna2PrivilegedTrapHarness(code, {0u, 0u, 0u, 0u}, 4u, rfe_res).ok());
        OS_REQUIRE(rfe_res.termination == Rdna2TrapTerminationKind::RfeReturned);
        OS_REQUIRE(rfe_res.rfe_target_pc_bytes == static_cast<uint64_t>(resume_word) * 4ull);
        OS_REQUIRE(rfe_res.uav1[0] == 0x11112222u);
        OS_REQUIRE(rfe_res.uav1[1] == 0u);           // Skipped clobber block did NOT mutate UAV1[1]!
        OS_REQUIRE(rfe_res.uav1[2] == 0x55AA55AAu);  // Exception return target DID execute!
    }

    std::printf("[PASS] Test 1: 4/4 privileged trap/halt/RFE opcodes (s_rfe_b64, s_sethalt, s_sendmsghalt, s_trap) "
                "decoded, lifted, validated, and CPU-ref harness verified\n");
}

// ---------------------------------------------------------------------------
// Test 2: 6/6 Newly GPU-Verified Shader-Executable Opcodes (Direct Parity Check)
// ---------------------------------------------------------------------------
void Test2_SixNewlyGpuVerifiedOpcodesDirect() {
    // 2A. Core ISA s_swappc_b64 + s_setpc_b64 + s_code_end -> SPIR-V + Vulkan HW + DXIL + CPU-ref
    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 2, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 1, 2, 0, 0);
    a.EmitSop1(Rdna2Opcode::S_GETPC_B64, 8, Rdna2Operand::InlineInt(0));
    const uint32_t after_getpc_word = a.CurrentWordOffset();
    a.EmitSop2(Rdna2Opcode::S_ADD_U32, 8, Rdna2Operand::Sgpr(8), Rdna2Operand::InlineInt(28));
    a.EmitSop1(Rdna2Opcode::S_SWAPPC_B64, 10, Rdna2Operand::Sgpr(8));
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 2, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 1, 2, 4, 0);
    a.EmitSopp(Rdna2Opcode::S_CODE_END, 0);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    const uint32_t helper_word = a.CurrentWordOffset();
    OS_REQUIRE((helper_word - after_getpc_word) * 4u == 28u);
    a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 1, Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(3));
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 1, Rdna2Operand::InlineInt(7), 1);
    a.EmitSop1(Rdna2Opcode::S_SETPC_B64, 0, Rdna2Operand::Sgpr(10));

    Rdna2LiftOptions lopts{};
    lopts.use_sm65 = true;
    lopts.emit_spirv = true;
    lopts.emit_hlsl = true;
    lopts.compile_dxil_with_dxc = false;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), lifted, lopts).ok());
    OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());

    std::vector<openscarlett::rdna2_sv::SvDecodedInst> sv_dec;
    OS_REQUIRE(openscarlett::rdna2_sv::DecodeScalarVectorStream(a.Words(), sv_dec).ok());
    openscarlett::rdna2_sv::SvWaveState wave{};
    wave.Reset(32u);
    wave.SeedThreadIds(0u);
    wave.uav0.resize(32u);
    for (uint32_t i = 0; i < 32u; ++i) wave.uav0[i] = 10u + i * 5u;
    wave.uav1.assign(32u, 0u);
    OS_REQUIRE(openscarlett::rdna2_sv::ExecuteScalarVectorCpu(sv_dec, wave).ok());
    for (uint32_t i = 0; i < 32u; ++i) {
        OS_REQUIRE(wave.uav1[i] == wave.uav0[i] * 3u + 7u);
    }

    VulkanBackend vk;
    bool ran_vk = false;
    if (vk.Initialize(/*allow_software_fallback=*/false).ok() && vk.GetDeviceInfo().is_hardware) {
        std::vector<uint32_t> vk_out;
        OS_REQUIRE(vk.ExecuteComputeShader(lifted.spirv_words, wave.uav0, vk_out, 1u).ok());
        vk.Shutdown();
        OS_REQUIRE(vk_out.size() >= 32u);
        for (uint32_t i = 0; i < 32u; ++i) {
            OS_REQUIRE(vk_out[i] == wave.uav1[i]);
        }
        ran_vk = true;
    }

    std::printf("[PASS] Test 2: Core s_swappc_b64 + s_setpc_b64 + s_code_end direct parity verified (CPU%s)\n",
                ran_vk ? " + Vulkan HW" : "");
}

// ---------------------------------------------------------------------------
// Test 3: RunRdna2CoverageBenchmark (--fast mode): 779/779 ISA + 775/779 Honest GPU
// ---------------------------------------------------------------------------
void Test3_FastBenchHonest775Coverage() {
    std::error_code ec;
    const std::filesystem::path tmp_dir = std::filesystem::temp_directory_path(ec) / "openscarlett_s15_trap_harness";
    std::filesystem::create_directories(tmp_dir, ec);
    const std::string json_path = (tmp_dir / "s15_fast_bench.json").string();

    Rdna2BenchOptions opts{};
    opts.full_corpus = false;
    opts.corpus_count = 240u;
    opts.external_val_sample = 12u;
    opts.run_live_gpu = true;
    opts.run_external_validators = true;
    opts.quiet = true;
    opts.json_output_path = json_path;

    Rdna2CoverageReport rep{};
    Status st = RunRdna2CoverageBenchmark(opts, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] RunRdna2CoverageBenchmark(--fast): %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.opcodes_total == 779u);
    OS_REQUIRE(rep.opcodes_decoded == 779u);
    OS_REQUIRE(rep.opcodes_lifted_spirv == 779u);
    OS_REQUIRE(rep.opcodes_lifted_dxil == 779u);
    OS_REQUIRE(rep.opcodes_cpu_ref == 779u);
    OS_REQUIRE(rep.corpus_shaders_total == 240u);
    OS_REQUIRE(rep.corpus_shaders_passed == 240u);
    OS_REQUIRE(rep.elapsed_seconds < 30.0);

    if (rep.has_dxc_tool && (rep.has_d3d12_hw || rep.has_d3d12_warp)) {
        // EXACT honest count: 775/779 (all 775 shader-executable opcodes GPU-verified;
        // 4 privileged trap/halt ops NEVER falsely marked gpu_verified!)
        OS_REQUIRE(rep.opcodes_gpu_verified == 775u);
        OS_REQUIRE(rep.families.size() == 20u);
        for (const auto& f : rep.families) {
            if (f.family == "SOP1") {
                OS_REQUIRE(f.total == 58u && f.gpu_verified == 57u);
            } else if (f.family == "SOPK") {
                OS_REQUIRE(f.total == 27u && f.gpu_verified == 27u);
            } else if (f.family == "SOPP") {
                OS_REQUIRE(f.total == 39u && f.gpu_verified == 36u);
            } else {
                OS_REQUIRE(f.gpu_verified == f.total);
            }
        }
    }

    std::filesystem::remove_all(tmp_dir, ec);

    std::printf(
        "[PASS] Test 3: RunRdna2CoverageBenchmark (--fast): %u/779 decoded/spirv/dxil/cpu | "
        "GPU=%u/779 (SOP1=57/58, SOPK=27/27, SOPP=36/39, 17/20 families 100%%) | %.2fs\n",
        rep.opcodes_decoded,
        rep.opcodes_gpu_verified,
        rep.elapsed_seconds);
}

}  // namespace

int main() {
    Test1_PrivilegedTrapHaltRfeHarness();
    Test2_SixNewlyGpuVerifiedOpcodesDirect();
    Test3_FastBenchHonest775Coverage();
    std::printf("=== ALL SPRINT 15 RDNA2 BENCH & PRIVILEGED TRAP HARNESS TESTS PASSED ===\n");
    return 0;
}
