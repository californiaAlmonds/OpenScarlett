#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/shader_cache.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

uint32_t PackTwoHalfBits(uint16_t lo, uint16_t hi) noexcept {
    return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
}

void TestBasicComputeAndConstFold() {
    using namespace openscarlett;

    Rdna2Assembler a;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(2.0f));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 3, Rdna2Operand::Vgpr(1), 2);       // foldable 2.0 + 1.0
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 4, Rdna2Operand::Vgpr(0));      // float(tid)
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 5, Rdna2Operand::Vgpr(4), 3);       // tid * 3.0
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 6, 0);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 8, 8);
    Xs2ShaderContainer parsed;
    OS_REQUIRE(ParseXs2Container(xs2, parsed).ok());
    OS_REQUIRE(parsed.stage == ShaderStage::Compute);
    OS_REQUIRE(parsed.code_words.size() == a.Words().size());

    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(parsed.code_words, insns).ok());
    OS_REQUIRE(insns.size() == 8);
    OS_REQUIRE(insns.back().opcode == Rdna2Opcode::S_ENDPGM);

    Rdna2IrModule ir;
    OS_REQUIRE(BuildRdna2Ir(parsed, insns, ir).ok());
    Rdna2IrOptStats stats{};
    OS_REQUIRE(OptimizeRdna2Ir(ir, &stats).ok());
    OS_REQUIRE(stats.folded_constants >= 1);

    // Execute on CPU interpreter and check output
    Rdna2CpuExecState cpu{};
    cpu.uav0.assign(64, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(parsed, insns, cpu).ok());
    for (uint32_t i = 0; i < 64u; ++i) {
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[i]) - static_cast<float>(i) * 3.0f) < 1e-5f);
    }

    std::printf("[PASS] Core 1: Compute disasm + IR const-fold (%u folded, %u dead) + CPU exec\n",
                stats.folded_constants, stats.eliminated_ops);
}

void TestAaaOpcodesEncodeDecodeIrAndCpu() {
    using namespace openscarlett;

    // 1. Assemble all new AAA opcodes and verify exact binary round-trip + disassembly + IR
    Rdna2Assembler a;
    // Bitfield ops: V_BFE_U32, V_BFE_I32, V_BFI_B32
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x1234F89Au));
    a.EmitVop3(Rdna2Opcode::V_BFE_U32, 2, Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
    a.EmitVop3(Rdna2Opcode::V_BFE_I32, 3, Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
    a.EmitVop3(Rdna2Opcode::V_BFI_B32, 4, Rdna2Operand::LiteralU32(0x00FF00FFu), Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(0));

    // VOP3P (0x33) packed 16-bit f16 & u16/b16 ops
    // half(1.5) = 0x3E00, half(2.0) = 0x4000, half(0.5) = 0x3800, half(4.0) = 0x4400
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3E00u, 0x4000u)));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3800u, 0x4400u)));
    a.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 7, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
    a.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 8, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
    a.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 9, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(5));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0010u, 0x0100u)));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0002u, 0x0003u)));
    a.EmitVop3p(Rdna2Opcode::V_PK_ADD_U16, 12, Rdna2Operand::Vgpr(10), Rdna2Operand::Vgpr(11));
    a.EmitVop3p(Rdna2Opcode::V_PK_LSHLREV_B16, 13, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(10));
    a.EmitVop3p(Rdna2Opcode::V_PK_LSHRREV_B16, 14, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(10));

    // V_PERMLANE16_B32 & V_PERMLANEX16_B32 (reverse 16-lane row: 0x76543210 / 0xFEDCBA98 -> reversed = 0x89ABCDEF / 0x01234567)
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
    a.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 15, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
    a.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 16, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));

    // SCRATCH_STORE_DWORD & SCRATCH_LOAD_DWORD (FLAT encoding with flat_seg == 1)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17, Rdna2Operand::InlineInt(16));
    a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 16, 17, 4);
    a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 18, 17, 4);

    // MIMG: IMAGE_LOAD_MIP (3D dim=2), IMAGE_LOAD_MIP (Cube dim=3), IMAGE_ATOMIC_CMPSWAP, IMAGE_BVH_INTERSECT_RAY
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 20, 24, 0, 0, 0xFu, 2u, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 28, 24, 0, 0, 0xFu, 3u, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP, 32, 24, 0, 0, 0x1u, 1u, true, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 36, 40, 0, 0, 0xFu, 0u, false);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());
    OS_REQUIRE(insns.size() == 26u);

    // Verify exact VOP3P encoding prefix 0x33 and FLAT flat_seg == 1
    bool saw_vop3p = false;
    bool saw_scratch_seg1 = false;
    bool saw_bvh = false;
    for (const auto& inst : insns) {
        if (inst.encoding == Rdna2Encoding::VOP3P) {
            OS_REQUIRE((inst.raw_words[0] >> 26) == 0x33u);
            saw_vop3p = true;
        }
        if (inst.opcode == Rdna2Opcode::SCRATCH_LOAD_DWORD || inst.opcode == Rdna2Opcode::SCRATCH_STORE_DWORD) {
            OS_REQUIRE(inst.flat_seg == 1u);
            saw_scratch_seg1 = true;
        }
        if (inst.opcode == Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY) {
            OS_REQUIRE(inst.raw_opcode == 0xE6u);
            saw_bvh = true;
        }
    }
    OS_REQUIRE(saw_vop3p && saw_scratch_seg1 && saw_bvh);

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 32;
    c.wave_size = 32;
    c.code_words = a.Words();

    Rdna2IrModule ir{};
    OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
    OS_REQUIRE(ir.uses_scratch);
    OS_REQUIRE(ir.uses_texture3d_srv);
    OS_REQUIRE(ir.uses_texture_cube_srv);
    OS_REQUIRE(ir.uses_texture_u32_uav);
    OS_REQUIRE(ir.uses_bvh_ray);
    OS_REQUIRE(ir.uses_wave64_emulation);

    Rdna2IrOptStats opt{};
    OS_REQUIRE(OptimizeRdna2Ir(ir, &opt).ok());
    OS_REQUIRE(opt.folded_constants >= 6u);  // BFE/BFI + VOP3P constants folded

    // 2. Verify CPU RDNA2 interpreter on bitfield + VOP3P + scratch + permlane + BVH + MIMG 3D/Cube/Atomic
    Rdna2Assembler b;
    // v1 = tid * 4
    b.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // Bitfield checks stored to uav1[0..2] on lane 0
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x1234F89Au));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralU32(0x0000AB00u));
    b.EmitVop3(Rdna2Opcode::V_BFE_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));   // 0xF8
    b.EmitVop3(Rdna2Opcode::V_BFE_I32, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));   // -8 (0xFFFFFFF8)
    b.EmitVop3(Rdna2Opcode::V_BFI_B32, 5, Rdna2Operand::LiteralU32(0x0000FF00u), Rdna2Operand::Vgpr(20), Rdna2Operand::Vgpr(2));  // 0x1234AB9A
    // Packed f16: (1.5, 2.0) + (0.5, 4.0) = (2.0, 6.0) -> (0x4000, 0x4600)
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3E00u, 0x4000u)));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3800u, 0x4400u)));
    b.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 8, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
    b.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 9, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));   // (0.75=0x3A00, 8.0=0x4800)
    b.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 10, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(6));  // (2.25=0x4080, 10.0=0x4900)
    // Packed u16
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0010u, 0x0100u)));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0002u, 0x0003u)));
    b.EmitVop3p(Rdna2Opcode::V_PK_ADD_U16, 13, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12));      // (0x0012, 0x0103)
    b.EmitVop3p(Rdna2Opcode::V_PK_LSHLREV_B16, 14, Rdna2Operand::Vgpr(12), Rdna2Operand::Vgpr(11)); // (0x0040, 0x0800)
    b.EmitVop3p(Rdna2Opcode::V_PK_LSHRREV_B16, 15, Rdna2Operand::Vgpr(12), Rdna2Operand::Vgpr(11)); // (0x0004, 0x0020)
    // Scratch round-trip
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::InlineInt(8));
    b.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 10, 16, 4);
    b.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 17, 16, 4);
    // Permlane16 reverse + permlanex16 swap halves
    b.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
    b.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
    b.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 18, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
    b.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 19, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));

    // Store per-lane results to uav1 (srsrc=1)
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);     // [0..31]   BFE_U32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 128);   // [32..63]  BFE_I32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 256);   // [64..95]  BFI_B32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 384);   // [96..127] PK_ADD_F16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 512);   // [128..159] PK_MUL_F16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 1, 4, 640);  // [160..191] SCRATCH(PK_FMA_F16)
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 768);  // [192..223] PK_ADD_U16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 1, 4, 896);  // [224..255] PK_LSHLREV_B16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 1, 4, 1024); // [256..287] PK_LSHRREV_B16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 18, 1, 4, 1152); // [288..319] PERMLANE16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 19, 1, 4, 1280); // [320..351] PERMLANEX16
    b.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2CpuExecState cpu{};
    cpu.uav1.assign(352, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(b.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 32, 32), cpu).ok());
    OS_REQUIRE(cpu.uav1[0] == 0x000000F8u);
    OS_REQUIRE(cpu.uav1[32] == 0xFFFFFFF8u);
    OS_REQUIRE(cpu.uav1[64] == 0x1234AB9Au);
    OS_REQUIRE(cpu.uav1[96] == PackTwoHalfBits(0x4000u, 0x4600u));
    OS_REQUIRE(cpu.uav1[128] == PackTwoHalfBits(0x3A00u, 0x4800u));
    OS_REQUIRE(cpu.uav1[160] == PackTwoHalfBits(0x4080u, 0x4900u));
    OS_REQUIRE(cpu.uav1[192] == PackTwoHalfBits(0x0012u, 0x0103u));
    OS_REQUIRE(cpu.uav1[224] == PackTwoHalfBits(0x0040u, 0x0800u));
    OS_REQUIRE(cpu.uav1[256] == PackTwoHalfBits(0x0004u, 0x0020u));
    for (uint32_t l = 0; l < 32u; ++l) {
        const uint32_t exp_p16 = (l & ~15u) | (15u - (l & 15u));
        const uint32_t exp_px16 = ((l & ~15u) ^ 16u) | (15u - (l & 15u));
        OS_REQUIRE(cpu.uav1[288 + l] == exp_p16);
        OS_REQUIRE(cpu.uav1[320 + l] == exp_px16);
    }

    // 3. Verify CPU interpreter BVH triangle & box intersection + MIMG 3D/Cube/Atomic
    {
        Rdna2Assembler rt;
        // Ray 0 (lane 0): hits triangle at byte offset 0 (hit_id = 42, t = 5.0, u = 0.25, v = 0.25)
        // Ray 1 (lane 1): hits AABB box at byte offset 64 (hit_id = 77, t_enter = 2.0, t_exit = 4.0)
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        rt.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(64));
        rt.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20, Rdna2Operand::Vgpr(10), 11);  // node_ptr = lane==1 ? 64 : 0
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralF32(100.0f));  // tmax
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 22, Rdna2Operand::LiteralF32(0.25f));   // ox
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::LiteralF32(0.25f));   // oy
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::InlineFloat(0.0f));   // oz
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineFloat(0.0f));   // dx
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::InlineFloat(0.0f));   // dy
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::InlineFloat(1.0f));   // dz
        rt.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 30, 20, 0, 0, 0xFu, 0u, false);
        rt.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);  // tid * 16
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 30, 1, 4, 0);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 31, 1, 4, 4);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 32, 1, 4, 8);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 33, 1, 4, 12);
        rt.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2CpuExecState rt_state{};
        rt_state.uav0.assign(32, 0u);
        // Triangle at uav0[0..9]: (0,0,5), (1,0,5), (0,1,5), id=42
        rt_state.uav0[0] = 42u;
        rt_state.uav0[1] = FloatToBits(0.0f); rt_state.uav0[2] = FloatToBits(0.0f); rt_state.uav0[3] = FloatToBits(5.0f);
        rt_state.uav0[4] = FloatToBits(1.0f); rt_state.uav0[5] = FloatToBits(0.0f); rt_state.uav0[6] = FloatToBits(5.0f);
        rt_state.uav0[7] = FloatToBits(0.0f); rt_state.uav0[8] = FloatToBits(1.0f); rt_state.uav0[9] = FloatToBits(5.0f);
        // AABB Box at uav0[16..22] (byte offset 64): bmin=(0,0,2), bmax=(1,1,4), id=77 | 0x80000000
        rt_state.uav0[16] = 0x80000000u | 77u;
        rt_state.uav0[17] = FloatToBits(0.0f); rt_state.uav0[18] = FloatToBits(0.0f); rt_state.uav0[19] = FloatToBits(2.0f);
        rt_state.uav0[20] = FloatToBits(1.0f); rt_state.uav0[21] = FloatToBits(1.0f); rt_state.uav0[22] = FloatToBits(4.0f);
        rt_state.uav1.assign(8, 0u);

        OS_REQUIRE(ExecuteRdna2BlobCpu(rt.BuildXs2(ShaderStage::Compute, 2, 1, 1, 16, 40, 32), rt_state).ok());
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[0]) - 5.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[1]) - 0.25f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[2]) - 0.25f) < 1e-5f);
        OS_REQUIRE(rt_state.uav1[3] == 42u);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[4]) - 2.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[5]) - 4.0f) < 1e-5f);
        OS_REQUIRE(rt_state.uav1[7] == 77u);
    }

    std::printf("[PASS] Core 2: AAA opcodes (permlane16/x16, bfe/bfi, VOP3P f16/u16, scratch, MIMG 3D/Cube/atomic, BVH) + CPU interpreter\n");
}

