#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/shader_ir.h"

namespace openscarlett {

using IrValueId = uint32_t;
constexpr IrValueId kInvalidIrValue = 0u;

enum class IrType : uint8_t {
    Void = 0,
    U32,
    I32,
    F32,
    Bool,
};

enum class IrOp : uint16_t {
    ConstU32 = 0,
    ConstF32,
    ThreadIdX,
    StageInput,     // Vertex/Pixel input lane (e.g. PS interpolated PARAM0 channel 0..3)
    LoadRootConst,  // Read 32-bit scalar constant from b0[imm_u32]
    LoadUav,        // Load 32-bit word from uav[imm_u32] at byte offset operands[0]
    StoreUav,       // Store operands[1] to uav[imm_u32] at byte offset operands[0]

    // Texture / Image ops (MIMG)
    ImageSample,        // operands: [u_f32, v_f32, lod_or_dref_f32], imm_u32 = channel (0..3) | (is_cmp << 7) -> f32
    ImageSampleDynamic, // operands: [u_f32, v_f32, lod_or_dref_f32, desc_idx_u32], imm_u32 = ch | (is_cmp << 7) | (array_len << 8)
    ImageSampleImplicit,// operands: [u_f32, v_f32], imm_u32 = channel (0..3) -> f32 (PS helper-lane quad derivatives)
    ImageLoad,          // operands: [x_u32, y_u32], imm_u32 = channel (0..3) -> f32
    ImageLoadMip,       // operands: [x_u32, y_u32, z_or_face_u32, mip_u32], imm_u32 = ch | (dim << 4) -> f32
    ImageStore,         // operands: [x_u32, y_u32, r_f32, g_f32, b_f32, a_f32] -> void
    ImageAtomicCmpSwap, // operands: [x_u32, y_u32, z_or_face_u32, cmp_u32, src_u32], imm_u32 = dim -> prev_u32
    BvhIntersectRay,    // operands: [node_ptr, tmax, ox, oy, oz, dx, dy, dz], imm_u32 = comp (0=t,1=u,2=v,3=hit_id)

    // Workgroup shared memory / LDS ops (DS)
    LdsLoad,            // operands: [byte_addr_u32] -> u32
    LdsStore,           // operands: [byte_addr_u32, val_u32] -> void
    LdsAtomicAdd,       // operands: [byte_addr_u32, val_u32] -> prev_u32

    // Thread-private scratch memory ops (FLAT flat_seg == 1)
    ScratchLoad,        // operands: [byte_addr_u32] -> u32
    ScratchStore,       // operands: [byte_addr_u32, val_u32] -> void

    // Wave / Subgroup ops (wave32 native or wave64-on-wave32 emulation)
    WaveLaneId,             // [] -> lane_id u32 (0..wave_size-1)
    WaveReadFirstLane,      // [val] -> u32
    WaveReadLaneAt,         // [val, lane_idx_u32] -> u32
    WaveBallotLo,           // [pred_bool] -> u32 (lanes 0..31)
    WaveBallotHi,           // [pred_bool] -> u32 (lanes 32..63)
    WaveActiveCountBits64,  // [ballot_lo_u32, ballot_hi_u32] -> u32
    WavePrefixCount64,      // [mask_u32, init_u32], imm_u32=0 (lo32) / 1 (hi32) / 2 (combined [lo, hi]) -> u32
    WavePermLane16,         // [vsrc0, ssrc1, ssrc2], imm_u32=0 (same 16-lane row) / 1 (cross-16 X16) -> u32

    // Mesh & Amplification Shader ops (SM 6.5)
    MeshGlobalThreadId,     // [] -> group * NumThreads + lane (mesh v5 seeding)
    MergedWaveInfo,         // [] -> full packed merged_wave_info in s3
    SetMeshOutputCounts,    // [vert_count_u32, prim_count_u32] -> void
    ExportMeshPrim,         // [i0_u32, i1_u32, i2_u32] (imm_u32=0) or [packed_u32] (imm_u32=1) -> void
    ExportMeshCull,         // [cull_bool] -> void (CullPrimitiveEXT / SV_CullPrimitive)
    DispatchMeshGroups,     // [gx_u32, gy_u32, gz_u32] -> void

