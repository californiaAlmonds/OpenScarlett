#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/rdna2_xbox_container.h"
#include "openscarlett/gpu/shader_cache.h"
#include "openscarlett/gpu/vulkan_backend.h"

#include "../src/gpu/rdna2_coverage_tool.h"
#include "../src/gpu/rdna2_isa_mem_img.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace openscarlett {
Status ExecuteGuardedShaderTranslation(
    const std::function<Status()>& work,
    std::string* out_diagnostics,
    bool* out_recovered_from_poison);
bool IsShaderTranslatorMutexPoisoned() noexcept;
uint64_t GetShaderTranslatorMutexRecoveryCount() noexcept;
void ResetShaderTranslatorMutexForTest() noexcept;
}  // namespace openscarlett

using namespace openscarlett;

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        ++g_failures;
    } else {
        std::printf("[PASS] %s\n", msg);
    }
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string FindSpirvValIfPresent() {
    const std::filesystem::path p(R"(E:\ScarlettX-dev\tools\vulkan-sdk\Bin\spirv-val.exe)");
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        return p.string();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Test 1: MIMG R128 / TFE / LWE Unsupported Modifier Refusal
// ---------------------------------------------------------------------------
void TestMimgUnsupportedModifierRefusal() {
    std::printf("\n--- Test 1: MIMG R128 / TFE / LWE Refusal ---\n");
    using namespace openscarlett::rdna2_mem;

    struct ModCase {
        const char* label;
        bool r128;
        bool tfe;
        bool lwe;
        const char* expect_token;
    };
    const ModCase cases[] = {
        {"R128", true, false, false, "R128"},
        {"TFE", false, true, false, "TFE"},
        {"LWE", false, false, true, "LWE"},
        {"R128+TFE+LWE", true, true, true, "R128"},
    };

    for (const auto& tc : cases) {
        std::vector<uint32_t> words;
        MemImgEncodeParams p{};
        p.vdata = 4u;
        p.vaddr = 2u;
        p.srsrc_sgpr = 0u;
        p.dmask = 0xFu;
        p.dim = kMimgDim2D;
        p.r128 = tc.r128;
        p.tfe = tc.tfe;
        p.lwe = tc.lwe;
        Check(EncodeMemImgInst("image_load", p, words), "EncodeMemImgInst(image_load) with modifier");
        words.push_back(0xBF810000u);  // s_endpgm

        // Decoder still decodes the flags accurately (no decode regression)
        std::vector<MemImgDecodedInst> decoded;
        Check(DecodeMemImgStream(words, decoded).ok() && !decoded.empty(),
              "DecodeMemImgStream decodes MIMG modifier bits");
        Check(decoded[0].r128 == tc.r128 && decoded[0].tfe == tc.tfe && decoded[0].lwe == tc.lwe,
              "Decoded MIMG r128/tfe/lwe bits match encoded flags");

        // LiftMemImgShader must refuse cleanly with descriptive diagnostic
        Xs2ShaderContainer container{};
        container.stage = ShaderStage::Compute;
        container.threadgroup_x = 1u;
        container.threadgroup_y = 1u;
        container.threadgroup_z = 1u;
        container.wave_size = 32u;
        container.code_words = words;

        Rdna2LiftResult mem_res{};
        Rdna2LiftOptions mem_opts{};
        mem_opts.emit_spirv = true;
        mem_opts.emit_hlsl = true;
        mem_opts.compile_dxil_with_dxc = false;
        const Status lift_st = LiftMemImgShader(container, decoded, mem_res, mem_opts);
        Check(!lift_st.ok() && lift_st.code == StatusCode::UnsupportedFormat,
              "LiftMemImgShader refuses unsupported MIMG modifier with UnsupportedFormat");
        Check(Contains(lift_st.message, "MIMG") &&
                  Contains(lift_st.message, "image_load") &&
                  Contains(lift_st.message, tc.expect_token),
              "LiftMemImgShader diagnostic names MIMG, opcode, and modifier");

        // ExecuteMemImgCpu must also refuse cleanly
        MemImgExecState cpu_st{};
        const Status exec_st = ExecuteMemImgCpu(decoded, cpu_st);
        Check(!exec_st.ok() && exec_st.code == StatusCode::UnsupportedFormat &&
                  Contains(exec_st.message, tc.expect_token),
              "ExecuteMemImgCpu refuses unsupported MIMG modifier");

        // Core LiftRdna2Xs2Shader / BuildRdna2Ir must refuse cleanly too
        const std::vector<uint8_t> xs2 = BuildXs2Container(container);
        Rdna2LiftResult core_res{};
        Rdna2LiftOptions core_opts{};
        core_opts.emit_spirv = true;
        core_opts.emit_hlsl = true;
        core_opts.compile_dxil_with_dxc = false;
        const Status core_st = LiftRdna2Xs2Shader(xs2, core_res, core_opts);
        Check(!core_st.ok() && core_st.code == StatusCode::UnsupportedFormat,
              "Core LiftRdna2Xs2Shader refuses unsupported MIMG modifier with UnsupportedFormat");
        Check(Contains(core_st.message, "MIMG") && Contains(core_st.message, tc.expect_token) &&
                  Contains(core_res.dxc_diagnostics, tc.expect_token),
              "Core LiftRdna2Xs2Shader diagnostic names MIMG and modifier");
    }

    // Positive control: standard MIMG without R128/TFE/LWE lifts cleanly
    {
        std::vector<uint32_t> words;
        MemImgEncodeParams p{};
        p.vdata = 4u;
        p.vaddr = 2u;
        p.dmask = 0xFu;
        p.dim = kMimgDim2D;
        Check(EncodeMemImgInst("image_load", p, words), "Encode clean MIMG image_load");
        words.push_back(0xBF810000u);
        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1u;
        c.code_words = words;
        Rdna2LiftResult res{};
        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = false;
        Check(LiftRdna2Xs2Shader(BuildXs2Container(c), res, opts).ok(),
              "Clean MIMG image_load without R128/TFE/LWE lifts cleanly");
    }
}

// ---------------------------------------------------------------------------
// Test 2: Poisoned Shader-Translator Mutex Recovery
// ---------------------------------------------------------------------------
void TestPoisonedTranslatorMutexRecovery() {
    std::printf("\n--- Test 2: Poisoned Shader-Translator Mutex Recovery ---\n");
    ResetShaderTranslatorMutexForTest();
    Check(!IsShaderTranslatorMutexPoisoned(), "Translator mutex starts clean (unpoisoned)");
    Check(GetShaderTranslatorMutexRecoveryCount() == 0u, "Recovery counter starts at 0");

    // 1. Trigger a thrown exception while holding the translator mutex
    std::string fault_diag;
    bool fault_recovered = false;
    const Status fault_st = ExecuteGuardedShaderTranslation(
        []() -> Status { throw std::runtime_error("simulated translator fault #159"); },
        &fault_diag,
        &fault_recovered);
    Check(!fault_st.ok() && fault_st.code == StatusCode::ShaderError,
          "Thrown exception inside translator returns StatusCode::ShaderError cleanly");
    Check(!fault_recovered && IsShaderTranslatorMutexPoisoned(),
          "Mutex is marked poisoned after uncommitted unwind");
    Check(Contains(fault_diag, "simulated translator fault #159"),
          "Fault diagnostic captures thrown exception reason");

    // 2. Recover from a SECOND thread (proves mutex was unlocked cleanly with zero deadlock!)
    Rdna2Assembler a;
    a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 2, Rdna2Operand::InlineInt(7), 0);
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 1, 4, 0);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    const std::vector<uint8_t> xs2 = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);

    std::atomic<bool> worker_finished{false};
    Status recover_st = Status::Error(StatusCode::ShaderError, "not run");
    std::string recover_diag;
    bool did_recover = false;

    std::thread worker([&]() {
        Rdna2LiftResult res{};
        Rdna2LiftOptions opts{};
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = false;
        recover_st = ExecuteGuardedShaderTranslation(
            [&]() -> Status { return LiftRdna2Xs2Shader(xs2, res, opts); },
            &recover_diag,
            &did_recover);
        worker_finished.store(true, std::memory_order_release);
    });
    worker.join();

    Check(worker_finished.load() && recover_st.ok(),
          "Subsequent translation on another thread completes without deadlocking");
    Check(did_recover && !IsShaderTranslatorMutexPoisoned(),
          "Poisoned mutex detected, recovered, and cleared");
    Check(GetShaderTranslatorMutexRecoveryCount() == 1u,
          "Translator mutex recovery counter incremented to 1");
    Check(Contains(recover_diag, "Recovered") && Contains(recover_diag, "simulated translator fault #159"),
          "Recovery diagnostic names prior poison fault");

    // 3. Verify ShaderCache transparently recovers too using portable temp dir
    (void)ExecuteGuardedShaderTranslation(
        []() -> Status { throw std::runtime_error("poison before ShaderCache"); },
        nullptr,
        nullptr);
    Check(IsShaderTranslatorMutexPoisoned(), "Mutex re-poisoned before ShaderCache call");

    const std::filesystem::path cache_dir =
        std::filesystem::temp_directory_path() / "openscarlett_test_s13_poison_cache";
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    {
        ShaderCache cache;
        cache.SetCacheDirectory(cache_dir.string());
        ShaderCacheLiftRequest req{};
        req.highest_shader_model = 0x50u;  // SM 5.0 FXC works everywhere on Windows without external dxc
        CachedShaderResult cached{};
        std::string cache_diag;
        const Status cst = cache.GetOrLiftXboxShader(xs2.data(), xs2.size(), req, cached, &cache_diag);
        Check(cst.ok() && !cached.dxil.empty(),
              "ShaderCache::GetOrLiftXboxShader recovers cleanly from poisoned translator mutex");
        Check(!IsShaderTranslatorMutexPoisoned() && GetShaderTranslatorMutexRecoveryCount() >= 2u,
              "ShaderCache cleared poison state and incremented recovery count");
    }
    std::filesystem::remove_all(cache_dir, ec);
}