void TestXboxDxbcContainerParser() {
    using namespace openscarlett;

    Rdna2Assembler a;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(7));
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64);

    // 1. Raw XS2 detection
    OS_REQUIRE(IsXboxShaderBytecode(xs2.data(), xs2.size()));
    XboxDxbcContainer parsed_raw{};
    OS_REQUIRE(ParseXboxShaderContainer(xs2, parsed_raw).ok());
    OS_REQUIRE(parsed_raw.format == XboxShaderBlobFormat::RawXs2);
    OS_REQUIRE(parsed_raw.has_xs2);
    OS_REQUIRE(parsed_raw.xs2.wave_size == 64u);

    // 2. DXBC container with XBOX + ISG1 + OSG1 + STSM + DXIL
    XboxDxbcBuildDesc desc{};
    desc.xs2_blob = xs2;
    desc.xbox_chunk_fourcc = "XBOX";
    desc.input_signature.push_back({"POSITION", 0, 0, 3, 0, 0xFu, 0xFu, 0});
    desc.input_signature.push_back({"TEXCOORD", 0, 0, 3, 1, 0x3u, 0x3u, 0});
    desc.output_signature.push_back({"SV_TARGET", 0, 0, 3, 0, 0xFu, 0xFu, 0});
    XboxStaticSamplerInfo samp{};
    samp.filter = 0x15u;
    samp.address_u = 1u;
    samp.shader_register = 2u;
    samp.max_lod = 12.0f;
    desc.static_samplers.push_back(samp);
    desc.aux_dxil_or_shdr = {0x10u, 0x20u, 0x30u, 0x40u};
    desc.aux_shader_fourcc = "DXIL";

    const auto dxbc_xbox = BuildXboxDxbcContainer(desc);
    OS_REQUIRE(IsXboxShaderBytecode(dxbc_xbox.data(), dxbc_xbox.size()));

    XboxDxbcContainer parsed{};
    OS_REQUIRE(ParseXboxShaderContainer(dxbc_xbox, parsed).ok());
    OS_REQUIRE(parsed.format == XboxShaderBlobFormat::XboxDxbc);
    OS_REQUIRE(parsed.has_xs2);
    OS_REQUIRE(parsed.has_dxil_or_shdr);
    OS_REQUIRE(parsed.host_shader_chunk_fourcc == "DXIL");
    OS_REQUIRE(parsed.input_signature.size() == 2u);
    OS_REQUIRE(parsed.input_signature[0].semantic_name == "POSITION");
    OS_REQUIRE(parsed.input_signature[1].semantic_name == "TEXCOORD");
    OS_REQUIRE(parsed.output_signature.size() == 1u);
    OS_REQUIRE(parsed.output_signature[0].semantic_name == "SV_TARGET");
    OS_REQUIRE(parsed.static_samplers.size() == 1u);
    OS_REQUIRE(parsed.static_samplers[0].filter == 0x15u);
    OS_REQUIRE(parsed.static_samplers[0].shader_register == 2u);
    OS_REQUIRE(parsed.xs2.wave_size == 64u);

    // 3. DXBC container with XS2M + RTS0 root signature static samplers
    XboxDxbcBuildDesc desc2{};
    desc2.xs2_blob = xs2;
    desc2.xbox_chunk_fourcc = "XS2M";
    desc2.static_samplers.push_back(samp);
    desc2.emit_rts0_for_samplers = true;
    const auto dxbc_xs2m = BuildXboxDxbcContainer(desc2);
    OS_REQUIRE(IsXboxShaderBytecode(dxbc_xs2m.data(), dxbc_xs2m.size()));
    XboxDxbcContainer parsed2{};
    OS_REQUIRE(ParseXboxShaderContainer(dxbc_xs2m, parsed2).ok());
    OS_REQUIRE(parsed2.format == XboxShaderBlobFormat::XboxDxbc);
    OS_REQUIRE(parsed2.has_xs2);
    OS_REQUIRE(!parsed2.raw_root_signature_chunk.empty());
    OS_REQUIRE(parsed2.static_samplers.size() == 1u);
    OS_REQUIRE(parsed2.static_samplers[0].shader_register == 2u);

    // 4. Standard host DXBC (no XBOX/XS2M) must return IsXboxShaderBytecode == false
    XboxDxbcBuildDesc host_desc{};
    host_desc.aux_dxil_or_shdr = {0x01u, 0x02u, 0x03u, 0x04u};
    host_desc.aux_shader_fourcc = "DXIL";
    const auto host_dxbc = BuildXboxDxbcContainer(host_desc);
    OS_REQUIRE(!IsXboxShaderBytecode(host_dxbc.data(), host_dxbc.size()));
    XboxDxbcContainer parsed_host{};
    OS_REQUIRE(ParseXboxShaderContainer(host_dxbc, parsed_host).ok());
    OS_REQUIRE(parsed_host.format == XboxShaderBlobFormat::StandardHostDxbc);
    OS_REQUIRE(!parsed_host.has_xs2);

    std::printf("[PASS] Core 3: Xbox DXBC/DXIL container parser (XS2/XBOX/XS2M/ISG1/OSG1/STSM/RTS0/host passthrough)\n");
}

