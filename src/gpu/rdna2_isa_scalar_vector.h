#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"

namespace openscarlett::rdna2_sv {

// ---------------------------------------------------------------------------
// AMD RDNA2 / GFX10.3 Scalar & Vector ISA Modifier Definitions
// (SDWA src0==249, DPP16 src0==250, DPP8 src0==233, DPP8FI src0==234)
// ---------------------------------------------------------------------------
enum class SvModifierKind : uint8_t {
    None = 0,
    Sdwa,
    Dpp16,
    Dpp8,
    Dpp8Fi,
};

// SDWA byte/word/dword selection values (public GFX10.3 ISA specification)
inline constexpr uint8_t kSdwaSelByte0 = 0u;
inline constexpr uint8_t kSdwaSelByte1 = 1u;
inline constexpr uint8_t kSdwaSelByte2 = 2u;
inline constexpr uint8_t kSdwaSelByte3 = 3u;
inline constexpr uint8_t kSdwaSelWord0 = 4u;
inline constexpr uint8_t kSdwaSelWord1 = 5u;
inline constexpr uint8_t kSdwaSelDword = 6u;

inline constexpr uint8_t kSdwaUnusedPad = 0u;
inline constexpr uint8_t kSdwaUnusedSext = 1u;
inline constexpr uint8_t kSdwaUnusedPreserve = 2u;

// DPP16 control constants (public GFX10.3 ISA specification)
inline constexpr uint16_t kDppRowShl1 = 0x101u;
inline constexpr uint16_t kDppRowShr1 = 0x111u;
inline constexpr uint16_t kDppRowRor1 = 0x121u;
inline constexpr uint16_t kDppWaveShl1 = 0x130u;
inline constexpr uint16_t kDppWaveRol1 = 0x134u;
inline constexpr uint16_t kDppWaveShr1 = 0x138u;
inline constexpr uint16_t kDppWaveRor1 = 0x13Cu;
inline constexpr uint16_t kDppRowMirror = 0x140u;
inline constexpr uint16_t kDppRowHalfMirror = 0x141u;
inline constexpr uint16_t kDppRowBcast15 = 0x142u;
inline constexpr uint16_t kDppRowBcast31 = 0x143u;
inline constexpr uint16_t kDppRowShareBase = 0x150u;
inline constexpr uint16_t kDppRowXmaskBase = 0x160u;

inline constexpr uint16_t MakeDppQuadPerm(uint8_t l0, uint8_t l1, uint8_t l2, uint8_t l3) noexcept {
    return static_cast<uint16_t>((l0 & 3u) | ((l1 & 3u) << 2) | ((l2 & 3u) << 4) | ((l3 & 3u) << 6));
}

struct DecodedSvModifier {
    SvModifierKind kind = SvModifierKind::None;
    uint32_t raw_word = 0u;

    // Effective source-0 VGPR (or SGPR if SDWA src0_is_sgpr is set) encoded in modifier word[7:0].
    uint8_t vsrc0 = 0u;

    // SDWA (src0 == 249) fields
    uint8_t dst_sel = kSdwaSelDword;
    uint8_t dst_unused = kSdwaUnusedPad;
    bool sdwa_clamp = false;
    uint8_t sdwa_omod = 0u;
    uint8_t src0_sel = kSdwaSelDword;
    bool src0_sext = false;
    bool src0_neg = false;
    bool src0_abs = false;
    bool src0_is_sgpr = false;
    uint8_t src1_sel = kSdwaSelDword;
    bool src1_sext = false;
    bool src1_neg = false;
    bool src1_abs = false;
    bool src1_is_sgpr = false;

    // DPP16 (src0 == 250) fields
    uint16_t dpp_ctrl = 0xE4u;  // Default identity quad_perm:[0,1,2,3]
    bool dpp_fi = false;        // Fetch-inactive bit (GFX10+)
    bool bound_ctrl = false;    // 1 = zero out-of-bounds lanes; 0 = preserve old dst
    uint8_t bank_mask = 0xFu;
    uint8_t row_mask = 0xFu;

    // DPP8 / DPP8FI (src0 == 233 / 234) fields: 8x 3-bit lane selectors
    std::array<uint8_t, 8> dpp8_sel = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u};

    static DecodedSvModifier MakeSdwa(
        uint8_t src0_reg,
        uint8_t s0_sel = kSdwaSelDword,
        uint8_t s1_sel = kSdwaSelDword,
        uint8_t d_sel = kSdwaSelDword,
        uint8_t d_unused = kSdwaUnusedPad) noexcept {
        DecodedSvModifier m{};
        m.kind = SvModifierKind::Sdwa;
        m.vsrc0 = src0_reg;
        m.src0_sel = s0_sel;
        m.src1_sel = s1_sel;
        m.dst_sel = d_sel;
        m.dst_unused = d_unused;
        return m;
    }

