// OpenScarlett - MIT License
// Sprint 11 Verification: 4 RDNA2/GFX10.3 Hardware Accuracy Rules + Expanded Real-GPU Opcode Suite.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"
#include "rdna2_coverage_tool.h"
#include "rdna2_isa_mem_img.h"
#include "rdna2_isa_scalar_vector.h"

namespace {

using namespace openscarlett;

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

float HalfToF32(uint16_t h) noexcept {
    const uint32_t s = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t e = (h >> 10u) & 0x1Fu;
    const uint32_t m = h & 0x3FFu;
    uint32_t bits = 0u;
    if (e == 0u) {
        if (m == 0u) {
            bits = s;
        } else {
            uint32_t shift = 0u;
            uint32_t mm = m;
            while ((mm & 0x400u) == 0u) {
                mm <<= 1u;
                ++shift;
            }
            bits = s | ((127u - 14u - shift) << 23u) | ((mm & 0x3FFu) << 13u);
        }
    } else if (e == 31u) {
        bits = s | 0x7F800000u | (m << 13u);
    } else {
        bits = s | ((e + 112u) << 23u) | (m << 13u);
    }
    return BitsToFloat(bits);
}

void AppendMubufLoadStore(std::vector<uint32_t>& words,
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

void RunOptionalVulkanAndValidators(const Rdna2LiftResult& lifted,
                                    const std::vector<uint32_t>& uav0,
                                    std::vector<uint32_t>& out_vk,
                                    bool& ran_vk) {
    ran_vk = false;
    std::string diag;
    const std::string spv_val = kDefaultSpirvValPath;
    if (!lifted.spirv_words.empty()) {
        OS_REQUIRE(ValidateSpirvBinary(lifted.spirv_words).ok());
    }
    const std::string dxc_path = ResolvePortableDxcPath();
    if (!dxc_path.empty() && !lifted.hlsl_source.empty()) {
        std::vector<uint8_t> dxil;
        Status st = CompileHlslWithDxc(lifted.hlsl_source, lifted.hlsl_target, "main", dxil, diag, dxc_path);
        if (!st.ok()) {
            std::fprintf(stderr, "[FAIL] DXC compile failed: %s\n%s\n", st.message.c_str(), diag.c_str());
        }
        OS_REQUIRE(st.ok());
        OS_REQUIRE(!dxil.empty());
    }
    if (!spv_val.empty() && !lifted.spirv_words.empty()) {
        // Validate external SPIR-V if spirv-val.exe is present on host
        Status sv_st = ValidateSpirvExternal(lifted.spirv_words, diag, spv_val, "vulkan1.2");
        if (sv_st.code != StatusCode::NotFound) {
            if (!sv_st.ok()) {
                std::fprintf(stderr, "[FAIL] spirv-val failed: %s\n%s\n", sv_st.message.c_str(), diag.c_str());
            }
            OS_REQUIRE(sv_st.ok());
        }
    }

    VulkanBackend vk;
    if (!lifted.spirv_words.empty() && vk.Initialize(false).ok() && vk.GetDeviceInfo().is_hardware) {
        Status vk_st = vk.ExecuteComputeShader(lifted.spirv_words, uav0, out_vk, 1u);
        vk.Shutdown();
        if (!vk_st.ok()) {
            std::fprintf(stderr, "[FAIL] Vulkan execute failed: %s\n", vk_st.message.c_str());
        }
        OS_REQUIRE(vk_st.ok());
        ran_vk = true;
    }
}

// ---------------------------------------------------------------------------
// Test 1: Rule (a) — v_sin_f32 / v_cos_f32 (and f16) GFX10.3 turns reduction
// ---------------------------------------------------------------------------
void Test1_TrigTurnsAccuracy() {
    using namespace openscarlett::rdna2_sv;

    std::vector<uint32_t> code;
    // v[1] = v[0] << 2
    OS_REQUIRE(EncodeScalarVectorInst("v_lshlrev_b32", 1u, {Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(0)}, 0, code));
    // v[2] = uav0[lane] (f32 turns)
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 1u, 0u, 0u, true);
    // v[3] = v_sin_f32(v[2])
    OS_REQUIRE(EncodeScalarVectorInst("v_sin_f32", 3u, {Rdna2Operand::Vgpr(2)}, 0, code));
    // v[4] = v_cos_f32(v[2])
    OS_REQUIRE(EncodeScalarVectorInst("v_cos_f32", 4u, {Rdna2Operand::Vgpr(2)}, 0, code));
    // v[5] = v_cvt_f16_f32(v[2]); v[6] = v_sin_f16(v[5]); v[7] = v_cos_f16(v[5])
    OS_REQUIRE(EncodeScalarVectorInst("v_cvt_f16_f32", 5u, {Rdna2Operand::Vgpr(2)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("v_sin_f16", 6u, {Rdna2Operand::Vgpr(5)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("v_cos_f16", 7u, {Rdna2Operand::Vgpr(5)}, 0, code));
    // Store v[3], v[4], v[6], v[7] to uav1
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 3u, 1u, 4u, 0u, true);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 4u, 1u, 4u, 128u, true);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 6u, 1u, 4u, 256u, true);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 7u, 1u, 4u, 384u, true);
    OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));

    std::vector<SvDecodedInst> decoded;
    OS_REQUIRE(DecodeScalarVectorStream(code, decoded).ok());

    const std::array<float, 12> kTurns = {
        0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 100.25f, -0.25f, 255.75f,
        256.0f, 300.25f, 0.125f, std::numeric_limits<float>::quiet_NaN()
    };

    SvWaveState wave{};
    wave.Reset(32u);
    wave.SeedThreadIds(0u);
    wave.uav0.assign(128u, 0u);
    for (uint32_t i = 0u; i < 32u; ++i) {
        wave.uav0[i] = FloatToBits(kTurns[i % kTurns.size()]);
    }
    wave.uav1.assign(128u, 0u);
    OS_REQUIRE(ExecuteScalarVectorCpu(decoded, wave).ok());

    auto check_turns = [&](const std::vector<uint32_t>& out, const char* backend) {
        // Quarter-turn & boundary exactness (turns, NOT raw radians!)
        OS_REQUIRE(BitsToFloat(out[0]) == 0.0f && BitsToFloat(out[32 + 0]) == 1.0f);    // 0.0
        OS_REQUIRE(BitsToFloat(out[1]) == 1.0f && BitsToFloat(out[32 + 1]) == 0.0f);    // 0.25
        OS_REQUIRE(BitsToFloat(out[2]) == 0.0f && BitsToFloat(out[32 + 2]) == -1.0f);   // 0.5
        OS_REQUIRE(BitsToFloat(out[3]) == -1.0f && BitsToFloat(out[32 + 3]) == 0.0f);   // 0.75
        OS_REQUIRE(BitsToFloat(out[4]) == 0.0f && BitsToFloat(out[32 + 4]) == 1.0f);    // 1.0
        OS_REQUIRE(BitsToFloat(out[5]) == 1.0f && BitsToFloat(out[32 + 5]) == 0.0f);    // 100.25 (reduced!)
        OS_REQUIRE(BitsToFloat(out[6]) == -1.0f && BitsToFloat(out[32 + 6]) == 0.0f);   // -0.25
        OS_REQUIRE(BitsToFloat(out[7]) == -1.0f && BitsToFloat(out[32 + 7]) == 0.0f);   // 255.75
        OS_REQUIRE(BitsToFloat(out[8]) == 0.0f && BitsToFloat(out[32 + 8]) == 1.0f);    // |x| >= 256
        OS_REQUIRE(BitsToFloat(out[9]) == 0.0f && BitsToFloat(out[32 + 9]) == 1.0f);    // 300.25 -> clamped
        const float s8 = BitsToFloat(out[10]);
        const float c8 = BitsToFloat(out[32 + 10]);
        OS_REQUIRE(std::fabs(s8 - 0.70710678f) < 1e-4f);
        OS_REQUIRE(std::fabs(c8 - 0.70710678f) < 1e-4f);
        OS_REQUIRE(std::isnan(BitsToFloat(out[11])) && std::isnan(BitsToFloat(out[32 + 11])));

        // FP16 turns checks (0.25 -> sin=1, cos=0; 0.5 -> sin=0, cos=-1)
        OS_REQUIRE(HalfToF32(static_cast<uint16_t>(out[64 + 1])) == 1.0f);
        OS_REQUIRE(HalfToF32(static_cast<uint16_t>(out[96 + 1])) == 0.0f);
        OS_REQUIRE(HalfToF32(static_cast<uint16_t>(out[64 + 2])) == 0.0f);
        OS_REQUIRE(HalfToF32(static_cast<uint16_t>(out[96 + 2])) == -1.0f);
        (void)backend;
    };

    check_turns(wave.uav1, "CPU");

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 32u;
    c.wave_size = 32u;
    c.code_words = code;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftScalarVectorShader(c, decoded, lifted).ok());

    std::vector<uint32_t> vk_out;
    bool ran_vk = false;
    RunOptionalVulkanAndValidators(lifted, wave.uav0, vk_out, ran_vk);
    if (ran_vk) {
        check_turns(vk_out, "Vulkan");
    }

    std::printf("[PASS] Test 1: Rule (a) v_sin/cos_f32 & f16 GFX10.3 turns reduction verified (CPU + DXC + spirv-val%s)\n",
                ran_vk ? " + Vulkan HW" : "");
}