void TestShaderCacheAtomicsAndCorruptRecovery() {
    using namespace openscarlett;

    // Isolated temp dir under %TEMP% (strictly on E:\ under build_wt.cmd)
    std::filesystem::path cache_dir;
#if defined(_WIN32)
    char env_buf[1024]{};
    DWORD len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len > 0 && len < sizeof(env_buf)) {
        cache_dir = std::filesystem::path(std::string(env_buf, len)) / "test_rdna2_core_cache";
    } else {
        cache_dir = std::filesystem::temp_directory_path() / "test_rdna2_core_cache";
    }
#else
    cache_dir = std::filesystem::temp_directory_path() / "test_rdna2_core_cache";
#endif
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);

    ShaderCache cache;
    cache.SetCacheDirectory(cache_dir.string());
    cache.ResetStats();

    const uint8_t sample_in[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const ShaderCacheKey k1 = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_5", 0x6500u);
    const ShaderCacheKey k1_dup = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_5", 0x6500u);
    const ShaderCacheKey k2 = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_2", 0x6200u);
    OS_REQUIRE(k1 == k1_dup);
    OS_REQUIRE(k1 != k2);
    OS_REQUIRE(k1.ToHex().size() == 32u);

    // Fake valid-magic DXBC payload for cache round-trip + corruption tests
    std::vector<uint8_t> fake_dxil(64, 0x5Au);
    std::memcpy(fake_dxil.data(), "DXBC", 4);

    CachedShaderResult res{};
    OS_REQUIRE(!cache.TryGet(k1, res));
    OS_REQUIRE(cache.Put(k1, ShaderStage::Compute, "cs_6_5", fake_dxil).ok());

    // Memory hit
    OS_REQUIRE(cache.TryGet(k1, res));
    OS_REQUIRE(res.from_memory_cache && !res.from_disk_cache);
    OS_REQUIRE(res.dxil == fake_dxil);

    // Disk hit after clearing in-memory cache
    cache.ClearMemoryCache();
    OS_REQUIRE(cache.TryGet(k1, res));
    OS_REQUIRE(!res.from_memory_cache && res.from_disk_cache);
    OS_REQUIRE(res.dxil == fake_dxil);

    // Corrupt on-disk cache file -> must gracefully miss & delete corrupt entry
    cache.ClearMemoryCache();
    const std::string disk_file = cache.CacheFilePathForKey(k1);
    OS_REQUIRE(std::filesystem::exists(disk_file, ec));
    {
        std::fstream fs(disk_file, std::ios::binary | std::ios::in | std::ios::out);
        OS_REQUIRE(fs.good());
        fs.seekp(-1, std::ios::end);
        const char bad = static_cast<char>(0xFF);
        fs.write(&bad, 1);
    }
    OS_REQUIRE(!cache.TryGet(k1, res));
    OS_REQUIRE(!std::filesystem::exists(disk_file, ec));

    const auto stats = cache.Stats();
    OS_REQUIRE(stats.memory_hits == 1u);
    OS_REQUIRE(stats.disk_hits == 1u);
    OS_REQUIRE(stats.corrupt_entries == 1u);

    std::filesystem::remove_all(cache_dir, ec);
    std::printf("[PASS] Core 4: ShaderCache 128-bit hash + atomic disk hit + corrupt-file recovery\n");
}