    static DecodedSvModifier MakeDpp16(
        uint8_t src0_reg,
        uint16_t ctrl,
        uint8_t rmask = 0xFu,
        uint8_t bmask = 0xFu,
        bool bctrl = true,
        bool fi = false) noexcept {
        DecodedSvModifier m{};
        m.kind = SvModifierKind::Dpp16;
        m.vsrc0 = src0_reg;
        m.dpp_ctrl = ctrl;
        m.row_mask = rmask;
        m.bank_mask = bmask;
        m.bound_ctrl = bctrl;
        m.dpp_fi = fi;
        return m;
    }

    static DecodedSvModifier MakeDpp8(
        uint8_t src0_reg,
        const std::array<uint8_t, 8>& sel,
        bool fetch_inactive = false) noexcept {
        DecodedSvModifier m{};
        m.kind = fetch_inactive ? SvModifierKind::Dpp8Fi : SvModifierKind::Dpp8;
        m.vsrc0 = src0_reg;
        m.dpp8_sel = sel;
        m.dpp_fi = fetch_inactive;
        return m;
    }
};

// ---------------------------------------------------------------------------
// Semantic Classification for GFX10.3 Scalar & Vector Instructions
// ---------------------------------------------------------------------------
enum class SvSemanticClass : uint16_t {
    Unknown = 0,
    ScalarArith = 1,
    ScalarLogic = 2,
    ScalarShiftBit = 3,
    ScalarCompare = 4,
    ScalarControl = 5,
    ScalarExecMask = 6,
    ScalarMemory = 7,
    ScalarAtomic = 8,
    ScalarSystem = 9,
    VectorMove = 10,
    VectorConvert = 11,
    VectorFloatArith = 12,
    VectorTranscendental = 13,
    VectorIntArith = 14,
    VectorLogicShiftBit = 15,
    VectorCompare = 16,
    VectorFmaMad = 17,
    VectorPacked16 = 18,
    VectorDotProduct = 19,
    VectorCrossLane = 20,
    VectorInterp = 21,
    VectorSadCube = 22,
};

// ---------------------------------------------------------------------------
// Comprehensive GFX10.3 Scalar & Vector Opcode Descriptor
// ---------------------------------------------------------------------------
struct ScalarVectorOpcodeEntry {
    Rdna2Encoding encoding;
    uint16_t raw_opcode;
    const char* mnemonic;
    uint8_t src_count;
    bool has_dst;
    bool dst_is_sgpr;
    bool dst_is_64bit;
    bool writes_scc;
    bool writes_vcc;
    bool writes_exec;
    bool has_mandatory_literal;
    uint8_t bit_width;
    Rdna2DataKind data_kind;
    uint16_t semantic_class;
    bool supports_spirv_lift;
    bool supports_dxil_lift;
    bool supports_cpu_ref;
    bool supports_gpu_exec;
};

struct SvDecodedInst {
    Rdna2Instruction base;
    const ScalarVectorOpcodeEntry* entry = nullptr;
    DecodedSvModifier modifier{};
};

// ---------------------------------------------------------------------------
// Table & Lookup API
// ---------------------------------------------------------------------------
const std::vector<ScalarVectorOpcodeEntry>& GetScalarVectorOpcodeTable() noexcept;

const ScalarVectorOpcodeEntry* LookupScalarVectorOpcode(
    Rdna2Encoding enc,
    uint16_t raw_opcode) noexcept;

const ScalarVectorOpcodeEntry* LookupScalarVectorByName(const char* mnemonic) noexcept;

size_t GetUniqueScalarVectorMnemonicCount() noexcept;

// ---------------------------------------------------------------------------
// Binary Decode & Encode API (SOP1/SOP2/SOPK/SOPC/SOPP/SMEM/VOP1/VOP2/VOPC/VOP3/VOP3P/VINTRP + SDWA/DPP16/DPP8)
// ---------------------------------------------------------------------------
Status DecodeScalarVectorAt(
    const uint32_t* words,
    size_t word_count,
    size_t pc,
    SvDecodedInst& out_inst);

Status DecodeScalarVectorStream(
    const std::vector<uint32_t>& words,
    std::vector<SvDecodedInst>& out_insts);

bool EncodeScalarVectorInst(
    const char* mnemonic,
    uint8_t dst,
    const std::vector<Rdna2Operand>& srcs,
    int32_t imm_or_offset,
    std::vector<uint32_t>& out_words,
    const DecodedSvModifier* mod = nullptr);

// ---------------------------------------------------------------------------
// Wave-Level CPU Reference State & Executor (Wave32 & Wave64)
// ---------------------------------------------------------------------------
struct SvWaveState {
    std::array<uint32_t, 106> sgpr{};
    std::array<std::array<uint32_t, 64>, 64> vgpr{};  // vgpr[lane][reg]
    bool scc = false;
    uint64_t vcc = 0u;
    uint64_t exec = 0xFFFFFFFFull;
    uint32_t m0 = 0u;
    uint32_t pc = 0u;
    uint32_t wave_size = 32u;

    std::vector<uint32_t> root_constants;
    std::vector<uint32_t> uav0;
    std::vector<uint32_t> uav1;

    // Auxiliary wave hardware registers (mode, status, trapsts, etc.) & telemetry
    std::array<uint32_t, 32> hwreg{};
    uint64_t instructions_executed = 0u;
    uint32_t max_steps = 100000u;
    bool ended = false;
    bool halted = false;