// ---------------------------------------------------------------------------
// Test 2: Rule (b) — s_bfe_u32 / s_bfe_i32 / s_bfe_u64 / s_bfe_i64 AMD rule
// ---------------------------------------------------------------------------
void Test2_SBfeHardwareAccuracy() {
    using namespace openscarlett::rdna2_sv;

    // Part A: Core rdna2_interpreter.cpp s_bfe_u32 & s_bfe_i32 edge cases
    {
        Rdna2Assembler a;
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 2, Rdna2Operand::LiteralU32(0x81234567u));
        // width = 0, offset = 4 -> 0
        a.EmitSop2(Rdna2Opcode::S_BFE_U32, 4, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32(4u));
        a.EmitSop2(Rdna2Opcode::S_BFE_I32, 5, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32(4u));
        // offset = 0, width = 32 -> full value (tests 7-bit width, not masked to 5 bits!)
        a.EmitSop2(Rdna2Opcode::S_BFE_U32, 6, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32(32u << 16u));
        a.EmitSop2(Rdna2Opcode::S_BFE_I32, 7, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32(32u << 16u));
        // offset = 24, width = 16 -> offset + width > 32
        a.EmitSop2(Rdna2Opcode::S_BFE_U32, 8, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32((16u << 16u) | 24u));
        a.EmitSop2(Rdna2Opcode::S_BFE_I32, 9, Rdna2Operand::Sgpr(2), Rdna2Operand::LiteralU32((16u << 16u) | 24u));
        // offset = 8, width = 8 on 0x00008500 -> bit 15 = 1
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 3, Rdna2Operand::LiteralU32(0x00008500u));
        a.EmitSop2(Rdna2Opcode::S_BFE_U32, 10, Rdna2Operand::Sgpr(3), Rdna2Operand::LiteralU32((8u << 16u) | 8u));
        a.EmitSop2(Rdna2Opcode::S_BFE_I32, 11, Rdna2Operand::Sgpr(3), Rdna2Operand::LiteralU32((8u << 16u) | 8u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(4));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 0, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(5));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 4, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(6));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 8, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(7));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 12, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(8));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 16, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(9));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 20, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(10));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 24, false);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::Sgpr(11));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 0, 4, 28, false);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Xs2ShaderContainer container{};
        container.stage = ShaderStage::Compute;
        container.threadgroup_x = 1u;
        container.wave_size = 32u;
        container.code_words = a.Words();
        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(container.code_words, insns).ok());
        Rdna2CpuExecState cpu{};
        cpu.uav1.assign(8u, 0xBADu);
        OS_REQUIRE(ExecuteRdna2Cpu(container, insns, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 0u);
        OS_REQUIRE(cpu.uav1[1] == 0u);
        OS_REQUIRE(cpu.uav1[2] == 0x81234567u);
        OS_REQUIRE(cpu.uav1[3] == 0x81234567u);
        OS_REQUIRE(cpu.uav1[4] == 0x00000081u);
        OS_REQUIRE(cpu.uav1[5] == 0xFFFFFF81u);
        OS_REQUIRE(cpu.uav1[6] == 0x00000085u);
        OS_REQUIRE(cpu.uav1[7] == 0xFFFFFF85u);
    }

    // Part B: rdna2_sv s_bfe_u32 / s_bfe_i32 / s_bfe_u64 / s_bfe_i64 on CPU + DXC + spirv-val + Vulkan HW
    std::vector<uint32_t> code;
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 1u, 0u, 0u, 0u, false);
    OS_REQUIRE(EncodeScalarVectorInst("s_mov_b32", 4u, {Rdna2Operand::LiteralU32(0x81234567u)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("s_mov_b32", 5u, {Rdna2Operand::LiteralU32(0x9ABCDEF0u)}, 0, code));
    auto emit_store_s = [&](uint8_t sreg, uint16_t slot) {
        OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 2u, {Rdna2Operand::Sgpr(sreg)}, 0, code));
        AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 2u, 0u, 4u, static_cast<uint16_t>(slot * 4u), false);
    };

    // 32-bit edge cases
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_u32", 10u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32(0u)}, 0, code));
    emit_store_s(10u, 0u);  // width=0 -> 0
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_u32", 10u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32(32u << 16u)}, 0, code));
    emit_store_s(10u, 1u);  // width=32, off=0 -> 0x81234567
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_u32", 10u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((16u << 16u) | 24u)}, 0, code));
    emit_store_s(10u, 2u);  // off+w>32 -> 0x81
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_i32", 10u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((16u << 16u) | 24u)}, 0, code));
    emit_store_s(10u, 3u);  // off+w>32 signed -> 0xFFFFFF81
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_i32", 10u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((32u << 16u) | 8u)}, 0, code));
    emit_store_s(10u, 4u);  // off=8, w=32 -> 0xFF812345

    // 64-bit edge cases on 0x9ABCDEF0_81234567ull
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_u64", 12u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((36u << 16u) | 20u)}, 0, code));
    emit_store_s(12u, 5u);
    emit_store_s(13u, 6u);
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_i64", 14u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((36u << 16u) | 20u)}, 0, code));
    emit_store_s(14u, 7u);
    emit_store_s(15u, 8u);
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_i64", 16u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32((32u << 16u) | 40u)}, 0, code));
    emit_store_s(16u, 9u);
    emit_store_s(17u, 10u);
    OS_REQUIRE(EncodeScalarVectorInst("s_bfe_u64", 18u, {Rdna2Operand::Sgpr(4), Rdna2Operand::LiteralU32(64u << 16u)}, 0, code));
    emit_store_s(18u, 11u);
    emit_store_s(19u, 12u);
    OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));

    std::vector<SvDecodedInst> decoded;
    OS_REQUIRE(DecodeScalarVectorStream(code, decoded).ok());

    SvWaveState wave{};
    wave.Reset(32u);
    wave.SeedThreadIds(0u);
    wave.uav0.assign(16u, 0u);
    wave.uav1.assign(16u, 0u);
    OS_REQUIRE(ExecuteScalarVectorCpu(decoded, wave).ok());

    // Expected 64-bit values
    const uint64_t val64 = 0x9ABCDEF081234567ull;
    const uint64_t u64_20_36 = (val64 >> 20u) & ((1ull << 36u) - 1ull);
    const uint64_t i64_20_36 = static_cast<uint64_t>(static_cast<int64_t>(u64_20_36 << 28u) >> 28u);
    const uint64_t i64_40_32 = static_cast<uint64_t>(static_cast<int64_t>(val64) >> 40u);

    auto check_bfe = [&](const std::vector<uint32_t>& out) {
        OS_REQUIRE(out[0] == 0u);
        OS_REQUIRE(out[1] == 0x81234567u);
        OS_REQUIRE(out[2] == 0x00000081u);
        OS_REQUIRE(out[3] == 0xFFFFFF81u);
        OS_REQUIRE(out[4] == 0xFF812345u);
        OS_REQUIRE(out[5] == static_cast<uint32_t>(u64_20_36 & 0xFFFFFFFFull));
        OS_REQUIRE(out[6] == static_cast<uint32_t>(u64_20_36 >> 32u));
        OS_REQUIRE(out[7] == static_cast<uint32_t>(i64_20_36 & 0xFFFFFFFFull));
        OS_REQUIRE(out[8] == static_cast<uint32_t>(i64_20_36 >> 32u));
        OS_REQUIRE(out[9] == static_cast<uint32_t>(i64_40_32 & 0xFFFFFFFFull));
        OS_REQUIRE(out[10] == static_cast<uint32_t>(i64_40_32 >> 32u));
        OS_REQUIRE(out[11] == 0x81234567u);
        OS_REQUIRE(out[12] == 0x9ABCDEF0u);
    };

    check_bfe(wave.uav1);

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 32u;
    c.wave_size = 32u;
    c.code_words = code;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftScalarVectorShader(c, decoded, lifted).ok());

    std::vector<uint32_t> vk_out;
    bool ran_vk = false;
    RunOptionalVulkanAndValidators(lifted, wave.uav0, vk_out, ran_vk);
    if (ran_vk) {
        check_bfe(vk_out);
    }

    std::printf("[PASS] Test 2: Rule (b) s_bfe_u32/i32/u64/i64 AMD RDNA2 7-bit width & bounds verified (CPU + DXC + spirv-val%s)\n",
                ran_vk ? " + Vulkan HW" : "");
}