void TestRdna2IsaCatchesCore() {
    using namespace openscarlett;

    // 1. Catch 1: CPU reference interpreter zero-initializes LDS at dispatch start
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        // Read unwritten LDS slot directly into v2, then atomic-add 1 into unwritten slot at byte 256
        a.EmitDs(Rdna2Opcode::DS_READ_B32, 2, 1);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(256u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(1));
        a.EmitDs(Rdna2Opcode::DS_ADD_RTN_U32, 5, 3, 4);
        a.EmitSopp(Rdna2Opcode::S_BARRIER);
        a.EmitDs(Rdna2Opcode::DS_READ_B32, 6, 3);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 2, 1, 4, 0);    // out[0..31]  = 0
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 128);  // out[32..63] = 32
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2CpuExecState cpu{};
        cpu.uav1.assign(64, 0xDEADu);
        OS_REQUIRE(ExecuteRdna2BlobCpu(a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32), cpu).ok());
        for (uint32_t i = 0; i < 32u; ++i) {
            OS_REQUIRE(cpu.uav1[i] == 0u);
            OS_REQUIRE(cpu.uav1[32 + i] == 32u);
        }
    }

    // 2. Catch 2: VOP2 0x36/0x37/0x38 fused f16 + FMA vs unfused denormal-flushing MAD
    {
        Rdna2Assembler a;
        // half(1.5)=0x3E00, half(2.0)=0x4000, half(0.5)=0x3800 -> fma(1.5, 2.0, 0.5) = 3.5 (0x4300)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x3E00u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x4000u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(0x3800u));
        a.EmitVop2(Rdna2Opcode::V_FMAC_F16, 3, Rdna2Operand::Vgpr(1), 2);               // 0x36: 1.5*2.0 + 0.5 = 3.5 (0x4300)
        a.EmitVop2Literal(Rdna2Opcode::V_FMAMK_F16, 4, Rdna2Operand::Vgpr(1), 2, 0x4000u);  // 0x37: 1.5*2.0 + 2.0 = 5.0 (0x4500)
        a.EmitVop2Literal(Rdna2Opcode::V_FMAAK_F16, 5, Rdna2Operand::Vgpr(1), 2, 0x3C00u);  // 0x38: 1.5*2.0 + 1.0 = 4.0 (0x4400)
        a.EmitVop3(Rdna2Opcode::V_FMA_F16, 6, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2), Rdna2Operand::LiteralU32(0x3C00u));

        // Rounding cancellation vector where FMA != MAD:
        // a = 1 + 2^-13 (0x3F800400), b = 1 - 2^-13 (0x3F7FF800), c = -1.0f (0xBF800000)
        // FMA = -2^-26 (0xB2800000), MAD = 0.0f (0x00000000)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::LiteralU32(0x3F800400u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(0x3F7FF800u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralU32(0xBF800000u));
        a.EmitVop3(Rdna2Opcode::V_FMA_F32, 13, Rdna2Operand::Vgpr(10), Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12));
        a.EmitVop3(Rdna2Opcode::V_MAD_F32, 14, Rdna2Operand::Vgpr(10), Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12));

        // Denormal vector where FMA != MAD:
        // a = 2^-127 (subnormal 0x00400000), b = 4.0f, c = 0.0f
        // FMA = 2^-125 (0x01000000), MAD = 0.0f (0x00000000)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::LiteralU32(0x00400000u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::InlineFloat(4.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop3(Rdna2Opcode::V_FMA_F32, 18, Rdna2Operand::Vgpr(15), Rdna2Operand::Vgpr(16), Rdna2Operand::Vgpr(17));
        a.EmitVop3(Rdna2Opcode::V_MAD_F32, 19, Rdna2Operand::Vgpr(15), Rdna2Operand::Vgpr(16), Rdna2Operand::Vgpr(17));
        // V_MAC_F32, V_MADMK_F32, V_MADAK_F32
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralU32(0xBF800000u));
        a.EmitVop2(Rdna2Opcode::V_MAC_F32, 20, Rdna2Operand::Vgpr(10), 11);
        a.EmitVop2Literal(Rdna2Opcode::V_MADMK_F32, 21, Rdna2Operand::Vgpr(10), 12, 0x3F7FF800u);
        a.EmitVop2Literal(Rdna2Opcode::V_MADAK_F32, 22, Rdna2Operand::Vgpr(10), 11, 0xBF800000u);

        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 25, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 25, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 25, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 25, 4, 12);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 25, 4, 16);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 25, 4, 20);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 18, 25, 4, 24);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 19, 25, 4, 28);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20, 25, 4, 32);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 21, 25, 4, 36);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 22, 25, 4, 40);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());
        bool saw_36 = false, saw_37 = false, saw_38 = false, saw_fma16 = false;
        for (const auto& inst : insns) {
            if (inst.opcode == Rdna2Opcode::V_FMAC_F16) {
                OS_REQUIRE(inst.encoding == Rdna2Encoding::VOP2 && inst.raw_opcode == 0x36u);
                OS_REQUIRE(inst.operand_width == Rdna2OperandWidth::Bits16 && inst.data_kind == Rdna2DataKind::Float);
                saw_36 = true;
            } else if (inst.opcode == Rdna2Opcode::V_FMAMK_F16) {
                OS_REQUIRE(inst.encoding == Rdna2Encoding::VOP2 && inst.raw_opcode == 0x37u && inst.word_count == 2u);
                saw_37 = true;
            } else if (inst.opcode == Rdna2Opcode::V_FMAAK_F16) {
                OS_REQUIRE(inst.encoding == Rdna2Encoding::VOP2 && inst.raw_opcode == 0x38u && inst.word_count == 2u);
                saw_38 = true;
            } else if (inst.opcode == Rdna2Opcode::V_FMA_F16) {
                OS_REQUIRE(IsVop3Float16Instruction(inst));
                saw_fma16 = true;
            }
        }
        OS_REQUIRE(saw_36 && saw_37 && saw_38 && saw_fma16);

        // Verify IR constant folding also preserves FMA != MAD
        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1;
        c.code_words = a.Words();
        Rdna2IrModule ir{};
        OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
        Rdna2IrOptStats opt{};
        OS_REQUIRE(OptimizeRdna2Ir(ir, &opt).ok());
        OS_REQUIRE(opt.folded_constants >= 8u);

        Rdna2CpuExecState cpu{};
        cpu.uav1.assign(16, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(c, insns, cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 0x4300u);       // v_fmac_f16: 3.5
        OS_REQUIRE(cpu.uav1[1] == 0x4500u);       // v_fmamk_f16: 5.0
        OS_REQUIRE(cpu.uav1[2] == 0x4400u);       // v_fmaak_f16: 4.0
        OS_REQUIRE(cpu.uav1[3] == 0x4400u);       // v_fma_f16: 4.0
        OS_REQUIRE(cpu.uav1[4] == 0xB2800000u);   // v_fma_f32: -2^-26
        OS_REQUIRE(cpu.uav1[5] == 0x00000000u);   // v_mad_f32: 0.0 (unfused!)
        OS_REQUIRE(cpu.uav1[4] != cpu.uav1[5]);   // FMA != MAD!
        OS_REQUIRE(cpu.uav1[6] == 0x01000000u);   // v_fma_f32 denorm preserved -> 2^-125
        OS_REQUIRE(cpu.uav1[7] == 0x00000000u);   // v_mad_f32 denorm flushed -> 0.0
        OS_REQUIRE(cpu.uav1[6] != cpu.uav1[7]);   // FMA != MAD!
        OS_REQUIRE(cpu.uav1[8] == 0x00000000u);   // v_mac_f32 unfused
        OS_REQUIRE(cpu.uav1[9] == 0x00000000u);   // v_madmk_f32 unfused
        OS_REQUIRE(cpu.uav1[10] == 0x00000000u);  // v_madak_f32 unfused
    }

    // 3. Catch 3: 16-bit integer VOP3 structural classification (no _e64 string checks) + s_bcnt1_i32_b32 vs b64
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x1234FFF0u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x56780020u));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(0x00000005u));
        a.EmitVop3(Rdna2Opcode::V_ADD_NC_U16, 4, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2));      // (0xFFF0+0x0020)&0xFFFF = 0x0010
        a.EmitVop3(Rdna2Opcode::V_SUB_NC_U16, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(1));      // (0x0020-0xFFF0)&0xFFFF = 0x0030
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U16, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));      // 0x20 * 5 = 0x00A0
        a.EmitVop3(Rdna2Opcode::V_MAD_U16, 7, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(7));  // 0xA7
        a.EmitVop3(Rdna2Opcode::V_MIN_I16, 8, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2));         // min(-16, 32) = -16 (0xFFF0)
        a.EmitVop3(Rdna2Opcode::V_MAX_I16, 9, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2));         // max(-16, 32) = 32 (0x0020)
        a.EmitVop3(Rdna2Opcode::V_LSHLREV_B16, 10, Rdna2Operand::InlineInt(3), Rdna2Operand::Vgpr(2)); // 0x20 << 3 = 0x0100
        a.EmitVop3(Rdna2Opcode::V_LSHRREV_B16, 11, Rdna2Operand::InlineInt(2), Rdna2Operand::Vgpr(2)); // 0x20 >> 2 = 0x0008

        // s_bcnt1_i32_b32 vs s_bcnt1_i32_b64 high-word accuracy:
        // s[8:9] = 0x00000001_80000003 -> b32 = 3, b64 = 4
        // s[12:13] = 0xF0F00001_00000000 -> b32 = 0, b64 = 9
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x80000003u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x00000001u));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 10, Rdna2Operand::Sgpr(8));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 11, Rdna2Operand::Sgpr(8));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 12, Rdna2Operand::LiteralU32(0x00000000u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 13, Rdna2Operand::LiteralU32(0xF0F00001u));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 14, Rdna2Operand::Sgpr(12));
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 15, Rdna2Operand::Sgpr(12));

        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::Sgpr(10));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::Sgpr(11));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::Sgpr(14));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::Sgpr(15));

        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 20, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 20, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 20, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 20, 4, 12);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 20, 4, 16);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 20, 4, 20);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 20, 4, 24);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 20, 4, 28);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 20, 4, 32);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 20, 4, 36);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 20, 4, 40);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 20, 4, 44);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());

        uint32_t vop3_i16_count = 0;
        bool saw_bcnt32 = false, saw_bcnt64 = false;
        for (const auto& inst : insns) {
            if (IsVop3Int16Instruction(inst)) {
                OS_REQUIRE(IsVop3EncodingWord(inst.raw_words[0]));
                OS_REQUIRE(inst.word_count >= 2u);
                OS_REQUIRE(inst.operand_width == Rdna2OperandWidth::Bits16);
                // Strictly classified by encoding/metadata, never by _e64 suffix
                OS_REQUIRE(inst.mnemonic.find("_e64") == std::string::npos);
                ++vop3_i16_count;
            }
            if (inst.opcode == Rdna2Opcode::S_BCNT1_I32_B32) {
                OS_REQUIRE(inst.raw_opcode == 0x08u && inst.operand_width == Rdna2OperandWidth::Bits32);
                saw_bcnt32 = true;
            }
            if (inst.opcode == Rdna2Opcode::S_BCNT1_I32_B64) {
                OS_REQUIRE(inst.raw_opcode == 0x09u && inst.operand_width == Rdna2OperandWidth::Bits64);
                saw_bcnt64 = true;
            }
        }
        OS_REQUIRE(vop3_i16_count == 8u && saw_bcnt32 && saw_bcnt64);

        Rdna2CpuExecState cpu{};
        cpu.uav1.assign(16, 0u);
        OS_REQUIRE(ExecuteRdna2BlobCpu(a.BuildXs2(ShaderStage::Compute, 1, 1, 1, 20, 24, 32), cpu).ok());
        OS_REQUIRE(cpu.uav1[0] == 0x0010u);
        OS_REQUIRE(cpu.uav1[1] == 0x0030u);
        OS_REQUIRE(cpu.uav1[2] == 0x00A0u);
        OS_REQUIRE(cpu.uav1[3] == 0x00A7u);
        OS_REQUIRE(cpu.uav1[4] == 0xFFF0u);
        OS_REQUIRE(cpu.uav1[5] == 0x0020u);
        OS_REQUIRE(cpu.uav1[6] == 0x0100u);
        OS_REQUIRE(cpu.uav1[7] == 0x0008u);
        OS_REQUIRE(cpu.uav1[8] == 3u);   // s_bcnt1_i32_b32(0x80000003) == 3
        OS_REQUIRE(cpu.uav1[9] == 4u);   // s_bcnt1_i32_b64(0x00000001_80000003) == 4
        OS_REQUIRE(cpu.uav1[10] == 0u);  // s_bcnt1_i32_b32(0x00000000) == 0
        OS_REQUIRE(cpu.uav1[11] == 9u);  // s_bcnt1_i32_b64(0xF0F00001_00000000) == 9
    }

    // 4. Catch 4A: Pixel shader implicit-LOD IMAGE_SAMPLE builds ImageSampleImplicit + quad derivs
    {
        Rdna2Assembler ps;
        ps.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 0, 0, 8, 0xFu, 1u, false);
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 6, 7, 0xFu, true, false, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(ps.Words(), insns).ok());
        Xs2ShaderContainer c{};
        c.stage = ShaderStage::Pixel;
        c.threadgroup_x = 4;
        c.code_words = ps.Words();
        Rdna2IrModule ir{};
        OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
        OS_REQUIRE(ir.uses_pixel_quad_derivatives);
    }

    std::printf("[PASS] Core 5: RDNA2 ISA Catches (LDS zero-init, VOP2 0x36..0x38 f16, FMA!=MAD, VOP3 i16 metadata, bcnt32/64, PS quad derivs)\n");
}