    // Floating-point arithmetic
    FAdd,
    FSub,
    FMul,
    FFma,           // Single-rounding fused multiply-add (preserves subnormals)
    FMad,           // Unfused denormal-flushing multiply then add (distinct from FFma)
    FFma16,         // Low-16-bit fused f16 multiply-add, imm_u32 = (op_sel & 7) | (clamp << 16) -> u32 (lo16 f16)
    FMin,
    FMax,
    FRcp,
    FRsq,
    FSqrt,
    FFloor,
    FCeil,
    FFract,
    FDot2,          // V_DOT2_F32_F16(a_packed, b_packed, c_f32)

    // Integer / bitwise arithmetic
    IAdd,
    ISub,
    IMul,
    IMin,
    IMax,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    BitReverse32,   // operands: [src_u32] -> u32 (S_BREV_B32)
    BitCount32,     // operands: [src_u32] -> u32 (strictly low 32-bit popcount for S_BCNT1_I32_B32)
    AbsDiffI32,     // operands: [a_i32, b_i32] -> u32 (S_ABSDIFF_I32)
    Shl,
    Shr,
    AlignBit,       // operands: [src0_hi, src1_lo, src2_shift] -> u32 (UB-safe V_ALIGNBIT_B32)
    AlignByte,      // operands: [src0_hi, src1_lo, src2_byte_sel] -> u32 (UB-safe shift < 32)
    FmaMix,         // operands: [a, b, c, old_dst], imm_u32 = mode(0=f32,1=lo16,2=hi16) | (vop3p_flags << 4)
    BufferLoadFormat, // operands: [byte_addr_u32], imm_u32 = uav_slot | (fmt<<4) | (comp<<12) | (d16<<16) -> u32/f32

    // 32-bit bitfield ops (V_BFE_U32, V_BFE_I32, V_BFI_B32)
    BfeU32,         // operands: [src0, offset, width] -> u32
    BfeI32,         // operands: [src0, offset, width] -> i32
    BfiB32,         // operands: [mask, insert, base] -> u32

    // Packed 16-bit (2x f16 / 2x u16 / 2x b16) ops (VOP3P)
    PkAddF16,       // operands: [src0, src1], imm_u32 = packed vop3p flags -> u32
    PkMulF16,       // operands: [src0, src1], imm_u32 = packed vop3p flags -> u32
    PkFmaF16,       // operands: [src0, src1, src2], imm_u32 = packed vop3p flags -> u32
    PkAddU16,       // operands: [src0, src1], imm_u32 = packed vop3p flags -> u32
    PkShl16,        // operands: [val_src1, shift_src0], imm_u32 = packed vop3p flags -> u32
    PkShr16,        // operands: [val_src1, shift_src0], imm_u32 = packed vop3p flags -> u32

    // Conversions
    I2F,
    F2I,
    U2F,
    F2U,
    CvtPkNormI16F32,  // operands: [src0_f32, src1_f32] -> u32 packed 2x snorm16
    CvtPkNormU16F32,  // operands: [src0_f32, src1_f32] -> u32 packed 2x unorm16
    CvtNormI16F16,    // operands: [src0_u32], imm_u32 = op_sel|(neg<<8)|(abs<<12) -> u32 lo16 snorm16
    CvtNormU16F16,    // operands: [src0_u32], imm_u32 = op_sel|(neg<<8)|(abs<<12) -> u32 lo16 unorm16

    // Comparisons (produce IrType::Bool)
    CmpEq,
    CmpNe,
    CmpLt,
    CmpGt,
    CmpGe,
    CmpLtU,
    CmpEqU,
    CmpGtU,
    CmpGeU,
    CmpF32,         // operands: [a_f32, b_f32], imm_u32 = cmp_code(0..15 ordered/unordered IEEE-754) -> Bool
    CmpF16,         // operands: [a_u32, b_u32], imm_u32 = cmp_code(0..15) | (op_sel<<8) | (neg<<12) | (abs<<16) -> Bool
    LogicalAnd,     // operands: [bool_a, bool_b] -> Bool
    LogicalNot,     // operands: [bool_a] -> Bool