// ---------------------------------------------------------------------------
// Test 3: Rule (c) — VOP2 0x36-0x38 fp16 FMA & VOP3P v_fma_mix* single-rounding
// ---------------------------------------------------------------------------
void Test3_FmaSingleRoundingAndMixAccuracy() {
    using namespace openscarlett::rdna2_sv;

    // Single-rounding FMA discriminator:
    //   a = 1.0f + 2^-12 (0x3F800800), b = 1.0f + 2^-12 (0x3F800800), c = -(1.0f + 2^-11) (0xBF801000)
    //   Exact single-rounding fmaf(a, b, c) == 2^-24 == 0x33800000
    //   Double-rounded (float)(a*b) + c     == 0.0f  == 0x00000000 !
    const float a32 = 1.0f + std::ldexp(1.0f, -12);
    const float b32 = 1.0f + std::ldexp(1.0f, -12);
    const float c32 = -(1.0f + std::ldexp(1.0f, -11));
    const uint32_t expected_fma32 = FloatToBits(std::fmaf(a32, b32, c32));
    OS_REQUIRE(expected_fma32 == 0x33800000u);

    // FP16 single-rounding FMA discriminator:
    //   a16 = 0x3C01 (1 + 2^-10), b16 = 0x3C01 (1 + 2^-10), c16 = 0xBC02 (-(1 + 2^-9))
    //   Exact fmaf(a16, b16, c16) = 2^-20 -> FP16 subnormal 0x0010 !
    //   Double-rounded f16(a16*b16) + c16 == 0.0f (0x0000) !
    const uint32_t a16 = 0x3C01u;
    const uint32_t b16 = 0x3C01u;
    const uint32_t c16 = 0xBC02u;

    std::vector<uint32_t> code;
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 2u, 0u, 0u, 0u, false);   // a32
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 3u, 0u, 0u, 4u, false);   // b32
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 4u, 0u, 0u, 8u, false);   // c32
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 5u, 0u, 0u, 12u, false);  // a16
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 6u, 0u, 0u, 16u, false);  // b16
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_LOAD_DWORD, 7u, 0u, 0u, 20u, false);  // c16

    // 1. VOP2 v_fmac_f16 (0x36): v[10] = fma(a16, b16, c16)
    OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 10u, {Rdna2Operand::Vgpr(7)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("v_fmac_f16", 10u, {Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6)}, 0, code));
    // 2. VOP2 v_fmamk_f16 (0x37): v[11] = fma(a16, K=b16, c16)
    OS_REQUIRE(EncodeScalarVectorInst("v_fmamk_f16", 11u,
        {Rdna2Operand::Vgpr(5), Rdna2Operand::LiteralU32(b16), Rdna2Operand::Vgpr(7)}, static_cast<int32_t>(b16), code));
    // 3. VOP2 v_fmaak_f16 (0x38): v[12] = fma(a16, b16, K=c16)
    OS_REQUIRE(EncodeScalarVectorInst("v_fmaak_f16", 12u,
        {Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::LiteralU32(c16)}, static_cast<int32_t>(c16), code));
    // 4. VOP3P v_fma_mix_f32 (0x20) with all FP32 sources (imm=0x20000 -> op_sel=0, op_sel_hi=0)
    OS_REQUIRE(EncodeScalarVectorInst("v_fma_mix_f32", 13u,
        {Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4)}, 0x20000, code));
    // 5. VOP3P v_fma_mixlo_f16 (0x21) with all FP16 lo sources (imm=0 -> op_sel_hi=7, op_sel=0), preserving upper 16 bits 0xDEAD
    OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 14u, {Rdna2Operand::LiteralU32(0xDEADCAFEu)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("v_fma_mixlo_f16", 14u,
        {Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7)}, 0, code));
    // 6. VOP3P v_fma_mixhi_f16 (0x22) with all FP16 lo sources, preserving lower 16 bits 0xBEEF
    OS_REQUIRE(EncodeScalarVectorInst("v_mov_b32", 15u, {Rdna2Operand::LiteralU32(0x1234BEEFu)}, 0, code));
    OS_REQUIRE(EncodeScalarVectorInst("v_fma_mixhi_f16", 15u,
        {Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7)}, 0, code));

    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 10u, 0u, 4u, 0u, false);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 11u, 0u, 4u, 4u, false);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 12u, 0u, 4u, 8u, false);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 13u, 0u, 4u, 12u, false);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 14u, 0u, 4u, 16u, false);
    AppendMubufLoadStore(code, Rdna2Opcode::BUFFER_STORE_DWORD, 15u, 0u, 4u, 20u, false);
    OS_REQUIRE(EncodeScalarVectorInst("s_endpgm", 0u, {}, 0, code));

    std::vector<SvDecodedInst> decoded;
    OS_REQUIRE(DecodeScalarVectorStream(code, decoded).ok());

    SvWaveState wave{};
    wave.Reset(32u);
    wave.SeedThreadIds(0u);
    wave.uav0 = {FloatToBits(a32), FloatToBits(b32), FloatToBits(c32), a16, b16, c16, 0u, 0u};
    wave.uav1.assign(8u, 0u);
    OS_REQUIRE(ExecuteScalarVectorCpu(decoded, wave).ok());

    auto check_fma = [&](const std::vector<uint32_t>& out) {
        OS_REQUIRE(out[0] == 0x0010u);           // v_fmac_f16 single-rounded subnormal 2^-20
        OS_REQUIRE(out[1] == 0x0010u);           // v_fmamk_f16
        OS_REQUIRE(out[2] == 0x0010u);           // v_fmaak_f16
        OS_REQUIRE(out[3] == expected_fma32);    // v_fma_mix_f32 == 2^-24 (NOT 0!)
        OS_REQUIRE(out[4] == 0xDEAD0010u);       // v_fma_mixlo_f16 preserved hi16 0xDEAD
        OS_REQUIRE(out[5] == 0x0010BEEFu);       // v_fma_mixhi_f16 preserved lo16 0xBEEF
    };

    check_fma(wave.uav1);

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 32u;
    c.wave_size = 32u;
    c.code_words = code;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftScalarVectorShader(c, decoded, lifted).ok());

    std::vector<uint32_t> vk_out;
    bool ran_vk = false;
    RunOptionalVulkanAndValidators(lifted, wave.uav0, vk_out, ran_vk);
    if (ran_vk) {
        check_fma(vk_out);
    }

    std::printf("[PASS] Test 3: Rule (c) VOP2 fp16 FMA (0x36-0x38) & VOP3P v_fma_mix* single-rounding verified (CPU + DXC + spirv-val%s)\n",
                ran_vk ? " + Vulkan HW" : "");
}