    void Reset(uint32_t wave_sz = 32u) noexcept {
        sgpr.fill(0u);
        for (auto& lane_regs : vgpr) {
            lane_regs.fill(0u);
        }
        hwreg.fill(0u);
        scc = false;
        vcc = 0u;
        wave_size = (wave_sz == 64u) ? 64u : 32u;
        exec = (wave_size == 64u) ? 0xFFFFFFFFFFFFFFFFull : 0x00000000FFFFFFFFull;
        m0 = 0u;
        pc = 0u;
        instructions_executed = 0u;
        ended = false;
        halted = false;
    }

    uint64_t WaveMask() const noexcept {
        return (wave_size == 64u) ? 0xFFFFFFFFFFFFFFFFull : 0x00000000FFFFFFFFull;
    }

    uint64_t ActiveMask() const noexcept {
        return exec & WaveMask();
    }

    bool IsLaneActive(uint32_t lane) const noexcept {
        if (lane >= wave_size || lane >= 64u) return false;
        return ((ActiveMask() >> lane) & 1ull) != 0ull;
    }

    uint32_t ReadSgpr(uint8_t idx) const noexcept {
        if (idx < sgpr.size()) return sgpr[idx];
        if (idx == 106u) return static_cast<uint32_t>(vcc & 0xFFFFFFFFull);
        if (idx == 107u) return static_cast<uint32_t>(vcc >> 32);
        if (idx == 124u) return m0;
        if (idx == 126u) return static_cast<uint32_t>(exec & 0xFFFFFFFFull);
        if (idx == 127u) return static_cast<uint32_t>(exec >> 32);
        return 0u;
    }

    void WriteSgpr(uint8_t idx, uint32_t val) noexcept {
        if (idx < sgpr.size()) {
            sgpr[idx] = val;
        } else if (idx == 106u) {
            vcc = (vcc & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(val);
        } else if (idx == 107u) {
            vcc = (vcc & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
        } else if (idx == 124u) {
            m0 = val;
        } else if (idx == 126u) {
            exec = (exec & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(val);
        } else if (idx == 127u) {
            exec = (exec & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
        }
    }

    uint64_t ReadSgpr64(uint8_t idx) const noexcept {
        const uint64_t lo = static_cast<uint64_t>(ReadSgpr(idx));
        const uint64_t hi = static_cast<uint64_t>(ReadSgpr(static_cast<uint8_t>(idx + 1u)));
        return lo | (hi << 32);
    }

    void WriteSgpr64(uint8_t idx, uint64_t val) noexcept {
        WriteSgpr(idx, static_cast<uint32_t>(val & 0xFFFFFFFFull));
        WriteSgpr(static_cast<uint8_t>(idx + 1u), static_cast<uint32_t>(val >> 32));
    }

    uint32_t ReadVgpr(uint32_t lane, uint8_t idx) const noexcept {
        if (lane >= 64u || idx >= 64u) return 0u;
        return vgpr[lane][idx];
    }

    void WriteVgpr(uint32_t lane, uint8_t idx, uint32_t val) noexcept {
        if (lane >= 64u || idx >= 64u) return;
        vgpr[lane][idx] = val;
    }

    float ReadVgprF32(uint32_t lane, uint8_t idx) const noexcept {
        const uint32_t bits = ReadVgpr(lane, idx);
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    void WriteVgprF32(uint32_t lane, uint8_t idx, float f) noexcept {
        uint32_t bits = 0u;
        std::memcpy(&bits, &f, sizeof(bits));
        WriteVgpr(lane, idx, bits);
    }

    uint64_t ReadVgpr64(uint32_t lane, uint8_t idx) const noexcept {
        const uint64_t lo = static_cast<uint64_t>(ReadVgpr(lane, idx));
        const uint64_t hi = static_cast<uint64_t>(ReadVgpr(lane, static_cast<uint8_t>(idx + 1u)));
        return lo | (hi << 32);
    }

    void WriteVgpr64(uint32_t lane, uint8_t idx, uint64_t val) noexcept {
        WriteVgpr(lane, idx, static_cast<uint32_t>(val & 0xFFFFFFFFull));
        WriteVgpr(lane, static_cast<uint8_t>(idx + 1u), static_cast<uint32_t>(val >> 32));
    }

    void SeedThreadIds(uint32_t base_tid = 0u) noexcept {
        const uint32_t n = (wave_size == 64u) ? 64u : 32u;
        for (uint32_t l = 0u; l < n; ++l) {
            vgpr[l][0] = base_tid + l;
        }
    }
};

Status ExecuteScalarVectorCpu(
    const std::vector<SvDecodedInst>& insts,
    SvWaveState& wave);

// ---------------------------------------------------------------------------
// Scalar + Vector Lifter (Emits SPIR-V 1.3 + SM 6.5 HLSL / DXIL)
// ---------------------------------------------------------------------------
Status LiftScalarVectorShader(
    const Xs2ShaderContainer& container,
    const std::vector<SvDecodedInst>& insts,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options = {});

}  // namespace openscarlett::rdna2_sv