    // Selection & SSA merge
    Select,         // operands: [cond_bool, true_val, false_val]
    Phi,            // incoming: (pred_block_id, value_id) pairs

    // Control flow & shader outputs
    Barrier,
    Branch,         // target_block = true_block
    CondBranch,     // operands[0] = cond_bool, true_block, false_block
    Export,         // operands[0..3] = rgba/xyzw, export_target = target
    Return,
};

struct IrInstruction {
    IrValueId id = kInvalidIrValue;  // 0 for void/side-effecting ops
    IrOp op = IrOp::Return;
    IrType type = IrType::Void;

    std::vector<IrValueId> operands;
    std::vector<std::pair<uint32_t, IrValueId>> phi_incoming;  // (pred_block_id, value_id)

    uint32_t imm_u32 = 0;
    float imm_f32 = 0.0f;
    uint32_t true_block = 0;
    uint32_t false_block = 0;
    uint8_t export_target = 0;
    uint8_t export_mask = 0xFu;
    IrValueId exec_pred = kInvalidIrValue;  // Optional active-lane predicate (e.g. from V_CMPX)
};

struct IrBasicBlock {
    uint32_t id = 0;
    uint32_t start_word_offset = 0;
    std::vector<uint32_t> predecessors;
    std::vector<uint32_t> successors;
    std::vector<IrInstruction> instructions;
};

struct Rdna2IrOptStats {
    uint32_t initial_ops = 0;
    uint32_t final_ops = 0;
    uint32_t folded_constants = 0;
    uint32_t pruned_mask_ops = 0;
    uint32_t eliminated_ops = 0;
    uint32_t inserted_lockstep_lds_barriers = 0;
    uint32_t folded_execz_branches = 0;
};

// Shared IR flag constants between rdna2_ir.cpp and rdna2_lifter.cpp
constexpr uint32_t kIrLoadCbHeapFlag = 0x80000000u;
constexpr uint32_t kIrLoadSmemBoundsCheckFlag = 0x20000000u;
constexpr uint32_t kIrWaveSwizzleFlag = 0x80000000u;

// Optional SRV texture format contract for MIMG IMAGE_LOAD / IMAGE_LOAD_MIP
enum class Rdna2TextureFormat : uint8_t {
    Float4 = 0,  // RGBA32F / RGBA8_UNORM / RGBA16F
    Uint4 = 1,   // R32_UINT / RGBA32_UINT
};

// Interpolation mode contract between Pixel Shader and its feeder stage (VS / GS / MS)
enum class Rdna2InterpMode : uint8_t {
    Perspective = 0,
    NoPerspective,
    Flat,
    Centroid,
    Sample,
};

// Vertex-fetch subroutine table entry (VS prolog / S_SWAPPC_B64 mapping)
struct VertexFetchEntry {
    uint32_t table_index = 0;
    uint32_t vb_slot = 0;
    uint32_t byte_offset = 0;
    Rdna2MtbufFormat format = Rdna2MtbufFormat::R32G32B32A32_FLOAT;
    uint8_t dst_vgpr = 0;
    uint8_t num_components = 4;
};

// FSSM (Fragment/Split Shader Model) pixel-input parameter mapping entry
struct FssmPixelInputEntry {
    uint32_t param_slot = 0;
    uint32_t semantic_location = 0;
    Rdna2InterpMode interp_mode = Rdna2InterpMode::Perspective;
    uint8_t component_mask = 0xFu;
};

struct StageContractTable {
    std::vector<VertexFetchEntry> vertex_fetches;
    std::vector<FssmPixelInputEntry> pixel_inputs;
    Rdna2TextureFormat texture_srv_format = Rdna2TextureFormat::Float4;
};

struct Rdna2IrModule {
    ShaderStage stage = ShaderStage::Compute;
    uint32_t threadgroup_x = 1;
    uint32_t threadgroup_y = 1;
    uint32_t threadgroup_z = 1;
    uint32_t sgpr_count = 16;
    uint32_t vgpr_count = 16;
    uint32_t wave_size = 32;