void TestPostTosSdwaAndOpcodes() {
    using namespace openscarlett;

    const std::string dxc_path = ResolvePortableDxcPath();
    std::error_code fs_ec;
    const bool has_spirv_val = std::filesystem::exists(kDefaultSpirvValPath, fs_ec);

    auto verify_lift = [&](const Rdna2IrModule& ir, const char* label, std::string* out_hlsl_opt = nullptr) {
        std::vector<uint32_t> spv;
        OS_REQUIRE(EmitSpirv(ir, spv).ok());
        OS_REQUIRE(ValidateSpirvBinary(spv).ok());
        if (has_spirv_val) {
            std::string spv_diag;
            auto vst = ValidateSpirvExternal(spv, spv_diag, kDefaultSpirvValPath, "vulkan1.2");
            if (!vst.ok()) {
                std::fprintf(stderr, "[FAIL] spirv-val for %s: %s\n", label, spv_diag.c_str());
            }
            OS_REQUIRE(vst.ok());
        }
        std::string hlsl, target;
        OS_REQUIRE(EmitHlsl(ir, hlsl, target, true).ok());
        if (out_hlsl_opt != nullptr) {
            *out_hlsl_opt = hlsl;
        }
        if (!dxc_path.empty()) {
            std::vector<uint8_t> dxil;
            std::string dxc_diag;
            auto cst = CompileHlslWithDxc(hlsl, target, "main", dxil, dxc_diag, dxc_path);
            if (!cst.ok()) {
                std::fprintf(stderr, "[FAIL] DXC for %s (%s): %s\n--- HLSL ---\n%s\n",
                             label, target.c_str(), dxc_diag.c_str(), hlsl.c_str());
            }
            OS_REQUIRE(cst.ok());
            OS_REQUIRE(!dxil.empty());
        }
    };

    // -----------------------------------------------------------------------
    // 6A. GFX10 SDWA (src0 == 0xF9): Scaleform UI PS WORD_0 / WORD_1 + VOP1/VOP2/VOPC SDWA
    // -----------------------------------------------------------------------
    {
        Rdna2Assembler setup;
        // v2 = 0x00EA00D5 -> WORD_0 & 63 = 0xD5 & 63 = 21, WORD_1 & 63 = 0xEA & 63 = 42
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x00EA00D5u));
        // v3 = packed half(+1.5f = 0x3E00, -3.0f = 0xC200)
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3E00u, 0xC200u)));
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralU32(0x11223344u));
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralU32(0x000000ABu));
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::LiteralU32(0x00000085u));
        setup.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::LiteralU32(0x000000FEu));

        std::vector<uint32_t> words = setup.Words();
        // Exact Scaleform UI PS raw SDWA words:
        // 361004f9 048606bf = v_and_b32_sdwa v8, 63, v2 dst_sel:DWORD src0_sel:DWORD src1_sel:WORD_0
        words.push_back(0x361004F9u);
        words.push_back(0x048606BFu);
        // 361204f9 058606bf = v_and_b32_sdwa v9, 63, v2 dst_sel:DWORD src0_sel:DWORD src1_sel:WORD_1
        words.push_back(0x361204F9u);
        words.push_back(0x058606BFu);
        // Retail v_cvt_f32_f16_sdwa (w0=0x7E0E16F9): v7 = f16tof32(v3.WORD_0) = +1.5f
        words.push_back(0x7E0E16F9u);
        words.push_back(0x00040603u);
        // v10 = f16tof32(|v3.WORD_1|) * 2.0f (omod=1, src0_abs=1, src0_sel=WORD_1) = 6.0f
        words.push_back(0x7E1416F9u);
        words.push_back(0x00254603u);
        // v11 = clamp(-f16tof32(v3.WORD_0)) (clamp=1, src0_neg=1, src0_sel=WORD_0) = 0.0f
        words.push_back(0x7E1616F9u);
        words.push_back(0x00142603u);

        Rdna2Assembler tail;
        // PRESERVE (dst_unused=2, dst_sel=BYTE_2=2, src0_sel=BYTE_0=0): v12[23:16] = 0xAB -> 0x11AB3344
        tail.EmitVop1Sdwa(Rdna2Opcode::V_MOV_B32, 12, (2u << 11) | (2u << 8) | (0u << 16) | 13u);
        // SEXT (dst_unused=1, dst_sel=BYTE_1=1, src0_sel=BYTE_0=0): v14(0x85) -> 0xFFFF8500 in v16
        tail.EmitVop1Sdwa(Rdna2Opcode::V_MOV_B32, 16, (1u << 11) | (1u << 8) | (0u << 16) | 14u);
        // src0_sext=1 on BYTE_0 (0xFE -> -2) + inline 10 (s1=1, vsrc1=138) -> 8 in v17
        tail.EmitVop2Sdwa(Rdna2Opcode::V_ADD_NC_U32, 17, 138u,
                          (1u << 31) | (6u << 24) | (1u << 19) | (0u << 16) | (6u << 8) | 15u);
        // VOPC SDWA with s1=1: compare v9 (42, WORD_0) == inline 42 (128+42=170) -> VCC = 1 -> v18 = 77
        tail.EmitVopcSdwa(Rdna2Opcode::V_CMP_EQ_U32, 170u,
                          (1u << 31) | (6u << 24) | (4u << 16) | 9u);
        tail.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 18, Rdna2Operand::InlineInt(0), 9);

        tail.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::InlineInt(0));
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 20, 0, 0);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 20, 0, 4);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 20, 0, 8);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 20, 0, 12);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 20, 0, 16);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 20, 0, 20);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 16, 20, 0, 24);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 20, 0, 28);
        tail.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 18, 20, 0, 32);
        tail.EmitSopp(Rdna2Opcode::S_ENDPGM);
        words.insert(words.end(), tail.Words().begin(), tail.Words().end());

        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(words, insns).ok());
        bool saw_scaleform_w0 = false, saw_scaleform_w1 = false, saw_cvt_f16_sdwa = false;
        for (const auto& ins : insns) {
            if (ins.has_sdwa && ins.opcode == Rdna2Opcode::V_AND_B32) {
                if (ins.disasm.find("src1_sel:WORD_0") != std::string::npos) saw_scaleform_w0 = true;
                if (ins.disasm.find("src1_sel:WORD_1") != std::string::npos) saw_scaleform_w1 = true;
            }
            if (ins.has_sdwa && ins.opcode == Rdna2Opcode::V_CVT_F32_F16) {
                saw_cvt_f16_sdwa = true;
            }
        }
        OS_REQUIRE(saw_scaleform_w0 && saw_scaleform_w1 && saw_cvt_f16_sdwa);

        Xs2ShaderContainer c{};
        c.version = kXs2ContainerVersion;
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1;
        c.sgpr_count = 16;
        c.vgpr_count = 24;
        c.wave_size = 32;
        c.entry_offset = kXs2HeaderSizeBytes;
        c.code_words = words;

        Rdna2IrModule ir{};
        OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
        OS_REQUIRE(OptimizeRdna2Ir(ir).ok());
        verify_lift(ir, "SDWA_Scaleform_And_Subdword");

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(16, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(c, insns, cpu).ok());
        OS_REQUIRE(cpu.uav0[0] == 21u);
        OS_REQUIRE(cpu.uav0[1] == 42u);
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[2]) - 1.5f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[3]) - 6.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[4]) - 0.0f) < 1e-5f);
        OS_REQUIRE(cpu.uav0[5] == 0x11AB3344u);
        OS_REQUIRE(cpu.uav0[6] == 0xFFFF8500u);
        OS_REQUIRE(cpu.uav0[7] == 8u);
        OS_REQUIRE(cpu.uav0[8] == 42u);
    }

    // -----------------------------------------------------------------------
    // 6B. All 16 Post-TOS RDNA2 Opcode Families (SOP2, SOPC, SOPK, SOP1, VOP1, VOP3, DS, MUBUF)
    // -----------------------------------------------------------------------
    {
        Rdna2Assembler a;
        // SOP2: s_pack_ll/lh/hh_b32_b16, s_mul_hi_u32, s_mul_hi_i32
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::LiteralU32(0x11112222u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 5, Rdna2Operand::LiteralU32(0x33334444u));
        a.EmitSop2(Rdna2Opcode::S_PACK_LL_B32_B16, 6, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
        a.EmitSop2(Rdna2Opcode::S_PACK_LH_B32_B16, 7, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
        a.EmitSop2(Rdna2Opcode::S_PACK_HH_B32_B16, 8, Rdna2Operand::Sgpr(4), Rdna2Operand::Sgpr(5));
        a.EmitSop2(Rdna2Opcode::S_MUL_HI_U32, 9, Rdna2Operand::LiteralU32(0x80000000u), Rdna2Operand::InlineInt(4));
        a.EmitSop2(Rdna2Opcode::S_MUL_HI_I32, 10, Rdna2Operand::InlineInt(-4), Rdna2Operand::LiteralU32(0x40000000u));

        // SOPC: s_bitcmp0/1_b32, s_bitcmp0/1_b64 + SOPK: s_cmovk_i32, s_cmpk_*_i32, s_cmpk_*_u32
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 12, Rdna2Operand::LiteralU32(0x00000010u));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 13, Rdna2Operand::LiteralU32(0x00002000u));  // bit 45 set in s[12:13]
        a.EmitSopc(Rdna2Opcode::S_BITCMP1_B32, Rdna2Operand::Sgpr(12), Rdna2Operand::InlineInt(4));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 14, Rdna2Operand::InlineInt(0));
        a.EmitSopk(Rdna2Opcode::S_CMOVK_I32, 14, static_cast<int16_t>(-123));            // SCC=1 -> s14 = -123
        a.EmitSopc(Rdna2Opcode::S_BITCMP0_B64, Rdna2Operand::Sgpr(12), Rdna2Operand::InlineInt(45));
        a.EmitSopk(Rdna2Opcode::S_CMOVK_I32, 14, static_cast<int16_t>(999));             // SCC=0 -> s14 stays -123
        a.EmitSopc(Rdna2Opcode::S_BITCMP1_B64, Rdna2Operand::Sgpr(12), Rdna2Operand::InlineInt(45));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 15, Rdna2Operand::InlineInt(0));
        a.EmitSopk(Rdna2Opcode::S_CMOVK_I32, 15, static_cast<int16_t>(77));              // SCC=1 -> s15 = 77

        // SOPK signed & zero-extended unsigned compares (must NOT clobber sdst!)
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 16, Rdna2Operand::LiteralU32(0x0000FFFEu));   // +65534
        a.EmitSopk(Rdna2Opcode::S_CMPK_EQ_U32, 16, static_cast<int16_t>(0xFFFEu));       // 65534 == 65534 -> SCC=1
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 17, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_CMOV_B32, 17, Rdna2Operand::Sgpr(16));                 // s17 = 0xFFFE (proves s16 intact!)
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 18, Rdna2Operand::InlineInt(-5));
        a.EmitSopk(Rdna2Opcode::S_CMPK_LT_I32, 18, static_cast<int16_t>(-2));            // -5 < -2 -> SCC=1
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 20, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 21, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_CMOV_B64, 20, Rdna2Operand::Sgpr(12));                 // s[20:21] = s[12:13]

        // SOP1: s_brev_b64, s_bcnt0_i32_b32/b64, s_ff0/ff1_i32_b32/b64, s_sext_i32_i8/i16, s_bitset0/1_b32, s_abs_i32, s_bitreplicate_b64_b32
        a.EmitSop1(Rdna2Opcode::S_BREV_B64, 22, Rdna2Operand::Sgpr(12));
        a.EmitSop1(Rdna2Opcode::S_BCNT0_I32_B32, 24, Rdna2Operand::LiteralU32(0xF0F0F0F0u));      // 16
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 26, Rdna2Operand::LiteralU32(0xFFFFFFFFu));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 27, Rdna2Operand::LiteralU32(0x0000FFFFu));
        a.EmitSop1(Rdna2Opcode::S_BCNT0_I32_B64, 25, Rdna2Operand::Sgpr(26));                     // 16
        a.EmitSop1(Rdna2Opcode::S_FF0_I32_B32, 28, Rdna2Operand::LiteralU32(0x0000000Fu));        // 4
        a.EmitSop1(Rdna2Opcode::S_FF0_I32_B64, 29, Rdna2Operand::Sgpr(26));                       // 48
        a.EmitSop1(Rdna2Opcode::S_FF1_I32_B32, 30, Rdna2Operand::LiteralU32(0x00080000u));        // 19
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 32, Rdna2Operand::InlineInt(0));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 33, Rdna2Operand::LiteralU32(0x00000010u));
        a.EmitSop1(Rdna2Opcode::S_FF1_I32_B64, 31, Rdna2Operand::Sgpr(32));                       // 36
        a.EmitSop1(Rdna2Opcode::S_SEXT_I32_I8, 34, Rdna2Operand::LiteralU32(0x12345680u));        // 0xFFFFFF80
        a.EmitSop1(Rdna2Opcode::S_SEXT_I32_I16, 35, Rdna2Operand::LiteralU32(0x12348001u));       // 0xFFFF8001
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 36, Rdna2Operand::LiteralU32(0x000000FFu));
        a.EmitSop1(Rdna2Opcode::S_BITSET0_B32, 36, Rdna2Operand::InlineInt(4));                   // 0xEF
        a.EmitSop1(Rdna2Opcode::S_BITSET1_B32, 36, Rdna2Operand::InlineInt(12));                  // 0x10EF
        a.EmitSop1(Rdna2Opcode::S_ABS_I32, 37, Rdna2Operand::InlineInt(-16));                     // 16
        a.EmitSop1(Rdna2Opcode::S_BITREPLICATE_B64_B32, 38, Rdna2Operand::LiteralU32(0x80010005u)); // lo=0x33, hi=0xC0000003

        // VOP1 + VOP3 + DS + MUBUF
        a.EmitVop1(Rdna2Opcode::V_CVT_F16_F32, 1, Rdna2Operand::LiteralU32(FloatToBits(-2.5f)));  // 0xC100
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_F16, 2, Rdna2Operand::Vgpr(1));                         // -2.5f
        a.EmitVop1(Rdna2Opcode::V_CVT_RPI_I32_F32, 3, Rdna2Operand::LiteralU32(FloatToBits(2.5f)));  // 3
        a.EmitVop1(Rdna2Opcode::V_CVT_FLR_I32_F32, 4, Rdna2Operand::LiteralU32(FloatToBits(-2.1f))); // -3
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(0xC8643219u));
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_UBYTE0, 6, Rdna2Operand::Vgpr(5));                      // 25.0f
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_UBYTE1, 7, Rdna2Operand::Vgpr(5));                      // 50.0f
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_UBYTE2, 8, Rdna2Operand::Vgpr(5));                      // 100.0f
        a.EmitVop1(Rdna2Opcode::V_CVT_F32_UBYTE3, 9, Rdna2Operand::Vgpr(5));                      // 200.0f
        a.EmitVop1(Rdna2Opcode::V_RCP_IFLAG_F32, 10, Rdna2Operand::InlineFloat(4.0f));            // 0.25f
        a.EmitVop1(Rdna2Opcode::V_NOT_B32, 11, Rdna2Operand::LiteralU32(0x0F0F0F0Fu));            // 0xF0F0F0F0
        a.EmitVop1(Rdna2Opcode::V_BFREV_B32, 12, Rdna2Operand::LiteralU32(0x80000003u));          // 0xC0000001
        a.EmitVop1(Rdna2Opcode::V_FFBH_U32, 13, Rdna2Operand::LiteralU32(0x00100000u));           // 11
        a.EmitVop1(Rdna2Opcode::V_FFBL_B32, 14, Rdna2Operand::LiteralU32(0x00100000u));           // 20
        a.EmitVop1(Rdna2Opcode::V_FFBH_I32, 15, Rdna2Operand::LiteralU32(0xFFFFFFF0u));           // 28
        a.EmitVop3(Rdna2Opcode::V_BCNT_U32_B32, 16, Rdna2Operand::LiteralU32(0xF0F00003u), Rdna2Operand::InlineInt(10)); // 20
        a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F16, 17,
                   Rdna2Operand::LiteralU32(0x3C00u), Rdna2Operand::LiteralU32(0x3C00u));         // 1.0, 1.0 -> 0x7FFF7FFF

        // DS_WRITE2_B32 / DS_READ2_B32 (stride 4) & DS_WRITE2ST64_B32 / DS_READ2ST64_B32 (stride 256)
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 18, Rdna2Operand::InlineInt(0));
        a.EmitDs(Rdna2Opcode::DS_WRITE2_B32, 0, 18, 11, 12, static_cast<uint16_t>((5u << 8) | 2u));
        a.EmitDs(Rdna2Opcode::DS_READ2_B32, 20, 18, 0, 0, static_cast<uint16_t>((5u << 8) | 2u));
        a.EmitDs(Rdna2Opcode::DS_WRITE2ST64_B32, 0, 18, 13, 14, static_cast<uint16_t>((2u << 8) | 1u));
        a.EmitDs(Rdna2Opcode::DS_READ2ST64_B32, 22, 18, 0, 0, static_cast<uint16_t>((2u << 8) | 1u));

        // Move scalar results into VGPRs and store via BUFFER_STORE_FORMAT_XYZW + BUFFER_STORE_DWORD
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::Sgpr(6));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::Sgpr(7));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::Sgpr(8));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::Sgpr(9));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_FORMAT_XYZW, 24, 18, 0, 0);

        const uint8_t scalar_check_sgprs[] = {
            10, 14, 15, 17, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31, 34, 35, 36, 37, 38, 39
        };
        for (size_t i = 0; i < sizeof(scalar_check_sgprs); ++i) {
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 28, Rdna2Operand::Sgpr(scalar_check_sgprs[i]));
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 28, 18, 0, static_cast<uint16_t>(16u + i * 4u));
        }
        const uint8_t vec_check_vgprs[] = {
            1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23
        };
        for (size_t i = 0; i < sizeof(vec_check_vgprs); ++i) {
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, vec_check_vgprs[i], 18, 0, static_cast<uint16_t>(96u + i * 4u));
        }

        // Also include retail raw encodings:
        // - 0xBEFE3BFF, 0xFFFFFFFF = s_bitreplicate_b64_b32 exec, 0xffffffff
        // - 0xD7650001, 0x0001007E = v_mbcnt_lo_u32_b32 v1, exec_lo, 0
        // - 0xD7660001, 0x0002027F = v_mbcnt_hi_u32_b32 v1, exec_hi, v1
        std::vector<uint32_t> words = a.Words();
        words.push_back(0xBEFE3BFFu);
        words.push_back(0xFFFFFFFFu);
        words.push_back(0xD7650001u);
        words.push_back(0x0001007Eu);
        words.push_back(0xD7660001u);
        words.push_back(0x0002027Fu);
        Rdna2Assembler end_a;
        end_a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_FORMAT_X, 1, 18, 0, 176u);
        end_a.EmitSopp(Rdna2Opcode::S_ENDPGM);
        words.insert(words.end(), end_a.Words().begin(), end_a.Words().end());

        std::vector<Rdna2Instruction> insns;
        OS_REQUIRE(DisassembleRdna2(words, insns).ok());

        // Verify raw 0xE01C2000 (buffer_store_format_xyzw idxen) also decodes cleanly
        std::vector<Rdna2Instruction> raw_mubuf;
        OS_REQUIRE(DisassembleRdna2({0xE01C2000u, 0x80000100u}, raw_mubuf).ok());
        OS_REQUIRE(raw_mubuf.size() == 1u && raw_mubuf[0].opcode == Rdna2Opcode::BUFFER_STORE_FORMAT_XYZW);

        Xs2ShaderContainer c{};
        c.version = kXs2ContainerVersion;
        c.stage = ShaderStage::Compute;
        c.threadgroup_x = 1;
        c.sgpr_count = 48;
        c.vgpr_count = 32;
        c.wave_size = 32;
        c.entry_offset = kXs2HeaderSizeBytes;
        c.code_words = words;

        Rdna2IrModule ir{};
        OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
        OS_REQUIRE(OptimizeRdna2Ir(ir).ok());
        verify_lift(ir, "PostTos_All16_Families");

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(64, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(c, insns, cpu).ok());
        // SOP2 via BUFFER_STORE_FORMAT_XYZW
        OS_REQUIRE(cpu.uav0[0] == 0x44442222u);
        OS_REQUIRE(cpu.uav0[1] == 0x33332222u);
        OS_REQUIRE(cpu.uav0[2] == 0x33331111u);
        OS_REQUIRE(cpu.uav0[3] == 2u);
        // Scalar results [4..23]
        OS_REQUIRE(cpu.uav0[4] == 0xFFFFFFFFu);                     // s_mul_hi_i32(-4, 0x40000000) = -1
        OS_REQUIRE(static_cast<int32_t>(cpu.uav0[5]) == -123);      // s_cmovk_i32
        OS_REQUIRE(cpu.uav0[6] == 77u);                             // s_bitcmp1_b64 + s_cmovk_i32
        OS_REQUIRE(cpu.uav0[7] == 0x0000FFFEu);                     // s_cmpk_eq_u32 zero-ext + s16 preserved
        OS_REQUIRE(cpu.uav0[8] == 0x00000010u && cpu.uav0[9] == 0x00002000u); // s_cmov_b64
        OS_REQUIRE(cpu.uav0[10] == 0x00040000u && cpu.uav0[11] == 0x08000000u); // s_brev_b64({0x10, 0x2000})
        OS_REQUIRE(cpu.uav0[12] == 16u && cpu.uav0[13] == 16u);     // s_bcnt0_i32_b32 / b64
        OS_REQUIRE(cpu.uav0[14] == 4u && cpu.uav0[15] == 48u);      // s_ff0_i32_b32 / b64
        OS_REQUIRE(cpu.uav0[16] == 19u && cpu.uav0[17] == 36u);     // s_ff1_i32_b32 / b64
        OS_REQUIRE(cpu.uav0[18] == 0xFFFFFF80u && cpu.uav0[19] == 0xFFFF8001u); // s_sext_i32_i8 / i16
        OS_REQUIRE(cpu.uav0[20] == 0x000010EFu && cpu.uav0[21] == 16u); // s_bitset0/1_b32, s_abs_i32
        OS_REQUIRE(cpu.uav0[22] == 0x00000033u && cpu.uav0[23] == 0xC0000003u); // s_bitreplicate_b64_b32
        // Vector + DS results [24..43]
        OS_REQUIRE(cpu.uav0[24] == 0x0000C100u);
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[25]) - (-2.5f)) < 1e-5f);
        OS_REQUIRE(static_cast<int32_t>(cpu.uav0[26]) == 3);
        OS_REQUIRE(static_cast<int32_t>(cpu.uav0[27]) == -3);
        OS_REQUIRE(BitsToFloat(cpu.uav0[28]) == 25.0f && BitsToFloat(cpu.uav0[29]) == 50.0f);
        OS_REQUIRE(BitsToFloat(cpu.uav0[30]) == 100.0f && BitsToFloat(cpu.uav0[31]) == 200.0f);
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[32]) - 0.25f) < 1e-5f);
        OS_REQUIRE(cpu.uav0[33] == 0xF0F0F0F0u && cpu.uav0[34] == 0xC0000001u);
        OS_REQUIRE(cpu.uav0[35] == 11u && cpu.uav0[36] == 20u && cpu.uav0[37] == 28u);
        OS_REQUIRE(cpu.uav0[38] == 20u);
        OS_REQUIRE(cpu.uav0[39] == 0x7FFF7FFFu);
        OS_REQUIRE(cpu.uav0[40] == 0xF0F0F0F0u && cpu.uav0[41] == 0xC0000001u); // DS_READ2_B32
        OS_REQUIRE(cpu.uav0[42] == 11u && cpu.uav0[43] == 20u);                 // DS_READ2ST64_B32
        OS_REQUIRE(cpu.uav0[44] == 0u);  // lane 0 v_mbcnt_lo/hi = 0
    }

    // -----------------------------------------------------------------------
    // 6C. Retail DXC HLSL Emitter Regression Tests (space2 _in0/_tex0 overlap & dead-branch SSA _v58)
    // -----------------------------------------------------------------------
    {
        // Case 1: _tex0 at register(t0, space2) + _in0 SRV colliding at register(t0, space2) -> _in0 moved to space98
        Rdna2Assembler ps;
        ps.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 4, 0, 0);
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 0, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(0.5f));
        ps.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 0, 0, 8, 0xFu, 1u, false);
        ps.EmitVop2(Rdna2Opcode::V_ADD_F32, 4, Rdna2Operand::Sgpr(4), 4);
        ps.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 4, 5, 6, 7, 0xFu, true, false, true);
        ps.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> ps_insns;
        OS_REQUIRE(DisassembleRdna2(ps.Words(), ps_insns).ok());
        Xs2ShaderContainer ps_c{};
        ps_c.version = kXs2ContainerVersion;
        ps_c.stage = ShaderStage::Pixel;
        ps_c.threadgroup_x = 4;
        ps_c.sgpr_count = 16;
        ps_c.vgpr_count = 12;
        ps_c.wave_size = 32;
        ps_c.entry_offset = kXs2HeaderSizeBytes;
        ps_c.code_words = ps.Words();

        StageContractTable sc{};
        sc.psv0_bindings.enabled = true;
        sc.psv0_bindings.in0_is_srv = true;
        sc.psv0_bindings.in0_reg = 0u;
        sc.psv0_bindings.in0_space = 2u;
        sc.psv0_bindings.has_tex0 = true;
        sc.psv0_bindings.tex0_reg = 0u;
        sc.psv0_bindings.tex0_space = 2u;
        sc.psv0_bindings.has_samp0 = true;
        sc.psv0_bindings.samp0_reg = 0u;
        sc.psv0_bindings.samp0_space = 2u;

        Rdna2IrModule ps_ir{};
        OS_REQUIRE(BuildRdna2Ir(ps_c, ps_insns, ps_ir, &sc).ok());
        OS_REQUIRE(OptimizeRdna2Ir(ps_ir).ok());
        std::string ps_hlsl;
        verify_lift(ps_ir, "Retail_HLSL_Space2_In0_Tex0_Overlap", &ps_hlsl);
        OS_REQUIRE(ps_hlsl.find("_in0 : register(t0, space98)") != std::string::npos);

        // Case 2: Unwritten VGPR in bb0 + dead-branch write in bb1 + divergent EXEC-masked write in bb2
        Rdna2Assembler cs;
        cs.EmitSop1(Rdna2Opcode::S_MOV_B32, 4, Rdna2Operand::InlineInt(0));
        cs.EmitSopc(Rdna2Opcode::S_CMP_EQ_U32, Rdna2Operand::Sgpr(4), Rdna2Operand::InlineInt(1)); // constant false SCC
        const uint32_t br_off = cs.EmitSopp(Rdna2Opcode::S_CBRANCH_SCC0, 0);
        // bb1 (unreachable after constant-folding SCC == 0): only definition of v7 before bb2!
        cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(3.0f));
        // bb2: divergent active mask writes v7 (previously emitted `(_v2 ? _v199 : asfloat(_v58))` with undeclared _v58!)
        cs.PatchBranch(br_off, cs.CurrentWordOffset());
        cs.EmitVopc(Rdna2Opcode::V_CMPX_GT_U32, Rdna2Operand::InlineInt(2), 0);
        cs.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(5.0f));
        cs.EmitSop1(Rdna2Opcode::S_MOV_B32, 126, Rdna2Operand::InlineInt(-1));  // restore EXEC_LO = -1
        cs.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(2), 0);
        cs.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 8, 0, 0);
        cs.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<Rdna2Instruction> cs_insns;
        OS_REQUIRE(DisassembleRdna2(cs.Words(), cs_insns).ok());
        Xs2ShaderContainer cs_c{};
        cs_c.version = kXs2ContainerVersion;
        cs_c.stage = ShaderStage::Compute;
        cs_c.threadgroup_x = 4;
        cs_c.sgpr_count = 16;
        cs_c.vgpr_count = 12;
        cs_c.wave_size = 32;
        cs_c.entry_offset = kXs2HeaderSizeBytes;
        cs_c.code_words = cs.Words();

        Rdna2IrModule cs_ir{};
        OS_REQUIRE(BuildRdna2Ir(cs_c, cs_insns, cs_ir).ok());
        OS_REQUIRE(OptimizeRdna2Ir(cs_ir).ok());
        verify_lift(cs_ir, "Retail_HLSL_DeadBranch_Vgpr_Merge");

        Rdna2CpuExecState cpu{};
        cpu.uav0.assign(4, 0u);
        OS_REQUIRE(ExecuteRdna2Cpu(cs_c, cs_insns, cpu).ok());
        OS_REQUIRE(BitsToFloat(cpu.uav0[0]) == 5.0f && BitsToFloat(cpu.uav0[1]) == 5.0f);
        OS_REQUIRE(cpu.uav0[2] == 0u && cpu.uav0[3] == 0u);
    }

    std::printf("[PASS] Core 6: Post-TOS GFX10 SDWA + 16 Opcode Families + Retail DXC HLSL Fixes\n");
}

}  // namespace

int main() {
    TestBasicComputeAndConstFold();
    TestAaaOpcodesEncodeDecodeIrAndCpu();
    TestXboxDxbcContainerParser();
    TestShaderCacheAtomicsAndCorruptRecovery();
    TestRdna2IsaCatchesCore();
    TestPostTosSdwaAndOpcodes();
    std::printf("[PASS] test_rdna2_core: ALL 6 CORE SUITES PASSED!\n");
    return 0;
}