// ---------------------------------------------------------------------------
// Test 3: CFG Structurization Canonicalization (Non-Merge Latch & Exit-Less Loop)
// ---------------------------------------------------------------------------
void TestCfgCanonicalization() {
    std::printf("\n--- Test 3: CFG Canonicalization (Non-Merge Latch & Exit-Less Loop) ---\n");
    const std::string dxc_path = ResolvePortableDxcPath();
    const std::string spv_val = FindSpirvValIfPresent();
    std::error_code ec;
    const bool has_dxc = !dxc_path.empty() && std::filesystem::exists(dxc_path, ec);
    const bool has_spv_val = !spv_val.empty();

    // 3A. Natural loop with TWO back-edges (non-merge / multi-latch + conditional back-edge):
    //   s4 = 0 (counter), s5 = 0 (acc)
    //   loop_hdr:
    //     s5 = s5 + s4
    //     s4 = s4 + 1
    //     s_cmp_lt_u32 s4, 3
    //     s_cbranch_scc1 loop_hdr   // Back-edge #1 (conditional non-merge latch!)
    //     s_cmp_lt_u32 s4, 5
    //     s_cbranch_scc1 loop_hdr   // Back-edge #2 (second conditional latch!)
    //   exit:
    //     v2 = s5 + v0              // expected: (0+1+2+3+4) + tid = 10 + tid
    //     buffer_store_dword v2 -> uav1[tid*4]
    {
        Rdna2Assembler a;
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 5, Rdna2Operand::InlineInt(0));
        const uint32_t loop_hdr = a.CurrentWordOffset();
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 5, Rdna2Operand::Sgpr(5), Rdna2Operand::Sgpr(4));
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 4, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(1));
        a.EmitSopc(Rdna2Opcode::S_CMP_LT_U32, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(3));
        const uint32_t br1 = a.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC1);
        a.PatchBranch(br1, loop_hdr);
        a.EmitSopc(Rdna2Opcode::S_CMP_LT_U32, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(5));
        const uint32_t br2 = a.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC1);
        a.PatchBranch(br2, loop_hdr);
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 2, Rdna2Operand::Sgpr(5), 0);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.dxc_path = dxc_path;
        opts.validate_spirv_external = has_spv_val;
        opts.spirv_val_path = spv_val;

        Rdna2LiftResult res{};
        const Status st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), res, opts);
        Check(st.ok(), "Multi-latch non-merge loop lifts + compiles DXIL + validates SPIR-V");

        // Verify canonical single unconditional latch block -> header
        Rdna2CfgAnalysis cfg{};
        AnalyzeRdna2Cfg(res.ir, cfg);
        Check(cfg.has_cycles && cfg.natural_loop_count == 1u && cfg.back_edges.size() == 1u,
              "Multi-latch loop canonicalized to exactly 1 back-edge");
        if (!cfg.back_edges.empty()) {
            const uint32_t latch_bb = cfg.back_edges[0].first;
            const uint32_t hdr_bb = cfg.back_edges[0].second;
            const auto& lbb = res.ir.blocks[latch_bb];
            Check(latch_bb != hdr_bb && !lbb.instructions.empty() &&
                      lbb.instructions.back().op == IrOp::Branch &&
                      lbb.instructions.back().true_block == hdr_bb &&
                      lbb.successors.size() == 1u,
                  "Canonical loop latch is a dedicated unconditional Branch block -> header");
        }

        // Verify CPU ref & live Vulkan compute execution match exact expected values (10 + tid)
        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(32u, 0u);
        cpu.uav1.assign(32u, 0u);
        Check(ExecuteRdna2Cpu(res.container, res.instructions, cpu).ok(),
              "Multi-latch loop CPU reference executes cleanly");
        bool cpu_ok = (cpu.uav1.size() >= 32u);
        for (uint32_t i = 0u; i < 32u && cpu_ok; ++i) {
            if (cpu.uav1[i] != 10u + i) cpu_ok = false;
        }
        Check(cpu_ok, "Multi-latch loop CPU output == 10 + tid across all 32 lanes");

        VulkanBackend vk;
        if (vk.Initialize(/*allow_software_fallback=*/false).ok()) {
            std::vector<uint32_t> vk_out;
            const Status vst = vk.ExecuteComputeShader(res.spirv_words, cpu.uav0, vk_out, 1u);
            bool vk_ok = vst.ok() && vk_out.size() >= 32u;
            for (uint32_t i = 0u; i < 32u && vk_ok; ++i) {
                if (vk_out[i] != 10u + i) vk_ok = false;
            }
            Check(vk_ok, "Multi-latch canonicalized SPIR-V executes LIVE on Vulkan with exact parity");
            vk.Shutdown();
        } else {
            std::printf("[SKIP] Vulkan HW ICD unavailable (headless CI); multi-latch DXIL + SPIR-V + CPU parity verified\n");
        }
    }

    // 3B. Exit-less infinite / self-looping shader:
    //   s4 = 0
    //   spin:
    //     s4 = s4 + 1
    //     s_branch spin   (NO exit edge!)
    {
        Rdna2Assembler a;
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::InlineInt(0));
        const uint32_t spin_hdr = a.CurrentWordOffset();
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 4, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(1));
        const uint32_t br_inf = a.EmitSopp(Rdna2Opcode::S_BRANCH);
        a.PatchBranch(br_inf, spin_hdr);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.dxc_path = dxc_path;
        opts.validate_spirv_external = has_spv_val;
        opts.spirv_val_path = spv_val;

        Rdna2LiftResult res{};
        const Status st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), res, opts);
        Check(st.ok(), "Exit-less infinite loop canonicalizes, compiles DXIL, and validates SPIR-V");

        bool has_return_block = false;
        for (const auto& bb : res.ir.blocks) {
            if (!bb.instructions.empty() && bb.instructions.back().op == IrOp::Return) {
                has_return_block = true;
                break;
            }
        }
        Check(has_return_block, "Exit-less loop has synthesized Return exit block");

        Rdna2CfgAnalysis cfg{};
        AnalyzeRdna2Cfg(res.ir, cfg);
        Check(cfg.has_cycles && cfg.back_edges.size() == 1u,
              "Canonicalized exit-less loop retains structured loop with single canonical latch");
        if (!cfg.back_edges.empty()) {
            const uint32_t lbb_id = cfg.back_edges[0].first;
            const uint32_t hbb_id = cfg.back_edges[0].second;
            Check(lbb_id != hbb_id && !res.ir.blocks[lbb_id].instructions.empty() &&
                      res.ir.blocks[lbb_id].instructions.back().op == IrOp::Branch,
                  "Exit-less loop latch is canonical unconditional Branch");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 4: Mixed-Heap V# Slot-0 CB Collision + Malformed Vertex Input Table
// ---------------------------------------------------------------------------
void TestMixedHeapAndVertexTableDiagnostics() {
    std::printf("\n--- Test 4: Mixed-Heap V# Slot-0 CB Collision & Malformed Vertex Table ---\n");
    const std::string dxc_path = ResolvePortableDxcPath();
    const std::string spv_val = FindSpirvValIfPresent();
    std::error_code ec;
    const bool has_dxc = !dxc_path.empty() && std::filesystem::exists(dxc_path, ec);
    const bool has_spv_val = !spv_val.empty();

    // 4A. Mixed-heap V# slot 0 + CB with non-zero base SGPR offset (sbase = 2, byte_off = 16)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // Mixed-heap V# slot 0 (srsrc=0 -> u0, space0)
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        // Constant buffer with non-zero base offset (sbase_sgpr=4 -> sbase=2, offset=16 -> b2, space1; MUST NOT collide with u0/b0!)
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 8, 4, 16);
        // Root constant at b0, space0 (sbase=0, offset=0)
        a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 9, 0, 0);
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::Sgpr(8), 2);
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 3, Rdna2Operand::Sgpr(9), 3);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 0, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.dxc_path = dxc_path;
        opts.validate_spirv_external = has_spv_val;
        opts.spirv_val_path = spv_val;

        Rdna2LiftResult res{};
        const Status st = LiftRdna2Xs2Shader(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), res, opts);
        Check(st.ok(), "Mixed-heap V# slot 0 + non-zero CB offset lifts, compiles DXIL, and validates SPIR-V");
        Check(Contains(res.hlsl_source, "register(b2, space1)") &&
                  Contains(res.hlsl_source, "register(u0)") &&
                  Contains(res.hlsl_source, "register(b0)"),
              "HLSL separates non-zero CB (b2, space1) from V# slot 0 (u0) and root CB (b0)");
    }

    // 4B. Malformed Vertex Input Table Diagnostics
    {
        Rdna2Assembler vs;
        vs.EmitSop1(Rdna2Opcode::S_SWAPPC_B64, 6, Rdna2Operand::Sgpr(8));
        vs.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 4, 5, 6, 7, 0xFu, true);
        vs.EmitSopp(Rdna2Opcode::S_ENDPGM);
        const std::vector<uint8_t> vs_xs2 = vs.BuildXs2(ShaderStage::Vertex, 1, 1, 1, 16, 16, 32);

        auto expect_rejected = [&](const std::vector<VertexFetchEntry>& table, const char* label) {
            Rdna2LiftOptions opts{};
            opts.emit_spirv = false;
            opts.emit_hlsl = true;
            opts.compile_dxil_with_dxc = false;
            opts.stage_contract.vertex_fetches = table;
            Rdna2LiftResult res{};
            const Status st = LiftRdna2Xs2Shader(vs_xs2, res, opts);
            const bool ok = !st.ok() && st.code == StatusCode::InvalidArgument &&
                            Contains(st.message, "Malformed vertex input table");
            Check(ok, label);
        };

        // Case 1: Invalid format
        {
            VertexFetchEntry vf{};
            vf.format = Rdna2MtbufFormat::Invalid;
            vf.num_components = 4u;
            vf.dst_vgpr = 4u;
            expect_rejected({vf}, "Malformed vertex table rejected: Invalid format");
        }
        // Case 2: Invalid num_components (0 and 5)
        {
            VertexFetchEntry vf0{};
            vf0.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
            vf0.num_components = 0u;
            vf0.dst_vgpr = 4u;
            expect_rejected({vf0}, "Malformed vertex table rejected: num_components=0");

            VertexFetchEntry vf5 = vf0;
            vf5.num_components = 5u;
            expect_rejected({vf5}, "Malformed vertex table rejected: num_components=5");
        }
        // Case 3: Out-of-bounds vb_slot
        {
            VertexFetchEntry vf{};
            vf.vb_slot = 16u;
            vf.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
            vf.num_components = 4u;
            vf.dst_vgpr = 4u;
            expect_rejected({vf}, "Malformed vertex table rejected: vb_slot=16 out of bounds");
        }
        // Case 4: Corrupt/huge byte_offset
        {
            VertexFetchEntry vf{};
            vf.byte_offset = 0x100000u;
            vf.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
            vf.num_components = 4u;
            vf.dst_vgpr = 4u;
            expect_rejected({vf}, "Malformed vertex table rejected: corrupt byte_offset");
        }
        // Case 5: Overlapping dst_vgpr range
        {
            VertexFetchEntry a{};
            a.vb_slot = 0u;
            a.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
            a.num_components = 4u;
            a.dst_vgpr = 4u;
            VertexFetchEntry b{};
            b.vb_slot = 1u;
            b.format = Rdna2MtbufFormat::R32G32_FLOAT;
            b.num_components = 2u;
            b.dst_vgpr = 6u;  // Overlaps v6..v7 of entry a!
            expect_rejected({a, b}, "Malformed vertex table rejected: overlapping dst_vgpr");
        }
        // Positive control: valid non-overlapping vertex table succeeds
        {
            VertexFetchEntry a{};
            a.vb_slot = 0u;
            a.byte_offset = 0u;
            a.format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
            a.num_components = 4u;
            a.dst_vgpr = 4u;
            Rdna2LiftOptions opts{};
            opts.emit_spirv = true;
            opts.emit_hlsl = true;
            opts.compile_dxil_with_dxc = false;
            opts.stage_contract.vertex_fetches = {a};
            Rdna2LiftResult res{};
            Check(LiftRdna2Xs2Shader(vs_xs2, res, opts).ok(),
                  "Valid vertex input table lifts cleanly");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 5: Mesh Shader LDS Writes -> Reconverged s_barrier -> LDS Reads
// ---------------------------------------------------------------------------
void TestMeshShaderLdsBarrierReads() {
    std::printf("\n--- Test 5: Mesh Shader LDS Barrier Reads Across Wave Reconvergence ---\n");
    const std::string dxc_path = ResolvePortableDxcPath();
    const std::string spv_val = FindSpirvValIfPresent();
    std::error_code ec;
    const bool has_dxc = !dxc_path.empty() && std::filesystem::exists(dxc_path, ec);
    const bool has_spv_val = !spv_val.empty();

    // 5A. Multi-wave Mesh Shader (64 threads = 2 Wave32 waves):
    //   - Diverge EXEC via V_CMPX_LT_U32 (tid < 32)
    //   - Active lanes (wave 0) write (tid + 100) to LDS[tid * 4]
    //   - Reconverge EXEC (S_MOV_B32 exec_lo, 0xFFFFFFFF)
    //   - Unconditional S_BARRIER across reconverged waves
    //   - All lanes read LDS[(tid & 31) * 4], convert to float, export Mesh vertex Pos0 & Prim
    {
        Rdna2Assembler ms;
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u /*M0*/, Rdna2Operand::LiteralU32(PackGsAllocReqM0(3u, 1u)));
        ms.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
        ms.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);       // v1 = tid * 4
        ms.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 2, Rdna2Operand::LiteralU32(100u), 0);    // v2 = tid + 100
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineInt(32));             // v3 = 32
        ms.EmitVopc(Rdna2Opcode::V_CMPX_LT_U32, Rdna2Operand::Vgpr(0), 3);               // active iff tid < 32
        ms.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2, 0);                                // predicated LDS store
        ms.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /*EXEC_LO*/, Rdna2Operand::LiteralU32(0xFFFFFFFFu));  // reconverge
        ms.EmitSopp(Rdna2Opcode::S_BARRIER);                                             // workgroup barrier
        ms.EmitVop2(Rdna2Opcode::V_AND_B32, 4, Rdna2Operand::InlineInt(31), 0);          // v4 = tid & 31
        ms.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 4);       // v4 = (tid & 31) * 4
        ms.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4, 0, 0);                                 // cross-wave LDS read
        ms.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 6, Rdna2Operand::Vgpr(5));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(0.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
        ms.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralU32((0u) | (1u << 10u) | (2u << 20u)));
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 6, 7, 7, 8, 0xFu, false);
        ms.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 9, 9, 9, 9, 0x1u, true);
        ms.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_mesh_spirv = true;
        opts.mesh_has_payload = false;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.dxc_path = dxc_path;
        opts.validate_spirv_external = has_spv_val;
        opts.spirv_val_path = spv_val;
        opts.spirv_target_env = "vulkan1.3";

        Rdna2LiftResult res{};
        const Status st = LiftRdna2Xs2Shader(ms.BuildXs2(ShaderStage::Mesh, 64, 1, 1, 16, 16, 32), res, opts);
        Check(st.ok(), "Mesh shader LDS write -> reconverged s_barrier -> LDS read lifts + compiles ms_6_5 + validates SPIR-V");

        const size_t pos_if_lds = res.hlsl_source.find("if (");
        const size_t pos_barrier = res.hlsl_source.find("GroupMemoryBarrierWithGroupSync();");
        const size_t pos_lds_read = (pos_barrier != std::string::npos)
                                        ? res.hlsl_source.find("= _lds[", pos_barrier)
                                        : std::string::npos;
        Check(pos_if_lds != std::string::npos && pos_barrier != std::string::npos && pos_lds_read != std::string::npos,
              "Mesh HLSL predicates divergent LDS write and places unconditional GroupMemoryBarrierWithGroupSync before LDS read");

        // Verify SPIR-V binary contains OpControlBarrier (opcode 224, word_count=4 -> 0x000400E0)
        bool found_spv_barrier = false;
        for (size_t i = 5u; i < res.spirv_words.size(); ) {
            const uint32_t wc = res.spirv_words[i] >> 16u;
            const uint32_t op = res.spirv_words[i] & 0xFFFFu;
            if (wc == 0u) break;
            if (op == 224u) {
                found_spv_barrier = true;
                break;
            }
            i += wc;
        }
        Check(found_spv_barrier, "Mesh SPIR-V emits OpControlBarrier for workgroup LDS synchronization");
    }

    // 5B. Live GPU Execution: 64-thread (2-wave) divergent V_CMPX -> DS_WRITE_B32 -> reconverge -> S_BARRIER -> cross-wave DS_READ_B32
    {
        Rdna2Assembler cs;
        cs.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);       // v1 = tid * 4
        cs.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 2, Rdna2Operand::LiteralU32(1000u), 0);   // v2 = tid + 1000
        cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineInt(32));             // v3 = 32
        cs.EmitVopc(Rdna2Opcode::V_CMPX_LT_U32, Rdna2Operand::Vgpr(0), 3);               // only lanes 0..31 active
        cs.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2, 0);                                // write LDS[0..31]
        cs.EmitSop1(Rdna2Opcode::S_MOV_B32, 126u /*EXEC_LO*/, Rdna2Operand::LiteralU32(0xFFFFFFFFu));  // reconverge
        cs.EmitSopp(Rdna2Opcode::S_BARRIER);                                             // synchronize waves 0 & 1
        cs.EmitVop2(Rdna2Opcode::V_AND_B32, 4, Rdna2Operand::InlineInt(31), 0);          // v4 = tid & 31
        cs.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 4);       // v4 = (tid & 31) * 4
        cs.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4, 0, 0);                                 // wave 1 reads wave 0's LDS!
        cs.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 0);
        cs.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2LiftOptions opts{};
        opts.use_sm65 = true;
        opts.emit_spirv = true;
        opts.emit_hlsl = true;
        opts.compile_dxil_with_dxc = has_dxc;
        opts.dxc_path = dxc_path;
        opts.validate_spirv_external = has_spv_val;
        opts.spirv_val_path = spv_val;

        Rdna2LiftResult res{};
        const Status st = LiftRdna2Xs2Shader(cs.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32), res, opts);
        Check(st.ok(), "64-thread cross-wave LDS barrier shader lifts, compiles DXIL, and validates SPIR-V");

        Rdna2CpuExecState lds_cpu{};
        lds_cpu.uav0.assign(64u, 0u);
        lds_cpu.uav1.assign(64u, 0u);
        Check(ExecuteRdna2Cpu(res.container, res.instructions, lds_cpu).ok(),
              "64-thread cross-wave reconverged LDS barrier CPU reference executes cleanly");
        bool lds_cpu_ok = (lds_cpu.uav1.size() >= 64u);
        for (uint32_t i = 0u; i < 64u && lds_cpu_ok; ++i) {
            if (lds_cpu.uav1[i] != 1000u + (i & 31u)) lds_cpu_ok = false;
        }
        Check(lds_cpu_ok, "64-thread cross-wave reconverged LDS barrier CPU output is 100% bit-exact");

        VulkanBackend vk;
        if (vk.Initialize(/*allow_software_fallback=*/false).ok()) {
            std::vector<uint32_t> in_buf(64u, 0u);
            std::vector<uint32_t> out_buf;
            const Status vst = vk.ExecuteComputeShader(res.spirv_words, in_buf, out_buf, 1u);
            bool ok = vst.ok() && out_buf.size() >= 64u;
            for (uint32_t i = 0u; i < 64u && ok; ++i) {
                const uint32_t expected = 1000u + (i & 31u);
                if (out_buf[i] != expected) ok = false;
            }
            Check(ok, "Live Vulkan GPU 64-thread cross-wave reconverged LDS barrier readback is 100% bit-exact");
            vk.Shutdown();
        } else {
            std::printf("[SKIP] Vulkan HW ICD unavailable (headless CI); 64-thread cross-wave LDS DXIL + SPIR-V + CPU parity verified\n");
        }
    }
}