    uint32_t next_value_id = 1;
    uint32_t max_root_const_index = 0;
    bool uses_root_constants = false;
    bool uses_uav = false;
    bool uses_uav0 = false;
    bool uses_uav1 = false;
    bool uses_lds = false;
    uint32_t lds_size_dwords = 1024;
    bool uses_scratch = false;
    uint32_t scratch_size_dwords = 64;
    bool uses_texture_srv = false;
    bool uses_texture_sample = false;
    bool uses_comparison_sample = false;
    bool uses_dynamic_sampled_indexing = false;
    uint32_t sampled_image_array_size = 0;
    bool uses_pixel_quad_derivatives = false;
    bool uses_texture_load = false;
    bool uses_texture2d_mip_srv = false;
    bool uses_texture3d_srv = false;
    bool uses_texture_cube_srv = false;
    bool uses_texture_uav = false;
    bool uses_texture_u32_uav = false;
    uint8_t texture_u32_uav_dim = 1;  // 1=2D, 2=3D, 3=Cube
    Rdna2TextureFormat texture_srv_format = Rdna2TextureFormat::Float4;
    uint32_t inserted_lockstep_lds_barriers = 0;
    bool uses_bvh_ray = false;
    bool uses_wave64_emulation = false;
    bool exports_pos0 = false;
    bool exports_mrt0 = false;
    bool exports_param0 = false;
    bool exports_prim = false;
    bool uses_mesh_cull = false;
    bool uses_mesh_global_tid = false;
    bool uses_merged_wave_info = false;
    bool mesh_has_payload = true;
    uint32_t mesh_num_threads = 1;
    uint32_t mesh_max_vertices = 64;
    uint32_t mesh_max_primitives = 64;
    StageContractTable stage_contract{};
    std::vector<Rdna2InterpMode> output_interp_modes;
    std::vector<Rdna2InterpMode> input_interp_modes;

    std::vector<IrBasicBlock> blocks;
    Rdna2IrOptStats opt_stats{};

    IrValueId AllocateValueId() { return next_value_id++; }
    const IrInstruction* FindDef(IrValueId id) const noexcept;
};

inline constexpr uint32_t PackVop3pIrFlags(
    uint8_t op_sel,
    uint8_t op_sel_hi,
    uint8_t neg_lo,
    uint8_t neg_hi,
    bool clamp) noexcept {
    return (static_cast<uint32_t>(op_sel & 7u)) |
           (static_cast<uint32_t>(op_sel_hi & 7u) << 4) |
           (static_cast<uint32_t>(neg_lo & 7u) << 8) |
           (static_cast<uint32_t>(neg_hi & 7u) << 12) |
           (clamp ? (1u << 16) : 0u);
}

inline constexpr uint32_t kDefaultVop3pIrFlags = PackVop3pIrFlags(0u, 7u, 0u, 0u, false);

const char* IrOpName(IrOp op) noexcept;
const char* IrTypeName(IrType type) noexcept;
bool IrOpHasSideEffects(IrOp op) noexcept;

// Builds a CFG + SSA IR module from decoded RDNA2 instructions.
Status BuildRdna2Ir(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& out_module,
    const StageContractTable* stage_contract = nullptr);

// Reconciles interpolation modifiers (Flat / NoPerspective / Centroid / Sample) between PS and its feeder stage.
void ReconcileStageInterpolation(Rdna2IrModule& feeder_ir, Rdna2IrModule& ps_ir) noexcept;

// Inserts explicit group barriers for 1-wave (<=64 threads) lockstep LDS hazards.
uint32_t InsertSingleWaveLockstepLdsBarriers(Rdna2IrModule& module);

// Runs Constant Folding, Dead-Code Elimination, and single-wave lockstep LDS barrier insertion.
Status OptimizeRdna2Ir(Rdna2IrModule& module, Rdna2IrOptStats* out_stats = nullptr);

// Formats the SSA IR module into human-readable text for diagnostics/debugging.
std::string DumpRdna2Ir(const Rdna2IrModule& module);

}  // namespace openscarlett