// ---------------------------------------------------------------------------
// Test 4: Rule (d) — MTBUF partial writes, SNORM round-to-nearest-even ties,
//                    & D16 load high-half VGPR preservation
// ---------------------------------------------------------------------------
void Test4_MtbufPartialWriteSnormTiesAndD16HiPreserve() {
    using namespace openscarlett::rdna2_mem;

    // 4A. Direct unit check of SNORM/UNORM round-to-nearest-even ties
    {
        uint8_t buf[16]{};
        std::memset(buf, 0xAA, sizeof(buf));
        PackMtbufFormatComponent(buf, sizeof(buf), 0u, Rdna2MtbufFormat::R8G8B8A8_SNORM, 0u, FloatToBits(0.5f / 127.0f), false);
        PackMtbufFormatComponent(buf, sizeof(buf), 0u, Rdna2MtbufFormat::R8G8B8A8_SNORM, 1u, FloatToBits(1.5f / 127.0f), false);
        PackMtbufFormatComponent(buf, sizeof(buf), 0u, Rdna2MtbufFormat::R8G8B8A8_SNORM, 2u, FloatToBits(-0.5f / 127.0f), false);
        PackMtbufFormatComponent(buf, sizeof(buf), 0u, Rdna2MtbufFormat::R8G8B8A8_SNORM, 3u, FloatToBits(-1.5f / 127.0f), false);
        OS_REQUIRE(static_cast<int8_t>(buf[0]) == 0);
        OS_REQUIRE(static_cast<int8_t>(buf[1]) == 2);
        OS_REQUIRE(static_cast<int8_t>(buf[2]) == 0);
        OS_REQUIRE(static_cast<int8_t>(buf[3]) == -2);

        PackMtbufFormatComponent(buf, sizeof(buf), 4u, Rdna2MtbufFormat::R16G16_SNORM, 0u, FloatToBits(0.5f / 32767.0f), false);
        PackMtbufFormatComponent(buf, sizeof(buf), 4u, Rdna2MtbufFormat::R16G16_SNORM, 1u, FloatToBits(1.5f / 32767.0f), false);
        int16_t s0 = 0, s1 = 0;
        std::memcpy(&s0, buf + 4, 2);
        std::memcpy(&s1, buf + 6, 2);
        OS_REQUIRE(s0 == 0 && s1 == 2);
    }

    // 4B. Core rdna2_interpreter.cpp D16_X / D16_XYZ upper-16-bit VGPR preservation
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::LiteralU32(0xABCD1111u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralU32(0x56782222u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(0));
        a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X, 8, 1, 0, Rdna2MtbufFormat::R16G16B16A16_FLOAT, 0, true);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 0, true);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::LiteralU32(0xABCD1111u));
        a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ, 8, 1, 0, Rdna2MtbufFormat::R16G16B16A16_FLOAT, 0, true);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 4, true);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 8, true);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1u;
        c.wave_size = 32u;
        c.code_words = a.Words();
        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(c.code_words, insns).ok());
        Rdna2CpuExecState cpu{};
        cpu.uav0 = {0x40003C00u, 0x44004200u, 0u, 0u};  // {1.0, 2.0, 3.0, 4.0} fp16
        cpu.uav1.assign(4u, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(c, insns, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 0xABCD3C00u);  // d16_x preserved upper 0xABCD!
        OS_REQUIRE(cpu.uav1[1] == 0x40003C00u);  // d16_xyz lo0,lo1
        OS_REQUIRE(cpu.uav1[2] == 0x56784200u);  // d16_xyz preserved upper 0x5678 of v[9]!
    }

    // 4C. Full shader test via rdna2_mem: partial store + SNORM ties + D16 hi-preserve on CPU + DXC + spirv-val + Vulkan HW
    std::vector<uint32_t> code;
    auto push_mem = [&](const char* mnem, const MemImgEncodeParams& p) {
        OS_REQUIRE(EncodeMemImgInst(mnem, p, code));
    };

    // Load sentinels & float inputs from uav0:
    // uav0[0] = 0xDEADBEEF (sentinel for partial store)
    // uav0[1] = 0xCAFEBABE (sentinel for xy partial store)
    // uav0[2..5] = {0.5/127, 1.5/127, -0.5/127, -1.5/127} (SNORM8 ties)
    // uav0[6] = float(0x42 / 255.0f)
    // uav0[7..8] = {0x40003C00, 0x44004200} (fp16 {1.0, 2.0, 3.0, 4.0})
    // uav0[9..10] = {0xABCD1111, 0x56782222} (D16 VGPR pre-seed)
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 0u;
        p.vdata = 10u; p.offset = 0; push_mem("buffer_load_dwordx4", p);   // v[10..13] = uav0[0..3]
        p.vdata = 14u; p.offset = 16; push_mem("buffer_load_dwordx4", p);  // v[14..17] = uav0[4..7]
        p.vdata = 18u; p.offset = 32; push_mem("buffer_load_dwordx4", p);  // v[18..21] = uav0[8..11]
    }
    // Seed uav1[0] = 0xDEADBEEF, uav1[1] = 0xCAFEBABE
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 4u;
        p.vdata = 10u; p.offset = 0; push_mem("buffer_store_dword", p);
        p.vdata = 11u; p.offset = 4; push_mem("buffer_store_dword", p);
    }
    // Partial write 1: tbuffer_store_format_x (R8G8B8A8_UNORM) at uav1 byte 0 -> writes ONLY byte 0 (0x42), preserves 0xDEADBE__!
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 4u;
        p.vdata = 16u; p.offset = 0; p.mtbuf_format = Rdna2MtbufFormat::R8G8B8A8_UNORM;
        push_mem("tbuffer_store_format_x", p);
    }
    // Partial write 2: tbuffer_store_format_xy (R8G8B8A8_SNORM) at uav1 byte 4 -> writes bytes 4..5 (0, 2), preserves 0xCAFE____!
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 4u;
        p.vdata = 12u; p.offset = 4; p.mtbuf_format = Rdna2MtbufFormat::R8G8B8A8_SNORM;
        push_mem("tbuffer_store_format_xy", p);
    }
    // Full SNORM8 ties write at uav1 byte 8: {0.5/127, 1.5/127, -0.5/127, -1.5/127} -> bytes {0x00, 0x02, 0x00, 0xFE} = 0xFE000200
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 4u;
        p.vdata = 12u; p.offset = 8; p.mtbuf_format = Rdna2MtbufFormat::R8G8B8A8_SNORM;
        push_mem("tbuffer_store_format_xyzw", p);
    }
    // D16_X load into v[19] (pre-seeded 0xABCD1111) from uav0 byte 28 -> v[19] = 0xABCD3C00
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 0u;
        p.vdata = 19u; p.offset = 28; p.mtbuf_format = Rdna2MtbufFormat::R16G16B16A16_FLOAT;
        push_mem("tbuffer_load_format_d16_x", p);
        p.srsrc_sgpr = 4u;
        p.vdata = 19u; p.offset = 12;
        push_mem("buffer_store_dword", p);
    }
    // D16_XYZ load into v[19..20] (v[20] pre-seeded 0x56782222) -> v[20] = 0x56784200
    {
        MemImgEncodeParams p{};
        p.srsrc_sgpr = 0u;
        p.vdata = 19u; p.offset = 28; p.mtbuf_format = Rdna2MtbufFormat::R16G16B16A16_FLOAT;
        push_mem("tbuffer_load_format_d16_xyz", p);
        p.srsrc_sgpr = 4u;
        p.vdata = 19u; p.offset = 16;
        push_mem("buffer_store_dwordx2", p);
    }
    code.push_back(0xBF810000u);  // s_endpgm

    std::vector<MemImgDecodedInst> decoded;
    OS_REQUIRE(DecodeMemImgStream(code, decoded).ok());

    const std::vector<uint32_t> uav0_u32 = {
        0xDEADBEEFu,
        0xCAFEBABEu,
        FloatToBits(0.5f / 127.0f),
        FloatToBits(1.5f / 127.0f),
        FloatToBits(-0.5f / 127.0f),
        FloatToBits(-1.5f / 127.0f),
        FloatToBits(static_cast<float>(0x42u) / 255.0f),
        0x40003C00u,
        0x44004200u,
        0xABCD1111u,
        0x56782222u,
        0u,
    };

    MemImgExecState state{};
    state.wave_size = 1u;
    state.exec_mask = 1ull;
    SeedDefaultComputeVgprs(state);
    state.uav0.resize(uav0_u32.size() * 4u, 0u);
    std::memcpy(state.uav0.data(), uav0_u32.data(), state.uav0.size());
    state.uav1.assign(uav0_u32.size() * 4u, 0u);
    OS_REQUIRE(ExecuteMemImgCpu(decoded, state).ok());

    std::vector<uint32_t> cpu_out(uav0_u32.size(), 0u);
    std::memcpy(cpu_out.data(), state.uav1.data(), state.uav1.size());

    auto check_mtbuf = [&](const std::vector<uint32_t>& out) {
        OS_REQUIRE(out[0] == 0xDEADBE42u);  // partial store_x preserved upper 3 bytes!
        OS_REQUIRE(out[1] == 0xCAFE0200u);  // partial store_xy preserved upper 2 bytes + RNE ties (0, 2)!
        OS_REQUIRE(out[2] == 0xFE000200u);  // SNORM8 RNE ties: {0, 2, 0, -2}!
        OS_REQUIRE(out[3] == 0xABCD3C00u);  // d16_x preserved upper 16 bits!
        OS_REQUIRE(out[4] == 0x40003C00u);  // d16_xyz comp0..1
        OS_REQUIRE(out[5] == 0x56784200u);  // d16_xyz preserved upper 16 bits of v[20]!
    };

    check_mtbuf(cpu_out);

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 1u;
    c.wave_size = 32u;
    c.code_words = code;
    Rdna2LiftResult lifted{};
    OS_REQUIRE(LiftMemImgShader(c, decoded, lifted).ok());

    std::vector<uint32_t> vk_out;
    bool ran_vk = false;
    RunOptionalVulkanAndValidators(lifted, uav0_u32, vk_out, ran_vk);
    if (ran_vk) {
        check_mtbuf(vk_out);
    }

    std::printf("[PASS] Test 4: Rule (d) MTBUF partial writes, SNORM ties-to-even & D16 hi-preserve verified (CPU + DXC + spirv-val%s)\n",
                ran_vk ? " + Vulkan HW" : "");
}