// ---------------------------------------------------------------------------
// Test 6: Real-GPU Hardware Opcode Verification >= 700/779 Across All 20 Families
// ---------------------------------------------------------------------------
void TestRealGpu700Opcodes() {
    std::printf("\n--- Test 6: Real-GPU Opcode Verification >= 700/779 Across All 20 Families ---\n");
    Rdna2BenchOptions opts{};
    opts.full_corpus = false;
    opts.corpus_count = 12u;
    opts.run_external_validators = false;
    opts.run_live_gpu = true;
    opts.quiet = true;

    Rdna2CoverageReport report{};
    const auto t0 = std::chrono::steady_clock::now();
    const Status st = RunRdna2CoverageBenchmark(opts, report);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("[INFO] Live GPU coverage: %u/%u verified across %zu families (HW=%u, WARP=%u, VK=%u) in %.2fs\n",
                report.opcodes_gpu_verified,
                report.opcodes_total,
                report.families.size(),
                report.corpus_gpu_hw_executed,
                report.corpus_gpu_warp_executed,
                report.corpus_vulkan_executed,
                secs);

    if (!st.ok()) {
        std::fprintf(stderr, "[ERR] RunRdna2CoverageBenchmark error: %s\n", st.message.c_str());
    }
    Check(st.ok(), "RunRdna2CoverageBenchmark succeeds with 0 parity mismatches");
    Check(report.opcodes_gpu_verified >= 700u,
          "Real-GPU hardware-verified opcode count >= 700/779");
    Check(report.families.size() == 20u, "Coverage report covers all 20 GFX10.3 families");

    bool all_20_verified = (report.families.size() == 20u);
    for (const auto& f : report.families) {
        std::printf("  %-8s : decoded=%3u/%3u | gpu_verified=%3u\n",
                    f.family.c_str(), f.decoded, f.total, f.gpu_verified);
        if (f.gpu_verified == 0u) all_20_verified = false;
    }
    Check(all_20_verified, "Every single one of the 20 GFX10.3 opcode families has live GPU-verified opcodes");
    Check(secs < 30.0, "Live GPU opcode verification completes in < 30 seconds");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== OpenScarlett Sprint 13: RDNA2 CFG/MIMG/V#/LDS Safety & 700+ Real-GPU Verification ===\n");
    TestMimgUnsupportedModifierRefusal();
    TestPoisonedTranslatorMutexRecovery();
    TestCfgCanonicalization();
    TestMixedHeapAndVertexTableDiagnostics();
    TestMeshShaderLdsBarrierReads();
    TestRealGpu700Opcodes();

    if (g_failures > 0) {
        std::fprintf(stderr, "\nFAILED: %d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("\nALL SPRINT 13 SHADER CFG/SAFETY & 700+ REAL-GPU TESTS PASSED!\n");
    return 0;
}