// ---------------------------------------------------------------------------
// Test 5: Expanded Real-GPU Opcode Verification (>= 300/779, all 20 families)
// ---------------------------------------------------------------------------
void Test5_ExpandedRealGpuOpcodeVerification() {
    Rdna2BenchOptions opts{};
    opts.full_corpus = false;
    opts.corpus_count = 432u;
    opts.external_val_sample = 432u;
    opts.run_live_gpu = true;
    opts.run_external_validators = true;

    Rdna2CoverageReport rep{};
    Status st = RunRdna2CoverageBenchmark(opts, rep);
    if (!st.ok()) {
        std::fprintf(stderr, "[FAIL] RunRdna2CoverageBenchmark: %s\n", st.message.c_str());
    }
    OS_REQUIRE(st.ok());
    OS_REQUIRE(rep.opcodes_total == 779u);
    OS_REQUIRE(rep.corpus_shaders_passed == 432u);

    if (rep.has_spirv_val_tool) {
        OS_REQUIRE(rep.spirv_val_external_validated >= 400u);
    }
    if (rep.has_dxc_tool) {
        OS_REQUIRE(rep.corpus_dxil_compiled >= 300u);
        OS_REQUIRE(rep.corpus_dxil_resigned_verified == rep.corpus_dxil_compiled);
    }
    if (rep.has_d3d12_hw || rep.has_d3d12_warp || rep.has_vulkan_hw) {
        OS_REQUIRE(rep.opcodes_gpu_verified >= 300u);
        if ((rep.has_d3d12_hw || rep.has_d3d12_warp) && rep.has_dxc_tool) {
            OS_REQUIRE(rep.families.size() == 20u);
            for (const auto& f : rep.families) {
                if (f.gpu_verified == 0u) {
                    std::fprintf(stderr, "[FAIL] Family %s has 0 gpu_verified opcodes!\n", f.family.c_str());
                }
                OS_REQUIRE(f.gpu_verified > 0u);
            }
        }
    }

    std::printf(
        "[PASS] Test 5: Expanded real-GPU verification passed: opcodes_gpu_verified=%u/779 | "
        "spirv-val=%u | DXIL=%u (resigned=%u) | D3D12-HW=%u, WARP=%u, Vulkan=%u | %.2fs\n",
        rep.opcodes_gpu_verified,
        rep.spirv_val_external_validated,
        rep.corpus_dxil_compiled,
        rep.corpus_dxil_resigned_verified,
        rep.corpus_gpu_hw_executed,
        rep.corpus_gpu_warp_executed,
        rep.corpus_vulkan_executed,
        rep.elapsed_seconds);
}

}  // namespace

int main() {
    Test1_TrigTurnsAccuracy();
    Test2_SBfeHardwareAccuracy();
    Test3_FmaSingleRoundingAndMixAccuracy();
    Test4_MtbufPartialWriteSnormTiesAndD16HiPreserve();
    Test5_ExpandedRealGpuOpcodeVerification();
    std::printf("=== ALL RDNA2 GPU ACCURACY & VERIFICATION TESTS PASSED ===\n");
    return 0;
}
